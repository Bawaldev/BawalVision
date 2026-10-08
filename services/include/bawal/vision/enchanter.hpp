#pragma once

#include <cstdint>
#include <string>
#include <filesystem>
#include <opencv2/core/mat.hpp>

namespace bawal::vision {

/**
 * @brief Hot data parameters packed tightly without compiler padding bytes.
 * Designed to fit within a single CPU cache line slice for high-throughput inference loops.
 */
struct __attribute__((packed)) RealESRGANParams {
    int32_t tile_size{192};   ///< Tile dimension in pixels (0 for un-tiled full frame processing)
    int32_t prepadding{10};   ///< Tile overlap boundary padding to eliminate border seam artifacts
    int16_t scale{2};         ///< Upscaling factor multiplier (typically 2x or 4x)
    int16_t gpu_device{0};    ///< Target Vulkan physical device index (e.g., 0 for dedicated GPU)
    uint8_t use_gpu{1};       ///< Flag to enable Vulkan compute acceleration (1 = GPU, 0 = CPU SIMD)
    uint8_t use_fp16{1};      ///< Flag to enable half-precision FP16 arithmetic on supported GPUs
};

/**
 * @brief Cold configuration data containing model paths and runtime parameters.
 * Evaluated only during startup engine initialization.
 */
struct RealESRGANConfig {
    std::filesystem::path model_dir;               ///< Directory path containing .param and .bin weight files
    std::string model_name{"realesr-animevideov3-x2"}; ///< Model architecture name prefix
    RealESRGANParams params;                       ///< Inference execution parameters
};

/**
 * @brief Standalone Super-Resolution inference engine leveraging Tencent NCNN Vulkan backend.
 */
struct RealESRGANEngine {
    /**
     * @brief Initialize NCNN pipeline, allocate Vulkan compute shaders, and load model weights.
     * 
     * @param config Cold configuration containing model filesystem paths and execution settings.
     * @return true if Vulkan device context and model weight layers load successfully, false otherwise.
     */
    [[nodiscard]] bool init(const RealESRGANConfig& config);

    /**
     * @brief Execute super-resolution upscaling on a BGR image frame.
     * 
     * Performs optional image tiling, FP16/FP32 neural inference, and stitches
     * processed tiles into the scaled destination matrix.
     * 
     * @param input Source image matrix (8-bit BGR format).
     * @param output Destination matrix holding the upscaled image (dimensions = input * scale).
     * @return true on successful upscale execution, false if frame is empty or pipeline fails.
     */
    [[nodiscard]] bool process(const cv::Mat& input, cv::Mat& output);

    /**
     * @brief Release loaded NCNN network graphs and destroy Vulkan GPU pipeline instances.
     */
    void release();

    /**
     * @brief Query initialization state of the inference engine.
     * 
     * @return true if weights are resident in memory and ready for inference, false otherwise.
     */
    [[nodiscard]] bool isReady() const noexcept {
        return is_initialized;
    }

    /**
     * @brief Query whether neural acceleration is using hardware GPU (Vulkan).
     * 
     * @return true if hardware GPU Vulkan is active, false if operating on CPU SIMD.
     */
    [[nodiscard]] bool isGpuAccelerated() const noexcept {
        return is_initialized && (params.use_gpu != 0);
    }

    bool is_initialized{false}; ///< Operational readiness state flag
    RealESRGANParams params;     ///< Compact active hot parameters (14 bytes) retained during runtime
};

} // namespace bawal::vision