#pragma once

#include "bawal/vision/simd_food_evaluator.hpp"
#include <opencv2/core.hpp>
#include <onnxruntime_cxx_api.h>
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <memory>
#include <mutex>
#include <array>
#include <new>

namespace bawal::vision {

/**
 * @brief Compact, cache-friendly bounding box POD (16 bytes, zero OpenCV class overhead).
 */
struct alignas(16) CompactBBox {
    int32_t x{0};
    int32_t y{0};
    int32_t width{0};
    int32_t height{0};

    constexpr CompactBBox() noexcept = default;
    constexpr CompactBBox(int32_t x_, int32_t y_, int32_t w_, int32_t h_) noexcept
        : x(x_), y(y_), width(w_), height(h_) {}
    CompactBBox(const cv::Rect& r) noexcept
        : x(r.x), y(r.y), width(r.width), height(r.height) {}

    operator cv::Rect() const noexcept {
        return cv::Rect(x, y, width, height);
    }

    cv::Rect toCvRect() const noexcept {
        return cv::Rect(x, y, width, height);
    }

    cv::Rect operator&(const cv::Rect& r) const noexcept {
        return toCvRect() & r;
    }
};

static_assert(sizeof(CompactBBox) == 16, "CompactBBox must be exactly 16 bytes");
static_assert(alignof(CompactBBox) == 16, "CompactBBox must be 16-byte aligned for SSE/AVX registers");

/**
 * @brief Cache-aligned food detection outcome fitting exactly within one 64-byte L1 CPU cache line.
 * [0..15]  std::string_view class_name (16 bytes)
 * [16..31] CompactBBox bbox           (16 bytes, 16-byte aligned for SSE/AVX)
 * [32..43] SpoilageMetric spoilage    (12 bytes, 4-byte aligned)
 * [44..47] float confidence           (4 bytes)
 * [48..51] int32_t class_id           (4 bytes)
 * [52..63] uint32_t reserved[3]       (12 bytes explicit cache line padding)
 * Total: Exactly 64 bytes (1 CPU Cache Line, ZERO cache-line splitting across array indices)
 */
struct alignas(64) DetectionResult {
    std::string_view class_name{"no_food"}; 
    CompactBBox bbox;                        
    SpoilageMetric spoilage;                 
    float confidence{0.0f};                  
    int32_t class_id{-1};                    
    uint32_t reserved[3]{0, 0, 0};
};

static_assert(sizeof(DetectionResult) == 64, "DetectionResult must be exactly 64 bytes (1 CPU cache line)");
static_assert(alignof(DetectionResult) == 64, "DetectionResult must be 64-byte aligned");

using ClassificationResult = DetectionResult;

/**
 * @brief Temporal tracker container to eliminate frame-to-frame bounding box flickering.
 * Aligned to 64 bytes (128 bytes total = exactly 2 L1 CPU cache lines).
 */
struct alignas(64) TrackedItem {
    DetectionResult result;       ///< 64 bytes (Cache Line 0)
    int age{0};                   ///< 4 bytes  (Cache Line 1)
    int lost_frames{0};           ///< 4 bytes
    int stable_frames{0};         ///< 4 bytes
    cv::Point last_center{0, 0};  ///< 8 bytes (x, y)
    uint32_t reserved[11]{0};     ///< 44 bytes explicit padding
};

static_assert(sizeof(TrackedItem) == 128, "TrackedItem must be exactly 128 bytes (2 CPU cache lines)");
static_assert(alignof(TrackedItem) == 64, "TrackedItem must be 64-byte aligned");

/**
 * @brief High-speed hardware-accelerated scene optical and illumination telemetry.
 */
struct SceneOpticalMetrics {
    double sum_b{0.0};
    double sum_g{0.0};
    double sum_r{0.0};
    int sampled_pixels{0};
    float avg_b{0.0f};
    float avg_g{0.0f};
    float avg_r{0.0f};
    float lux_luminance{0.0f};           ///< Standard ITU-R BT.601 perceived luminance (lux proxy)
    int color_temperature{6500};         ///< Estimated Correlated Color Temperature in Kelvin
    std::string white_balance_status{"BALANCED"}; ///< BALANCED, WARM, COOL, UNDEREXPOSED, OVEREXPOSED
};

/**
 * @brief Vectorized AVX2 / SIMD BGR channel summation across sampled matrix rows.
 */
void computeBGRSumsAVX2(const cv::Mat& bgr_mat, double& out_sum_b, double& out_sum_g, double& out_sum_r, int& out_sampled_pixels);

/**
 * @brief Computes ambient illumination and color balance for the scene or food ROI.
 */
[[nodiscard]] SceneOpticalMetrics evaluateSceneOptical(const cv::Mat& bgr_mat);

class FoodClassifier {
public:
    FoodClassifier();
    ~FoodClassifier() = default;

