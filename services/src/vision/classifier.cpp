#include "bawal/vision/classifier.hpp"
#include "bawal/vision/simd_food_evaluator.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <opencv2/imgproc.hpp>

namespace bawal::vision {

FoodClassifier::FoodClassifier() 
    : m_class_names{
        "banana",
        "chicken",
        "egg",
        "orange",
        "rice",
        "spinach",
        "tempeh",
        "tofu",
        "water_spinach"
    } {
}

bool FoodClassifier::init(std::string_view model_path, int thread_count) {
    (void)thread_count;
    std::lock_guard<std::mutex> lock(m_infer_mutex);
    m_is_ready = false;

    std::vector<std::string> candidate_paths;
    if (!model_path.empty()) {
        candidate_paths.emplace_back(model_path);
    }
    candidate_paths.push_back("ml/AI1/model/ai1_vision.onnx");
    candidate_paths.push_back("../ml/AI1/model/ai1_vision.onnx");
    candidate_paths.push_back("../../ml/AI1/model/ai1_vision.onnx");
    candidate_paths.push_back("../../../ml/AI1/model/ai1_vision.onnx");

    std::string resolved_path = "";
    for (const auto& path : candidate_paths) {
        std::ifstream f(path.c_str());
        if (f.good()) {
            resolved_path = path;
            break;
        }
    }

    if (resolved_path.empty()) {
        std::cerr << "[FoodClassifier] Error: ONNX YOLOv8 model file not found in search paths.\n";
        return false;
    }

    try {
        m_net = cv::dnn::readNetFromONNX(resolved_path);
        if (!m_net.empty()) {
            m_net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            m_net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            m_is_ready = true;
            std::cout << "[FoodClassifier] Successfully loaded YOLOv8 ONNX model from: " << resolved_path << "\n";
            return true;
        }
    } catch (const cv::Exception& e) {
        std::cerr << "[FoodClassifier] OpenCV DNN error loading " << resolved_path << ": " << e.what() << "\n";
    }

    return false;
}

bool FoodClassifier::detect(const cv::Mat& frame, 
                            std::vector<DetectionResult>& out_detections,
                            float conf_threshold,
                            float nms_threshold) {
    if (frame.empty()) {
        return false;
    }

    out_detections.clear();

    std::lock_guard<std::mutex> lock(m_infer_mutex);

    if (!m_is_ready || m_net.empty()) {
        return false;
    }

    try {
        const int input_w = 640;
        const int input_h = 640;

        // 1. Preprocess: Create 640x640 normalized blob for YOLOv8
        cv::Mat blob = cv::dnn::blobFromImage(
            frame,
            1.0 / 255.0,
            cv::Size(input_w, input_h),
            cv::Scalar(),
            true, // swapRB
            false // crop
        );

        m_net.setInput(blob);

        // 2. Forward inference
        cv::Mat output = m_net.forward();
        if (output.empty()) {
            return false;
        }

        // YOLOv8 detection output shape: [1, 4 + num_classes, 8400] -> [1, 13, 8400]
        const int num_classes = static_cast<int>(m_class_names.size());
        const int channels = 4 + num_classes;
        const int num_anchors = 8400;

        float* data = reinterpret_cast<float*>(output.data);
        cv::Mat output_transposed;

        if (output.dims == 3 && output.size[1] == channels && output.size[2] == num_anchors) {
            cv::Mat mat2d(channels, num_anchors, CV_32F, data);
            output_transposed = mat2d.t(); // Shape: [8400, 13]
        } else if (output.dims == 3 && output.size[1] == num_anchors && output.size[2] == channels) {
            output_transposed = cv::Mat(num_anchors, channels, CV_32F, data);
        } else {
            // Fallback generic reshape
            cv::Mat flat = output.reshape(1, channels);
            output_transposed = flat.t();
        }

        const float scale_x = static_cast<float>(frame.cols) / static_cast<float>(input_w);
        const float scale_y = static_cast<float>(frame.rows) / static_cast<float>(input_h);

        std::vector<cv::Rect> boxes;
        std::vector<float> confidences;
        std::vector<int> class_ids;

        boxes.reserve(64);
        confidences.reserve(64);
        class_ids.reserve(64);

        const int rows = output_transposed.rows;
        for (int i = 0; i < rows; ++i) {
            const float* row_ptr = output_transposed.ptr<float>(i);
            const float* scores = row_ptr + 4;

            int best_class_id = 0;
            float max_score = scores[0];

            for (int c = 1; c < num_classes; ++c) {
                if (scores[c] > max_score) {
                    max_score = scores[c];
                    best_class_id = c;
                }
            }

            if (max_score >= conf_threshold) {
                const float cx = row_ptr[0];
                const float cy = row_ptr[1];
                const float w  = row_ptr[2];
                const float h  = row_ptr[3];

                int left   = static_cast<int>((cx - 0.5f * w) * scale_x);
                int top    = static_cast<int>((cy - 0.5f * h) * scale_y);
                int width  = static_cast<int>(w * scale_x);
                int height = static_cast<int>(h * scale_y);

                cv::Rect box(left, top, width, height);
                // Clamp bounding box to frame boundaries
                box &= cv::Rect(0, 0, frame.cols, frame.rows);

                if (box.width > 4 && box.height > 4) {
                    boxes.push_back(box);
                    confidences.push_back(max_score);
                    class_ids.push_back(best_class_id);
                }
            }
        }

        // 3. Non-Maximum Suppression (NMS)
        std::vector<int> nms_indices;
        cv::dnn::NMSBoxes(boxes, confidences, conf_threshold, nms_threshold, nms_indices);

        out_detections.reserve(nms_indices.size());

        // 4. Extract RoI & run SIMD food freshness/spoilage evaluation
        for (int idx : nms_indices) {
            DetectionResult det;
            det.class_id = class_ids[idx];
            det.class_name = (det.class_id < static_cast<int>(m_class_names.size()))
                                 ? m_class_names[det.class_id]
                                 : ("class_" + std::to_string(det.class_id));
            det.confidence = confidences[idx];
            det.bbox = boxes[idx];

            // Evaluate Spoilage / Hue / Degradation using AVX2 SIMD
            if (det.bbox.width >= 8 && det.bbox.height >= 8) {
                cv::Mat roi = frame(det.bbox);
                det.spoilage = evaluateSpoilageAVX2(roi, det.class_name);
            }

            out_detections.push_back(std::move(det));
        }

        return true;
    } catch (const cv::Exception& e) {
        std::cerr << "[FoodClassifier] YOLOv8 detection error: " << e.what() << "\n";
        return false;
    }
}

bool FoodClassifier::predict(const cv::Mat& bgr_image, ClassificationResult& out_result) {
    if (bgr_image.empty()) {
        return false;
    }

    std::vector<DetectionResult> detections;
    if (detect(bgr_image, detections, 0.20f, 0.45f) && !detections.empty()) {
        // Return detection with highest confidence
        auto best = std::max_element(detections.begin(), detections.end(),
                                     [](const DetectionResult& a, const DetectionResult& b) {
                                         return a.confidence < b.confidence;
                                     });
        out_result = *best;
        return true;
    }

    out_result.class_id = -1;
    out_result.class_name = "no_food";
    out_result.confidence = 0.0f;
    out_result.bbox = cv::Rect(0, 0, bgr_image.cols, bgr_image.rows);
    return true;
}

} // namespace bawal::vision