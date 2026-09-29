#include "bawal/vision/pipeline.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <opencv2/imgproc.hpp>

namespace bawal::vision {

bool InspectionPipeline::initialize(const std::string& model_path, int target_width, int target_height) {
    m_width = target_width;
    m_height = target_height;

    auto calib = CameraCalibrationParams::createDefaultOV5640(cv::Size(m_width, m_height));
    if (!m_dewarper.initCameraCalibration(calib.camera_matrix, calib.dist_coeffs, calib.image_size)) {
        std::cerr << "[Pipeline] Warning: Camera calibration initialization failed.\n";
    }

    if (!m_segmenter.initStandardTrayLayout(m_width, m_height)) {
        std::cerr << "[Pipeline] Failed to initialize tray compartment layout.\n";
        return false;
    }

    // 3. Initialize AI1 Inference Model (Optional, safe fallback if model file is not yet exported)
    if (!model_path.empty()) {
        if (m_classifier.init(model_path, 2)) {
            std::cout << "[Pipeline] AI1 ONNX Runtime model loaded: " << model_path << "\n";
        } else {
            std::cout << "[Pipeline] Model file not ready. Running in heuristic fallback mode.\n";
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

    // 2. Tray Dynamic Food Segmentation
    out_inspection.compartments.clear();
    if (!m_segmenter.extractCompartments(dewarped, out_inspection.compartments)) {
        return false;
    }

    // 3. Compartment Classification (AI1 Inference)
    const int num_compartments = static_cast<int>(out_inspection.compartments.size());
    out_inspection.classifications.clear();
    out_inspection.classifications.reserve(num_compartments);

    for (int i = 0; i < num_compartments; ++i) {
        const auto& comp = out_inspection.compartments[i];
        ClassificationResult res;
        if (!comp.cropped_mat.empty() && m_classifier.predict(comp.cropped_mat, res)) {
            out_inspection.classifications.push_back(std::move(res));
        } else {
            res.class_id = -1;
            res.class_name = "no_food";
            res.confidence = 0.0f;
            out_inspection.classifications.push_back(std::move(res));
        }
    }

    const int64_t t_end = cv::getTickCount();
    out_inspection.processing_time_ms = ((t_end - t_start) * 1000.0) / cv::getTickFrequency();
    out_inspection.is_valid = true;

    // 4. Render Visual Annotation Overlay
    renderOverlay(out_inspection, out_inspection.annotated_image);

    return out_inspection.is_valid;
}

void InspectionPipeline::renderOverlay(const InspectionOutput& inspection, cv::Mat& out_display) {
    if (inspection.dewarped_image.empty()) {
        return;
    }

    out_display = inspection.dewarped_image.clone();

    // Color palette for meal compartments / food categories
    const cv::Scalar colors[] = {
        cv::Scalar(0, 255, 255),  // Yellow - Rice / Carb
        cv::Scalar(0, 255, 0),    // Green - Vegetables
        cv::Scalar(0, 140, 255),  // Orange - Protein / Meat
        cv::Scalar(255, 100, 255), // Magenta - Fruit
        cv::Scalar(255, 200, 0),  // Cyan - Side
        cv::Scalar(100, 255, 255)  // Light Yellow
    };

    int detected_food_count = 0;

    for (size_t i = 0; i < inspection.compartments.size(); ++i) {
        const auto& comp = inspection.compartments[i];
        bool has_food = false;
        std::string label = comp.name;
        cv::Scalar color = colors[i % 6];

        if (i < inspection.classifications.size()) {
            const auto& cls = inspection.classifications[i];
            if (cls.class_id >= 0 && cls.confidence >= 0.35f && cls.class_name != "no_food" && cls.class_name != "no_model") {
                has_food = true;
                detected_food_count++;
                std::ostringstream ss;
                ss << cls.class_name << " (" << std::fixed << std::setprecision(1) << (cls.confidence * 100.0f) << "%)";
                label = ss.str();
            } else if (cls.class_name == "no_model") {
                label = "Model Not Loaded";
                color = cv::Scalar(80, 80, 80);
            } else {
                continue; // Skip drawing boxes for unconfident background noise
            }
        }

        if (!has_food && inspection.classifications[i].class_name != "no_model") {
            continue;
        }

        // Draw bounding box
        cv::rectangle(out_display, comp.roi, color, 2);

        // Render label background badge
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.48, 1, &baseline);
        int badge_y = std::max(comp.roi.y - 4, text_size.height + 4);
        cv::Point badge_tl(comp.roi.x, badge_y - text_size.height - 4);
        cv::Point badge_br(comp.roi.x + text_size.width + 8, badge_y + 2);
        
        cv::rectangle(out_display, badge_tl, badge_br, color, cv::FILLED);
        cv::putText(out_display, label, cv::Point(comp.roi.x + 4, badge_y - 2),
                    cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 0, 0), 1, cv::LINE_AA);
    }

    // Top HUD Status Bar
    cv::rectangle(out_display, cv::Point(0, 0), cv::Point(out_display.cols, 28), cv::Scalar(20, 20, 20), cv::FILLED);
    std::ostringstream hud_ss;
    hud_ss << "BawalVision Dynamic CV AI1 Pipeline | Frame #" << inspection.frame_id 
           << " | Latency: " << std::fixed << std::setprecision(1) << inspection.processing_time_ms << " ms"
           << " | Food Objects: " << detected_food_count;
    
    cv::putText(out_display, hud_ss.str(), cv::Point(10, 19),
                cv::FONT_HERSHEY_SIMPLEX, 0.48, cv::Scalar(0, 255, 180), 1, cv::LINE_AA);
}

} // namespace bawal::vision