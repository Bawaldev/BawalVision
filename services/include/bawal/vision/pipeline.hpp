#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <opencv2/core.hpp>

#include "bawal/vision/calibration.hpp"
#include "bawal/vision/camera_receiver.hpp"
#include "bawal/vision/classifier.hpp"
#include "bawal/vision/dark_detector.hpp"
#include "bawal/vision/dewarp.hpp"
#include "bawal/vision/enchanter.hpp"
#include "bawal/vision/types.hpp"
#include "bawal/vision/video_source.hpp"

namespace bawal::vision {

/**
 * @brief Complete inspection output container.
 */
struct InspectionOutput {
    double processing_time_ms{0.0}; // 8 bytes (offset 0)
    uint32_t frame_id{0};           // 4 bytes (offset 8)
    bool is_valid{false};           // 1 byte  (offset 12)
    uint8_t reserved[3]{0, 0, 0};   // 3 bytes explicit padding (16 bytes aligned)
    cv::Mat raw_image;
    cv::Mat dewarped_image;
    cv::Mat annotated_image;
    std::vector<DetectionResult> detections;
    SceneOpticalMetrics scene_optical;
    DarkDetectionResult dark_status;

    InspectionOutput() {
        detections.reserve(16);
    }
};

/**
 * @brief Master Orchestrator Pipeline:
 * [VideoSource / CameraReceiver] -> [Calibration & Dewarp] -> [Enchanter ISP + Real-ESRGAN Super-Resolution] -> [YOLOv8 Classifier + SIMD Food Evaluator]
 */
class InspectionPipeline {
public:
    InspectionPipeline() = default;
    ~InspectionPipeline() = default;

    /**
     * @brief Initialize all pipeline stages (dewarper, super-resolution enhancer, and AI1 YOLOv8 classifier).
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
     * @brief Open the pipeline's integrated VideoSource (ESP32_STREAM, LOCAL_MP4, or WEBCAM_USB).
     */
    [[nodiscard]] bool openVideoSource(SourceMode mode, const std::string& uri);

    /**
     * @brief Grab the next frame from the integrated VideoSource and run the real-time streaming pass
     * (Dewarp + Enchanter ISP + Real-ESRGAN ROI fusion + HUD Overlay).
     */
    [[nodiscard]] bool processNextSourceFrame(const std::vector<DetectionResult>& active_detections,
                                              InspectionOutput& out_inspection,
                                              bool is_mock_video = false);

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
     * @brief High-speed real-time streaming pass: Dewarp + Fast ISP + RealESRGAN ROI fusion + Overlay (< 6 ms, 30+ FPS).
     * 
     * @param frame Raw camera BGR matrix
     * @param active_detections Pre-computed latest detections from background AI worker
     * @param out_inspection Formatted inspection output with annotated stream frame
     * @return true if stream frame processed successfully.
     */
    [[nodiscard]] bool processStream(const cv::Mat& frame, 
                                     const std::vector<DetectionResult>& active_detections, 
                                     InspectionOutput& out_inspection,
                                     bool is_mock_video = false);

    /**
     * @brief High-performance AI detection and SIMD spoilage evaluation pass.
     * 
     * @param enhanced_frame ISP-enhanced frame
     * @param out_detections Output detected food items with freshness scores
     * @return true if inference succeeded.
     */
    [[nodiscard]] bool runInference(const cv::Mat& enhanced_frame, 
                                    std::vector<DetectionResult>& out_detections);

    /**
     * @brief Adaptive Image Enhancement: Super-Resolution / Bilateral Denoising, CLAHE, and Unsharp Masking.
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

    /**
     * @brief Reset temporal object tracks (e.g. on video rewind or scene cut).
     */
    void resetTracker() noexcept {
        m_classifier.resetTracker();
    }

    // Dynamic Confidence Threshold Accessors (Range: 0.25 - 0.98, Default: 0.35).
    // Atomic because the AI worker updates it while the stream thread renders the HUD.
    void setConfidenceThreshold(float thresh) noexcept {
        m_conf_threshold.store(std::clamp(thresh, 0.25f, 0.98f), std::memory_order_relaxed);
    }
    [[nodiscard]] float getConfidenceThreshold() const noexcept {
        return m_conf_threshold.load(std::memory_order_relaxed);
    }

    /**
     * @brief Gets the active model architecture string from the classifier.
     * @return Architecture name (e.g. YOLOv8s-INT8, YOLOv8s, YOLOv8n-INT8).
     */
    [[nodiscard]] std::string_view getModelArchitecture() const noexcept {
        return m_classifier.getModelArchitecture();
    }

    // Component accessors
    [[nodiscard]] VideoSource& getVideoSource() noexcept { return m_video_source; }
    [[nodiscard]] CameraReceiver& getReceiver() noexcept { return m_receiver; }
    [[nodiscard]] DewarpEngine& getDewarper() noexcept { return m_dewarper; }
    [[nodiscard]] RealESRGANEngine& getEnhancer() noexcept { return m_enhancer; }
    [[nodiscard]] FoodClassifier& getClassifier() noexcept { return m_classifier; }
    [[nodiscard]] const DarkDetector& getDarkDetector() const noexcept { return m_dark_detector; }

private:
    VideoSource m_video_source;
    CameraReceiver m_receiver;
    DewarpEngine m_dewarper;
    RealESRGANEngine m_enhancer;
    FoodClassifier m_classifier;
    DarkDetector m_dark_detector;
    cv::Ptr<cv::CLAHE> m_clahe;

    // Runtime configuration is parsed once in initialize() instead of being
    // lazily initialised through function-local statics (which race when the
    // pipeline is driven from more than one thread).
    std::atomic<float> m_conf_threshold{0.35f};
    bool m_enable_enhance{true};
    bool m_vflip{false};
    bool m_hflip{false};
    bool m_enable_dewarp{false};
    bool m_enable_sr{true};

    // Super-resolution rate limiter (touched only by runInference()).
    std::mutex m_sr_mutex;
    std::chrono::steady_clock::time_point m_last_sr_time{};

    // Reused HUD destination buffer. Avoids allocating a fresh full-frame Mat
    // in renderOverlay() for every streamed frame.
    cv::Mat m_overlay_buffer;

    int m_width{640};
    int m_height{480};
};

} // namespace bawal::vision