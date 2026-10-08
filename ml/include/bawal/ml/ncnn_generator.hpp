#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>

namespace bawal::ml {

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
};

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
};

/**
 * @brief Modular High-Performance NCNN Model Generator.
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
    static NCNNGenerationResult generate(const NCNNModelConfig& config);

    /**
     * @brief Generates valid NCNN .param file format (Magic 7767517).
     */
    static bool generateParamFile(const NCNNModelConfig& config, const std::filesystem::path& param_path);

    /**
     * @brief Generates packed binary float32 .bin weights file.
     */
    static bool generateBinFile(const NCNNModelConfig& config, const std::filesystem::path& bin_path);

    /**
     * @brief Synchronizes generated model files to secondary directories.
     */
    static std::vector<std::filesystem::path> syncWeights(
        const std::filesystem::path& param_path,
        const std::filesystem::path& bin_path,
        const std::vector<std::filesystem::path>& sync_dirs
    );
};

} // namespace bawal::ml

