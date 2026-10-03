#include "bawal/vision/pipeline.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <opencv2/imgproc.hpp>

namespace bawal::vision {

bool InspectionPipeline::initialize(std::string_view model_path, int target_width, int target_height) {
    m_width = target_width;
    m_height = target_height;

    auto calib = CameraCalibrationParams::createDefaultOV5640(cv::Size(m_width, m_height));
    if (!m_dewarper.initCameraCalibration(calib.camera_matrix, calib.dist_coeffs, calib.image_size)) {
        std::cerr << "[Pipeline] Warning: Camera calibration initialization failed.\n";
    }

    // Initialize YOLOv8 Inference Model & SIMD Evaluator
    if (!model_path.empty()) {
        if (m_classifier.init(model_path, 2)) {
            std::cout << "[Pipeline] AI1 YOLOv8 ONNX model loaded: " << model_path << "\n";
        } else {
            std::cout << "[Pipeline] Model file not ready. Running in fallback mode.\n";
        }
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

bool InspectionPipeline::execute(const cv::Mat& frame, InspectionOutput& out_inspection) {
    if (frame.empty()) {
        return false;
    }

    const int64_t t_start = cv::getTickCount();

    out_inspection.raw_image = frame;
    out_inspection.is_valid = false;

    // 1. Lens Distortion Correction & Perspective Dewarping
    cv::Mat dewarped;
    if (!m_dewarper.process(frame, dewarped)) {
        dewarped = frame.clone();
    }
    out_inspection.dewarped_image = dewarped;

    // 2. Dynamic Food Detection (YOLOv8) + SIMD Quality/Freshness Evaluation
    out_inspection.detections.clear();
    [[maybe_unused]] bool det_ok = m_classifier.detect(dewarped, out_inspection.detections, 0.25f, 0.45f);

    const int64_t t_end = cv::getTickCount();
    out_inspection.processing_time_ms = ((t_end - t_start) * 1000.0) / cv::getTickFrequency();
    out_inspection.is_valid = true;

    // 3. Render Visual Annotation Overlay
    renderOverlay(out_inspection, out_inspection.annotated_image);

    return out_inspection.is_valid;
}

void InspectionPipeline::renderOverlay(const InspectionOutput& inspection, cv::Mat& out_display) {
    if (inspection.dewarped_image.empty()) {
        return;
    }

    out_display = inspection.dewarped_image.clone();

    int detected_food_count = 0;

    for (const auto& det : inspection.detections) {
        if (det.class_id < 0 || det.confidence < 0.25f || det.class_name == "no_model") {
            continue;
        }

        detected_food_count++;

        const float freshness_score = std::clamp(1.0f - det.spoilage.discoloration_ratio, 0.0f, 1.0f);
        const std::string freshness_status = det.spoilage.is_visually_spoiled ? "SPOILED" : "FRESH";

        // Determine color based on freshness / spoilage
        cv::Scalar box_color;
        if (det.spoilage.is_visually_spoiled) {
            box_color = cv::Scalar(0, 0, 255); // Red (Spoiled)
        } else if (freshness_score < 0.70f) {
            box_color = cv::Scalar(0, 165, 255); // Amber/Orange (Warning/Degraded)
        } else {
            box_color = cv::Scalar(0, 255, 0); // Green (Fresh)
        }

        // Ensure bounding box stays within image bounds
        cv::Rect clamped_box = det.bbox & cv::Rect(0, 0, out_display.cols, out_display.rows);
        if (clamped_box.width <= 0 || clamped_box.height <= 0) {
            continue;
        }

        // Draw bounding box
        cv::rectangle(out_display, clamped_box, box_color, 2);

        // Construct label text
        std::ostringstream ss;
        ss << det.class_name << " " << std::fixed << std::setprecision(0) << (det.confidence * 100.0f) << "% ["
           << freshness_status << " " << std::fixed << std::setprecision(0) << (freshness_score * 100.0f) << "%]";
        std::string label = ss.str();

        // Render label background badge
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.44, 1, &baseline);
        int badge_y = std::max(clamped_box.y - 4, text_size.height + 4);
        cv::Point badge_tl(clamped_box.x, badge_y - text_size.height - 4);
        cv::Point badge_br(clamped_box.x + text_size.width + 8, badge_y + 2);
        
        cv::rectangle(out_display, badge_tl, badge_br, box_color, cv::FILLED);
        cv::putText(out_display, label, cv::Point(clamped_box.x + 4, badge_y - 2),
                    cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(0, 0, 0), 1, cv::LINE_AA);
    }

    // Top HUD Status Bar
    cv::rectangle(out_display, cv::Point(0, 0), cv::Point(out_display.cols, 28), cv::Scalar(20, 20, 20), cv::FILLED);
    std::ostringstream hud_ss;
    hud_ss << "BawalVision YOLOv8 + SIMD Pipeline | Frame #" << inspection.frame_id 
           << " | Latency: " << std::fixed << std::setprecision(1) << inspection.processing_time_ms << " ms"
           << " | Food Objects: " << detected_food_count;
    
    cv::putText(out_display, hud_ss.str(), cv::Point(10, 19),
                cv::FONT_HERSHEY_SIMPLEX, 0.48, cv::Scalar(0, 255, 180), 1, cv::LINE_AA);
}

} // namespace bawal::vision