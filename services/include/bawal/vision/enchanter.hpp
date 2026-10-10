#pragma once

#include <cstdint>
#include <string>
#include <filesystem>
#include <memory>
#include <opencv2/core/mat.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

namespace bawal::vision {

/**
 * @brief Hot data parameters packed tightly without compiler padding bytes.
 * Designed to fit within a single CPU cache line slice for high-throughput inference loops.
 */
struct alignas(4) RealESRGANParams {
    int32_t tile_size{192};   ///< Tile dimension in pixels (0 for un-tiled full frame processing)
    int32_t prepadding{10};   ///< Tile overlap boundary padding to eliminate border seam artifacts
    int16_t scale{2};         ///< Upscaling factor multiplier (typically 2x or 4x)
    int16_t gpu_device{0};    ///< Target Vulkan physical device index (e.g., 0 for dedicated GPU)
    uint8_t use_gpu{1};       ///< Flag to enable Vulkan compute acceleration (1 = GPU, 0 = CPU SIMD)
    uint8_t use_fp16{1};      ///< Flag to enable half-precision FP16 arithmetic on supported GPUs
    uint8_t reserved[2]{0, 0};///< Explicit 2-byte padding for clean 16-byte natural alignment
};

static_assert(sizeof(RealESRGANParams) == 16, "RealESRGANParams must be exactly 16 bytes");
static_assert(alignof(RealESRGANParams) == 4, "RealESRGANParams must be 4-byte aligned");

/**
 * @brief Cold configuration data containing model paths and runtime parameters.
 * Evaluated only during startup engine initialization.
 */
struct RealESRGANConfig {
    std::filesystem::path model_dir;               ///< Directory path containing .param/.bin or .onnx files
    std::string model_name{"realesr-animevideov3-x2"}; ///< Model architecture name prefix
    RealESRGANParams params;                       ///< Inference execution parameters
};

/**
 * @brief Standalone Super-Resolution and Enhancement engine leveraging OpenCV DNN and Tencent NCNN.
 */
struct RealESRGANEngine {
    /**
     * @brief Initialize NCNN or OpenCV DNN pipeline, allocate compute shaders, and load model weights.
     * 
     * @param config Cold configuration containing model filesystem paths and execution settings.
     * @return true if engine initialized successfully, false otherwise.
     */
    [[nodiscard]] bool init(const RealESRGANConfig& config);

    /**
     * @brief Initialize OpenCV DNN neural network model from ONNX weight file.
     * 
     * @param model_path Path to ONNX super-resolution / enhancer model.
     * @return true if loaded and compiled into OpenCV DNN, false otherwise.
     */
    [[nodiscard]] bool initOpenCVDNN(const std::filesystem::path& model_path);

    /**
     * @brief Execute super-resolution upscaling on a BGR image frame.
     * 
     * @param input Source image matrix (8-bit BGR format).
     * @param output Destination matrix holding the upscaled image (dimensions = input * scale).
     * @return true on successful upscale execution, false if frame is empty or pipeline fails.
     */
    [[nodiscard]] bool process(const cv::Mat& input, cv::Mat& output);

    /**
     * @brief Execute forward inference using OpenCV DNN neural net.
     * 
     * @param input Source image matrix (8-bit BGR format).
     * @param output Destination matrix.
     * @return true on successful execution.
     */
    [[nodiscard]] bool processOpenCVDNN(const cv::Mat& input, cv::Mat& output);

    /**
     * @brief Release loaded neural graphs and free compute instances.
     */
    void release();

    [[nodiscard]] bool isReady() const noexcept {
        return is_initialized || is_dnn_ready;
    }

    [[nodiscard]] bool isGpuAccelerated() const noexcept {
        return is_initialized && (params.use_gpu != 0);
    }

    [[nodiscard]] bool hasDnnModel() const noexcept {
        return is_dnn_ready;
    }

    bool is_initialized{false}; ///< Operational readiness state flag (NCNN/SIMD)
    bool is_dnn_ready{false};   ///< Operational readiness state flag (OpenCV DNN)
    cv::dnn::Net dnn_net;       ///< OpenCV DNN Neural Network container
    RealESRGANParams params;    ///< Compact active hot parameters (14 bytes) retained during runtime
};

/**
 * @brief Multi-Architecture Target-Cloned SIMD Super-Resolution / Texture Kernel (AVX-512 / AVX2 / OpenMP).
 */
void applyFastSuperResolutionSIMD(const cv::Mat& src, cv::Mat& dst, int scale = 2);

/**
 * @brief Deep AI Illumination & Scene Restoration Engine (C++).
 * Employs adaptive non-linear curve estimation (Zero-DCE style), multi-scale Retinex
 * shadow lifting, highlight glare compression, and CIELAB color vibrancy recovery.
 */
struct IlluminationEnhancerAI {
    /**
     * @brief Execute intelligent low-light restoration, shadow illumination, and edge sharpening.
     * 
     * @param src Raw camera BGR matrix
     * @param dst Restored high-definition BGR matrix
     * @param persistent_clahe Optional persistent CLAHE context to eliminate per-frame allocations
     */
    static void process(const cv::Mat& src, cv::Mat& dst, cv::Ptr<cv::CLAHE> persistent_clahe = nullptr);
};

} // namespace bawal::vision