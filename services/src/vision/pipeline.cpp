#include "bawal/vision/pipeline.hpp"
#include "bawal/vision/simd_food_evaluator.hpp"
#include "bawal/ml/ncnn_generator.hpp"
#include <chrono>
#include <thread>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <opencv2/imgproc.hpp>

namespace bawal::vision {

bool InspectionPipeline::initialize(std::string_view model_path, int target_width, int target_height) {
    m_width = target_width;
    m_height = target_height;

    auto calib = CameraCalibrationParams::createDefaultOV5640(cv::Size(m_width, m_height));
    if (!m_dewarper.initCameraCalibration(calib.camera_matrix, calib.dist_coeffs, calib.image_size)) {
        std::cerr << "[Pipeline] Warning: Camera calibration initialization failed.\n";
    }

    const int num_threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 2, 4);
    if (!model_path.empty()) {
        if (m_classifier.init(model_path, num_threads)) {
            std::cout << "[Pipeline] Model loaded successfully: " << model_path 
                      << " (Inference threads: " << num_threads << ")\n";
        } else {
            std::cout << "[Pipeline] Model path fallback triggered.\n";
        }
    } else {
        (void)m_classifier.init("", num_threads);
    }

    // Initialize persistent CLAHE engine to avoid per-frame allocation
    m_clahe = cv::createCLAHE(1.8, cv::Size(8, 8));

    // Ensure NCNN Super-Resolution weights exist; generate via bawal::ml::NCNNGenerator if missing
    const std::filesystem::path default_weight_dir = "/app/weights";
    const std::filesystem::path local_weight_dir = "weights";
    const std::filesystem::path weight_dir = std::filesystem::exists(default_weight_dir) ? default_weight_dir : local_weight_dir;
    const std::filesystem::path param_file = weight_dir / "realesr-animevideov3-x2.param";
    const std::filesystem::path bin_file = weight_dir / "realesr-animevideov3-x2.bin";

    if (!std::filesystem::exists(param_file) || !std::filesystem::exists(bin_file)) {
        std::cout << "[Pipeline] Synthesizing NCNN Super-Resolution model via bawal::ml::NCNNGenerator...\n";
        bawal::ml::NCNNModelConfig gen_cfg;
        gen_cfg.model_name = "realesr-animevideov3-x2";
        gen_cfg.scale = 2;
        gen_cfg.output_dir = weight_dir;
        gen_cfg.sync_dirs = {"/app/weights", "services/weights", "weights"};
        bawal::ml::NCNNGenerator::generate(gen_cfg);
    }

    // Initialize Real-ESRGAN NCNN Super-Resolution Engine
    RealESRGANConfig enhancer_cfg;
    enhancer_cfg.model_dir = weight_dir;
    enhancer_cfg.model_name = "realesr-animevideov3-x2";
    enhancer_cfg.params.scale = 2;
    enhancer_cfg.params.tile_size = 192;
    enhancer_cfg.params.prepadding = 10;
    enhancer_cfg.params.use_gpu = 1;
    enhancer_cfg.params.use_fp16 = 1;
    if (m_enhancer.init(enhancer_cfg)) {
        std::cout << "[Pipeline] Real-ESRGAN Super-Resolution Engine initialized successfully.\n";
    }

    return true;
}

bool InspectionPipeline::execute(const uint8_t* raw_bytes, size_t len, InspectionOutput& out_inspection) {
    if (!raw_bytes || len == 0) {
        return false;
    }

    DecodedFrame decoded;
    if (!m_receiver.decodeMJPEG(raw_bytes, len, decoded) || decoded.image.empty()) {
        return false;
    }

    out_inspection.frame_id = decoded.frame_id;
    return execute(decoded.image, out_inspection);
}

bool InspectionPipeline::execute(const std::vector<uint8_t>& raw_frame_bytes, InspectionOutput& out_inspection) {
    return execute(raw_frame_bytes.data(), raw_frame_bytes.size(), out_inspection);
}

