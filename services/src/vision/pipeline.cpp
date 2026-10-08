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

    if (!model_path.empty()) {
        if (m_classifier.init(model_path, 2)) {
            std::cout << "[Pipeline] Model loaded successfully: " << model_path << "\n";
        } else {
            std::cout << "[Pipeline] Model path fallback triggered.\n";
        }
    } else {
        (void)m_classifier.init("", 2);
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

    // 1. Bilateral Filter: Edge-preserving smoothing eliminates low-light sensor noise/speckles without blurring food boundaries
    cv::Mat denoised;
    cv::bilateralFilter(src, denoised, 5, 25.0, 25.0);

    // 2. CLAHE: Contrast Limited Adaptive Histogram Equalization on Luminance (L* channel in CIE-Lab)
    // Enhances local contrast in shadowed/dim areas without blowing out highlights
    cv::Mat lab;
    cv::cvtColor(denoised, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> lab_planes;
    cv::split(lab, lab_planes);

    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(lab_planes[0], lab_planes[0]);

    cv::merge(lab_planes, lab);
    cv::Mat clahe_bgr;
    cv::cvtColor(lab, clahe_bgr, cv::COLOR_Lab2BGR);

    // 3. Unsharp Masking: High-boost filter restores crisp micro-textures and reduces optical lens blur
    cv::Mat blurred;
    cv::GaussianBlur(clahe_bgr, blurred, cv::Size(0, 0), 3.0);
    cv::addWeighted(clahe_bgr, 1.4, blurred, -0.4, 0, dst);
}

bool InspectionPipeline::execute(const cv::Mat& frame, InspectionOutput& out_inspection) {
    if (frame.empty()) {
        return false;
    }

    const int64_t t_start = cv::getTickCount();

    out_inspection.raw_image = frame;
    out_inspection.is_valid = false;

    // 1. Lens undistortion and perspective correction
    cv::Mat dewarped;
    if (!m_dewarper.process(frame, dewarped)) {
        dewarped = frame.clone();
    }

    // 2. Image Preprocessing & Enhancement (Bilateral Denoise + CLAHE + Unsharp Masking)
    cv::Mat enhanced;
    enhanceImage(dewarped, enhanced);
    out_inspection.dewarped_image = enhanced;

    // 3. YOLOv8 multi-food detection and SIMD freshness evaluation on enhanced frame
    out_inspection.detections.clear();
    [[maybe_unused]] bool det_ok = m_classifier.detect(enhanced, out_inspection.detections, 0.50f, 0.45f);

    const int64_t t_end = cv::getTickCount();
    out_inspection.processing_time_ms = ((t_end - t_start) * 1000.0) / cv::getTickFrequency();
    out_inspection.is_valid = true;

    // 4. Render visual HUD and detection bounding boxes
    renderOverlay(out_inspection, out_inspection.annotated_image);

    return out_inspection.is_valid;
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