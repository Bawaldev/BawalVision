#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include "bawal/vision/simd_food_evaluator.hpp"

namespace bawal::vision {

struct DetectionResult {
    int class_id{-1};
    std::string class_name;
    float confidence{0.0f};
    cv::Rect bbox;
    SpoilageMetric spoilage;
};

// Aliases for compatibility
using ClassificationResult = DetectionResult;

class FoodClassifier {
public:
    FoodClassifier();
    ~FoodClassifier() = default;

    /**
     * @brief Initialize YOLOv8 object detection engine (ONNX via OpenCV DNN).
     * 
     * @param model_path Path to ai1_vision.onnx file.
     * @param thread_count Number of worker threads.
     * @return true if ONNX model was loaded successfully.
     */
    [[nodiscard]] bool init(std::string_view model_path, int thread_count = 2);

    /**
     * @brief Execute YOLOv8 detection and AVX2/SIMD food freshness evaluation on input frame.
     * 
     * @param frame BGR image matrix of tray / food.
     * @param out_detections Vector of detected food items with bounding boxes and spoilage metrics.
     * @param conf_threshold Confidence threshold for detection (default: 0.25).
     * @param nms_threshold Non-Maximum Suppression IoU threshold (default: 0.45).
     * @return true if detection inference executed successfully.
     */
    [[nodiscard]] bool detect(const cv::Mat& frame, 
                              std::vector<DetectionResult>& out_detections,
                              float conf_threshold = 0.25f,
                              float nms_threshold = 0.45f);

    /**
     * @brief Legacy single-ROI predict interface for backward compatibility.
     */
    [[nodiscard]] bool predict(const cv::Mat& bgr_image, ClassificationResult& out_result);

    /**
     * @brief Check AI1 model readiness.
     */
    [[nodiscard]] bool isReady() const noexcept {
        return m_is_ready;
    }

    [[nodiscard]] const std::vector<std::string>& getClassNames() const noexcept {
        return m_class_names;
    }

private:
    bool m_is_ready{false};
    cv::dnn::Net m_net;
    std::vector<std::string> m_class_names;
    mutable std::mutex m_infer_mutex;
};

} // namespace bawal::vision