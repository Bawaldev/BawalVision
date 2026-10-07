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
        m_classifier.init("", 2);
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

    // 1. Koreksi lensa dan distorsi
    cv::Mat dewarped;
    if (!m_dewarper.process(frame, dewarped)) {
        dewarped = frame.clone();
    }
    out_inspection.dewarped_image = dewarped;

    // 2. Deteksi makanan YOLOv8 + Evaluasi degradasi warna SIMD (Threshold 0.40f)
    out_inspection.detections.clear();
    [[maybe_unused]] bool det_ok = m_classifier.detect(dewarped, out_inspection.detections, 0.40f, 0.45f);

    const int64_t t_end = cv::getTickCount();
    out_inspection.processing_time_ms = ((t_end - t_start) * 1000.0) / cv::getTickFrequency();
    out_inspection.is_valid = true;

    // 3. Render visual overlay box
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
        if (det.class_id < 0 || det.confidence < 0.05f || det.class_name == "no_model") {
            continue;
        }

        detected_food_count++;

        const float freshness_score = std::clamp(1.0f - det.spoilage.discoloration_ratio, 0.0f, 1.0f);
        const std::string freshness_status = det.spoilage.is_visually_spoiled ? "SPOILED" : "FRESH";

        cv::Scalar box_color;
        if (det.spoilage.is_visually_spoiled) {
            box_color = cv::Scalar(0, 0, 255); // Merah
        } else if (freshness_score < 0.70f) {
            box_color = cv::Scalar(0, 165, 255); // Oranye
        } else {
            box_color = cv::Scalar(0, 255, 0); // Hijau
        }

        cv::Rect clamped_box = det.bbox & cv::Rect(0, 0, out_display.cols, out_display.rows);
        if (clamped_box.width <= 0 || clamped_box.height <= 0) {
            continue;
        }

        cv::rectangle(out_display, clamped_box, box_color, 2);

        std::ostringstream ss;
        ss << det.class_name << " " << std::fixed << std::setprecision(0) << (det.confidence * 100.0f) << "% ["
           << freshness_status << " " << std::fixed << std::setprecision(0) << (freshness_score * 100.0f) << "%]";
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