    [[nodiscard]] const SceneOpticalMetrics& getLastOpticalMetrics() const noexcept { return m_last_optical; }

    /**
     * @brief Initializes the ONNX Runtime Engine with the YOLOv8 food detection model.
     * 
     * @param model_path Path to the ONNX model file.
     * @param thread_count Number of intra-op execution threads.
     * @return true if initialization succeeded and session is active.
     */
    [[nodiscard]] bool init(std::string_view model_path, int thread_count = 4);

    /**
     * @brief Zero-allocation multi-object food detection writing directly to a caller-provided span.
     * 
     * @param frame Input BGR frame matrix.
     * @param out_buffer Output span slice (e.g. backed by std::array on the CPU stack).
     * @param out_count Number of valid detections written into out_buffer.
     * @param conf_threshold Minimum detection confidence threshold.
     * @param nms_threshold Non-Maximum Suppression IoU threshold.
     * @return true if inference and post-processing completed successfully.
     */
    [[nodiscard]] bool detect(const cv::Mat& frame, 
                              std::span<DetectionResult> out_buffer,
                              size_t& out_count,
                              float conf_threshold = 0.58f,
                              float nms_threshold = 0.45f);

    /**
     * @brief Overload populating an output vector of detections for callers preferring a container.
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

    /**
     * @brief Gets the active model architecture string (e.g. YOLOv8s-INT8, YOLOv8s, YOLOv8n-INT8).
     * @return String view describing the detected and loaded neural network architecture.
     */
    [[nodiscard]] std::string_view getModelArchitecture() const noexcept { return m_model_arch; }

    /**
     * @brief Gets the filesystem path of the currently loaded model.
     * @return Path string of the active ONNX model binary.
     */
    [[nodiscard]] std::string_view getActiveModelPath() const noexcept { return m_active_model_path; }

    /**
     * @brief Reset temporal object tracks (e.g. on video rewind or scene cut).
     */
    void resetTracker() noexcept {
        std::lock_guard<std::mutex> lock(m_infer_mutex);
        m_tracked_items.clear();
    }

private:

    // Model metadata (static, zero heap overhead)
    std::vector<std::string_view> m_class_names;
    std::mutex m_infer_mutex;
    bool m_is_ready{false};
    std::string m_model_arch{"YOLOv8s-INT8"};
    std::string m_active_model_path{};
    SceneOpticalMetrics m_last_optical;

    // ONNX Runtime Native Resources
    Ort::Env m_env;
    Ort::SessionOptions m_session_options;
    std::unique_ptr<Ort::Session> m_session;

    // ONNX node names: constant pointer literals without std::vector overhead
    static constexpr const char* K_INPUT_NAMES[]  = {"images"};
    static constexpr const char* K_OUTPUT_NAMES[] = {"output0"};

    // Fixed input tensor dimensions in stack/inline memory (1x3x640x640)
    static constexpr std::array<int64_t, 4> K_INPUT_SHAPE{1, 3, 640, 640};
    static constexpr size_t K_TENSOR_ELEMENTS = 1 * 3 * 640 * 640;

    // 64-byte aligned preprocessing buffer (AVX2/AVX-512 friendly)
    // Allocated once on heap during init() to prevent stack overflow
    struct AlignedDeleter {
        void operator()(float* p) const noexcept { ::operator delete[](p, std::align_val_t{64}); }
    };
    std::unique_ptr<float[], AlignedDeleter> m_input_tensor_values;
    cv::Mat m_resized_buffer;
    std::vector<TrackedItem> m_tracked_items;
};

} // namespace bawal::vision