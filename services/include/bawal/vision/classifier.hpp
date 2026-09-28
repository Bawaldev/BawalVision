#pragma once

#include <memory>
#include <string>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

namespace bawal::vision {

struct ClassificationResult {
    int class_id{-1};
    std::string class_name;
    float confidence{0.0f};
    std::vector<float> all_probabilities;
};

class FoodClassifier {
public:
    FoodClassifier();
    ~FoodClassifier() = default;

    /**
     * @brief Initialize AI1 inference engine (ONNX via OpenCV DNN).
     * 
     * @param model_path Path to ai1_vision.onnx file.
     * @param thread_count Number of worker threads.
     * @return true if ONNX model was loaded successfully.
     */
    [[nodiscard]] bool init(const std::string& model_path, int thread_count = 2);

    /**
     * @brief Execute classification inference on a meal compartment sub-image.
     * 
     * @param bgr_image BGR sub-matrix of compartment.
     * @param out_result Predicted class label, confidence, and probabilities.
     * @return true if inference succeeded.
     */
    [[nodiscard]] bool predict(const cv::Mat& bgr_image, ClassificationResult& out_result);

    /**
     * @brief Check AI1 model readiness.
     */
    [[nodiscard]] bool isReady() const noexcept {
        return m_is_ready;
    }

private:
    bool m_is_ready{false};
    cv::dnn::Net m_net;
    std::vector<std::string> m_class_names;
};

} // namespace bawal::vision