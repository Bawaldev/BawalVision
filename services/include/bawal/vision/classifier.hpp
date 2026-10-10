#pragma once

#include "bawal/vision/simd_food_evaluator.hpp"
#include <opencv2/core.hpp>
#include <onnxruntime_cxx_api.h>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <mutex>

namespace bawal::vision {

/**
 * @brief Detection outcome for an individual food bounding box with SIMD freshness metrics.
 */
struct DetectionResult {
    int class_id{-1};
    std::string class_name{"no_food"};
    float confidence{0.0f};
    cv::Rect bbox;
    SpoilageMetric spoilage;
};

using ClassificationResult = DetectionResult;

/**
 * @brief Temporal tracker container to eliminate frame-to-frame bounding box flickering.
 */
struct TrackedItem {
    DetectionResult result;
    int age{0};           ///< Number of consecutive frames tracked
    int lost_frames{0};   ///< Empty frame tolerance before track eviction
    int stable_frames{0}; ///< Consecutive frames bounding box has remained stationary
    cv::Point last_center{0, 0}; ///< Previous frame bounding box center coordinates
};

class FoodClassifier {
public:
    FoodClassifier();
    ~FoodClassifier() = default;

    /**
     * @brief Initializes the ONNX Runtime Engine with the YOLOv8 food detection model.
     * 
     * @param model_path Path to the ONNX model file.
     * @param thread_count Number of intra-op execution threads.
     * @return true if initialization succeeded and session is active.
     */
    [[nodiscard]] bool init(std::string_view model_path, int thread_count = 4);

    /**
     * @brief Multi-object food detection using YOLOv8 fused with AVX2 SIMD freshness evaluation.
     * 
     * @param frame Input BGR frame matrix.
     * @param out_detections Output vector of validated food detections.
     * @param conf_threshold Minimum detection confidence threshold.
     * @param nms_threshold Non-Maximum Suppression IoU threshold.
     * @return true if inference and post-processing completed successfully.
     */
    [[nodiscard]] bool detect(const cv::Mat& frame, 
                            std::vector<DetectionResult>& out_detections,
                            float conf_threshold = 0.58f,
                            float nms_threshold = 0.45f);

    /**
     * @brief Single-ROI prediction returning the candidate with highest confidence score.
     * 
     * @param bgr_image Input BGR ROI image.
     * @param out_result Highest confidence detection outcome.
     * @return true if classification succeeded.
     */
    [[nodiscard]] bool predict(const cv::Mat& bgr_image, ClassificationResult& out_result);

    [[nodiscard]] bool isReady() const noexcept { return m_is_ready; }

private:
    std::vector<std::string> m_class_names;
    std::mutex m_infer_mutex;
    bool m_is_ready{false};

    // ONNX Runtime Native Resources
    Ort::Env m_env;
    Ort::SessionOptions m_session_options;
    std::unique_ptr<Ort::Session> m_session;

    std::vector<std::string> m_input_node_names;
    std::vector<std::string> m_output_node_names;
    std::vector<const char*> m_input_names;
    std::vector<const char*> m_output_names;
    std::vector<int64_t> m_input_shape;

    // Persistent zero-allocation preprocessing buffers
    std::vector<float> m_input_tensor_values;
    cv::Mat m_resized_buffer;
};

} // namespace bawal::vision