void InspectionPipeline::enhanceImage(const cv::Mat& src, cv::Mat& dst) {
    if (src.empty()) {
        dst = src;
        return;
    }

    // 1. Gray-World Auto White Balance (Corrects sensor tint / pink-purple indoor color cast)
    cv::Scalar mean_bgr = cv::mean(src);
    const double gray_target = (mean_bgr[0] + mean_bgr[1] + mean_bgr[2]) / 3.0;

    const float gain_b = static_cast<float>(std::clamp((mean_bgr[0] > 1.0) ? (gray_target / mean_bgr[0]) : 1.0, 0.75, 1.35));
    const float gain_g = static_cast<float>(std::clamp((mean_bgr[1] > 1.0) ? (gray_target / mean_bgr[1]) : 1.0, 0.75, 1.35));
    const float gain_r = static_cast<float>(std::clamp((mean_bgr[2] > 1.0) ? (gray_target / mean_bgr[2]) : 1.0, 0.75, 1.35));

    cv::Mat balanced;
    std::vector<cv::Mat> channels(3);
    cv::split(src, channels);
    channels[0].convertTo(channels[0], -1, gain_b, 0);
    channels[1].convertTo(channels[1], -1, gain_g, 0);
    channels[2].convertTo(channels[2], -1, gain_r, 0);
    cv::merge(channels, balanced);

    // 2. Adaptive Local Contrast (CLAHE on L*) & Food Vibrancy Boost (a*, b*)
    cv::Mat lab;
    cv::cvtColor(balanced, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> lab_planes(3);
    cv::split(lab, lab_planes);

    if (!m_clahe) {
        m_clahe = cv::createCLAHE(1.8, cv::Size(8, 8));
    }
    m_clahe->apply(lab_planes[0], lab_planes[0]);

    // 1.15x vibrancy enhancement on chroma planes (a*, b*) centered at 128
    // Elevates food colors (rice, chicken, egg, vegetables) without oversaturation
    lab_planes[1].convertTo(lab_planes[1], -1, 1.15f, (1.0f - 1.15f) * 128.0f);
    lab_planes[2].convertTo(lab_planes[2], -1, 1.15f, (1.0f - 1.15f) * 128.0f);

    cv::merge(lab_planes, lab);
    cv::Mat contrast_enhanced;
    cv::cvtColor(lab, contrast_enhanced, cv::COLOR_Lab2BGR);

    // 3. Fast Edge-Preserving Sharpening (Unsharp Masking)
    cv::Mat low_freq;
    cv::GaussianBlur(contrast_enhanced, low_freq, cv::Size(0, 0), 1.6);

    // 1.25x original - 0.25x blur removes haze and boosts micro-texture without ringing halos
    cv::addWeighted(contrast_enhanced, 1.25, low_freq, -0.25, 0, dst);
}

bool InspectionPipeline::processStream(const cv::Mat& frame, 
                                       const std::vector<DetectionResult>& active_detections, 
                                       InspectionOutput& out_inspection) {
    if (frame.empty()) return false;

    const int64_t t_start = cv::getTickCount();
    out_inspection.raw_image = frame;
    out_inspection.is_valid = false;

    // 1. Lens Dewarping Calibration
    cv::Mat dewarped;
    if (!m_dewarper.process(frame, dewarped)) {
        dewarped = frame.clone();
    }

    // 2. Adaptive ISP Enhancement
    cv::Mat enhanced;
    enhanceImage(dewarped, enhanced);
    out_inspection.dewarped_image = enhanced;

    // 3. Attach current active detections from AI worker
    out_inspection.detections = active_detections;

    // 4. Render HUD Overlay on Enhanced Frame
    renderOverlay(out_inspection, out_inspection.annotated_image);

    const int64_t t_end = cv::getTickCount();
    out_inspection.processing_time_ms = ((t_end - t_start) * 1000.0) / cv::getTickFrequency();
    out_inspection.is_valid = true;
    return true;
}

bool InspectionPipeline::runInference(const cv::Mat& enhanced_frame, 
                                      std::vector<DetectionResult>& out_detections) {
    if (enhanced_frame.empty()) return false;
    out_detections.clear();

    if (!m_classifier.detect(enhanced_frame, out_detections, 0.45f, 0.45f)) {
        return false;
    }

    // Super-Resolution Enhancer & Targeted Spoilage Evaluation directly on food crops
    for (auto& det : out_detections) {
        if (det.class_id < 0 || det.class_name == "no_food") continue;
        cv::Rect crop_rect = det.bbox & cv::Rect(0, 0, enhanced_frame.cols, enhanced_frame.rows);
        if (crop_rect.width >= 16 && crop_rect.height >= 16) {
            cv::Mat food_crop = enhanced_frame(crop_rect);
            cv::Mat sr_crop;
            if (m_enhancer.process(food_crop, sr_crop) && !sr_crop.empty()) {
                det.spoilage = evaluateSpoilageAVX2(sr_crop, det.class_name);
            } else {
                det.spoilage = evaluateSpoilageAVX2(food_crop, det.class_name);
            }
        }
    }
    return true;
}

bool InspectionPipeline::execute(const cv::Mat& frame, InspectionOutput& out_inspection) {
    if (frame.empty()) return false;

    std::vector<DetectionResult> detected;
    // Process stream first for visual base
    if (!processStream(frame, {}, out_inspection)) {
        return false;
    }

    // Synchronous execution path: run inference on enhanced frame
    if (runInference(out_inspection.dewarped_image, detected)) {
        out_inspection.detections = detected;
        renderOverlay(out_inspection, out_inspection.annotated_image);
    }
    return true;
}

void InspectionPipeline::renderOverlay(const InspectionOutput& inspection, cv::Mat& out_display) {
    if (inspection.dewarped_image.empty()) {
        return;
    }

    out_display = inspection.dewarped_image.clone();

    for (const auto& det : inspection.detections) {
        if (det.class_id < 0 || det.confidence < 0.48f || det.class_name == "no_food" || det.class_name == "no_model") {
            continue;
        }

        const float spoilage_pct = std::clamp(det.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);
        const float freshness_score = std::clamp(100.0f - spoilage_pct, 0.0f, 100.0f);

        std::string status_badge = "PASS";
        cv::Scalar box_color = cv::Scalar(40, 200, 40); // Clean Green (PASS)

        if (det.spoilage.is_visually_spoiled || spoilage_pct >= 50.0f || det.confidence < 0.52f) {
            status_badge = "REJECT";
            box_color = cv::Scalar(30, 30, 235); // Crimson Red (REJECT)
        } else if (spoilage_pct >= 25.0f || det.confidence < 0.65f) {
            status_badge = "REVIEW";
            box_color = cv::Scalar(0, 165, 255); // Amber Orange (REVIEW)
        }

        cv::Rect clamped_box = det.bbox & cv::Rect(0, 0, out_display.cols, out_display.rows);
        if (clamped_box.width <= 0 || clamped_box.height <= 0) {
            continue;
        }

        // Draw bounding box
        cv::rectangle(out_display, clamped_box, box_color, 2);

        // Build item label: e.g. "rice 74% [PASS 96%]"
        std::ostringstream ss;
        ss << det.class_name << " " << std::fixed << std::setprecision(0) << (det.confidence * 100.0f) << "% ["
           << status_badge << " " << std::fixed << std::setprecision(0) << freshness_score << "%]";
        std::string label = ss.str();

        int baseline = 0;
        cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.44, 1, &baseline);
        int badge_y = std::max(clamped_box.y - 4, text_size.height + 4);
        cv::Point badge_tl(clamped_box.x, badge_y - text_size.height - 4);
        cv::Point badge_br(clamped_box.x + text_size.width + 8, badge_y + 2);
        
        cv::rectangle(out_display, badge_tl, badge_br, box_color, cv::FILLED);
        cv::putText(out_display, label, cv::Point(clamped_box.x + 4, badge_y - 2),
                    cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(0, 0, 0), 1, cv::LINE_AA);
    }
}

} // namespace bawal::vision