#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <opencv2/core.hpp>

#include "bawal/vision/calibration.hpp"
#include "bawal/vision/camera_receiver.hpp"
#include "bawal/vision/classifier.hpp"
#include "bawal/vision/dewarp.hpp"
#include "bawal/vision/types.hpp"

namespace bawal::vision {

/**
 * @brief Complete inspection output container.
 */
struct InspectionOutput {
    uint32_t frame_id{0};
    bool is_valid{false};
    cv::Mat raw_image;
    cv::Mat dewarped_image;
    cv::Mat annotated_image;
    std::vector<DetectionResult> detections;
    double processing_time_ms{0.0};
};

/**
 * @brief Master Orchestrator Pipeline:
 * [CameraReceiver] -> [Calibration & Dewarp] -> [YOLOv8 Classifier + SIMD Food Evaluator]
 */
class InspectionPipeline {
public:
    InspectionPipeline() = default;
    ~InspectionPipeline() = default;

    /**
     * @brief Initialize all pipeline stages (dewarper and AI1 YOLOv8 classifier).
     * 
     * @param model_path Path to ONNX model (optional, falls back to heuristic if empty/missing)
     * @param target_width Processing resolution width (default: 640)
     * @param target_height Processing resolution height (default: 480)
     * @return true on successful initialization.
     */
    [[nodiscard]] bool initialize(std::string_view model_path = "", 
                                  int target_width = 640, 
                                  int target_height = 480);

    /**
     * @brief End-to-end execution from raw byte buffer (MJPEG / JPEG / binary stream).
     * 
     * @param raw_frame_bytes Raw image payload from network
     * @param out_inspection Full inspection and classification output
     * @return true if pipeline executed successfully.
     */
    [[nodiscard]] bool execute(const uint8_t* raw_bytes, size_t len, InspectionOutput& out_inspection);
    [[nodiscard]] bool execute(const std::vector<uint8_t>& raw_frame_bytes, InspectionOutput& out_inspection);

    /**
     * @brief Direct execution on an already decoded cv::Mat frame.
     * 
     * @param frame BGR image matrix
     * @param out_inspection Full inspection and classification output
     * @return true if pipeline executed successfully.
     */
    [[nodiscard]] bool execute(const cv::Mat& frame, InspectionOutput& out_inspection);

    /**
     * @brief Adaptive Image Enhancement: Edge-preserving Bilateral Denoising, CLAHE (L* in CIE-Lab), and Unsharp Masking.
     * 
     * @param src Input BGR image matrix
     * @param dst Enhanced output image matrix with boosted micro-contrast and sharpness
     */
    void enhanceImage(const cv::Mat& src, cv::Mat& dst);

    /**
     * @brief Renders visual bounding boxes, food class badges, and spoilage indicators on image.
     * 
     * @param inspection Output struct from execute()
     * @param out_display Destination visual HUD matrix
     */
    void renderOverlay(const InspectionOutput& inspection, cv::Mat& out_display);

    // Component accessors
    [[nodiscard]] CameraReceiver& getReceiver() noexcept { return m_receiver; }
    [[nodiscard]] DewarpEngine& getDewarper() noexcept { return m_dewarper; }
    [[nodiscard]] FoodClassifier& getClassifier() noexcept { return m_classifier; }

private:
    CameraReceiver m_receiver;
    DewarpEngine m_dewarper;
    FoodClassifier m_classifier;
    int m_width{640};
    int m_height{480};
};

} // namespace bawal::vision