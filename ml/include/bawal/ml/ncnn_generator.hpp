#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <filesystem>
#include <concepts>
#include <compare>
#include <cstdint>

namespace bawal::ml {

/**
 * @brief Concepts for NCNN Super-Resolution numeric configurations.
 */
template <typename T>
concept PositiveNumeric = std::integral<T> || std::floating_point<T>;

/**
 * @brief Configuration parameters for NCNN Sub-Pixel Super-Resolution models.
 */
struct NCNNModelConfig {
    std::string model_name{"realesr-animevideov3-x2"};
    int scale{2};
    int in_channels{3};
    int mid_channels{32};
    float leaky_relu_slope{0.1f};
    std::filesystem::path output_dir{"weights"};
    std::vector<std::filesystem::path> sync_dirs{
        "services/weights"
    };

    auto operator<=>(const NCNNModelConfig&) const = default;
};

/**
 * @brief Compile-time calculations for convolution kernel geometry.
 */
[[nodiscard]] constexpr size_t computeConvWeightsCount(int in_c, int out_c, int k) noexcept {
    return static_cast<size_t>(in_c) * static_cast<size_t>(out_c) * static_cast<size_t>(k * k);
}

[[nodiscard]] constexpr int computeSubPixelChannels(int in_c, int scale) noexcept {
    return in_c * (scale * scale);
}

/**
 * @brief Generation statistics and output paths.
 */
struct NCNNGenerationResult {
    bool success{false};
    std::filesystem::path param_path;
    std::filesystem::path bin_path;
    size_t param_size_bytes{0};
    size_t bin_size_bytes{0};
    std::vector<std::filesystem::path> synced_paths;

    auto operator<=>(const NCNNGenerationResult&) const = default;
};

/**
 * @brief Modular High-Performance NCNN Model Generator (Modern C++20).
 * Synthesizes official NCNN .param graph and packed binary .bin weights
 * with identity-preserving Bilinear-Impulse sub-pixel reconstruction kernels.
 */
class NCNNGenerator {
public:
    /**
     * @brief End-to-end generation and directory synchronization.
     * @param config Model architecture and export configuration.
     * @return NCNNGenerationResult with file details and status.
     */
    [[nodiscard]] static NCNNGenerationResult generate(const NCNNModelConfig& config);

    /**
     * @brief Generates valid NCNN .param file format (Magic 7767517) using std::format.
     */
    [[nodiscard]] static bool generateParamFile(const NCNNModelConfig& config, const std::filesystem::path& param_path);

    /**
     * @brief Generates packed binary float32 .bin weights file using std::span.
     */
    [[nodiscard]] static bool generateBinFile(const NCNNModelConfig& config, const std::filesystem::path& bin_path);

    /**
     * @brief Synchronizes generated model files to secondary directories via std::span.
     */
    [[nodiscard]] static std::vector<std::filesystem::path> syncWeights(
        const std::filesystem::path& param_path,
        const std::filesystem::path& bin_path,
        std::span<const std::filesystem::path> sync_dirs
    );
};

} // namespace bawal::ml

