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
#include "bawal/vision/segmenter.hpp"
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
    std::vector<TrayCompartment> compartments;
    std::vector<ClassificationResult> classifications;
    double processing_time_ms{0.0};
};

/**
 * @brief Master Orchestrator Pipeline:
 * [CameraReceiver] -> [Calibration & Dewarp] -> [Segmenter] -> [FoodClassifier/AI1]
 */
class InspectionPipeline {
public:
    InspectionPipeline() = default;
    ~InspectionPipeline() = default;

    /**
     * @brief Initialize all pipeline stages (dewarper, segmenter grid, and AI1 classifier).
     * 
     * @param model_path Path to ONNX model (optional, falls back to heuristic if empty/missing)
     * @param target_width Processing resolution width (default: 640)
     * @param target_height Processing resolution height (default: 480)
     * @return true on successful initialization.
     */
    [[nodiscard]] bool initialize(const std::string& model_path = "", 
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
     * @brief Renders visual bounding boxes, compartment labels, and confidence overlays on image.
     * 
     * @param inspection Output struct from execute()
     * @param out_display Destination visual HUD matrix
     */
    void renderOverlay(const InspectionOutput& inspection, cv::Mat& out_display);

    // Component accessors
    [[nodiscard]] CameraReceiver& getReceiver() noexcept { return m_receiver; }
    [[nodiscard]] DewarpEngine& getDewarper() noexcept { return m_dewarper; }
    [[nodiscard]] Segmenter& getSegmenter() noexcept { return m_segmenter; }
    [[nodiscard]] FoodClassifier& getClassifier() noexcept { return m_classifier; }

private:
    CameraReceiver m_receiver;
    DewarpEngine m_dewarper;
    Segmenter m_segmenter;
    FoodClassifier m_classifier;
    int m_width{640};
    int m_height{480};
};

} // namespace bawal::vision