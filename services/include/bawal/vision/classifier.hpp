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

struct DetectionResult {
    int class_id{-1};
    std::string class_name{"no_food"};
    float confidence{0.0f};
    cv::Rect bbox;
    SpoilageMetric spoilage;
};

using ClassificationResult = DetectionResult;

class FoodClassifier {
public:
    FoodClassifier();
    ~FoodClassifier() = default;

    /**
     * @brief Inisialisasi ONNX Runtime Engine untuk model YOLOv8.
     */
    [[nodiscard]] bool init(std::string_view model_path, int thread_count = 4);

    /**
     * @brief Deteksi multi-objek makanan YOLOv8 + evaluasi degradasi warna SIMD.
     */
    [[nodiscard]] bool detect(const cv::Mat& frame, 
                            std::vector<DetectionResult>& out_detections,
                            float conf_threshold = 0.20f,
                            float nms_threshold = 0.45f);

    /**
     * @brief Prediksi tunggal ROI dengan skor keyakinan tertinggi.
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
};

} // namespace bawal::vision