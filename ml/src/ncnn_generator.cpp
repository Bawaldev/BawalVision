#include "bawal/ml/ncnn_generator.hpp"

#include <iostream>
#include <fstream>
#include <vector>
#include <span>
#include <format>
#include <cmath>
#include <cstring>
#include <ranges>

namespace fs = std::filesystem;

namespace bawal::ml {

bool NCNNGenerator::generateParamFile(const NCNNModelConfig& config, const fs::path& param_path) {
    const int c_in  = config.in_channels;
    const int c_mid = config.mid_channels;
    const int c_out = computeSubPixelChannels(config.in_channels, config.scale);

    constexpr int k1 = 5, k2 = 3, k3 = 3;
    const size_t w1_count = computeConvWeightsCount(c_in, c_mid, k1);
    const size_t w2_count = computeConvWeightsCount(c_mid, c_mid, k2);
    const size_t w3_count = computeConvWeightsCount(c_mid, c_out, k3);

    std::ofstream param_file(param_path);
    if (!param_file.is_open()) {
        std::cerr << std::format("[bawal::ml] Error: Failed to open param file for writing: {}\n", param_path.string());
        return false;
    }

    const std::string content = std::format(
        "7767517\n"
        "7 7\n"
        "Input            data             0 1 data\n"
        "Convolution      conv1            1 1 data conv1 0={} 1={} 2=1 3=1 4={} 5=1 6={}\n"
        "ReLU             relu1            1 1 conv1 conv1_act 0={:.6f}\n"
        "Convolution      conv2            1 1 conv1_act conv2 0={} 1={} 2=1 3=1 4={} 5=1 6={}\n"
        "ReLU             relu2            1 1 conv2 conv2_act 0={:.6f}\n"
        "Convolution      conv3            1 1 conv2_act conv3 0={} 1={} 2=1 3=1 4={} 5=1 6={}\n"
        "PixelShuffle     output           1 1 conv3 output 0={}\n",
        c_mid, k1, k1 / 2, w1_count,
        config.leaky_relu_slope,
        c_mid, k2, k2 / 2, w2_count,
        config.leaky_relu_slope,
        c_out, k3, k3 / 2, w3_count,
        config.scale
    );

    param_file << content;
    param_file.close();
    return true;
}

bool NCNNGenerator::generateBinFile(const NCNNModelConfig& config, const fs::path& bin_path) {
    std::ofstream bin_file(bin_path, std::ios::binary);
    if (!bin_file.is_open()) {
        std::cerr << std::format("[bawal::ml] Error: Failed to open bin file for writing: {}\n", bin_path.string());
        return false;
    }

    const int c_in  = config.in_channels;
    const int c_mid = config.mid_channels;
    const int c_out = computeSubPixelChannels(config.in_channels, config.scale);
    const int scale = config.scale;

    constexpr int k1 = 5, k2 = 3, k3 = 3;

    // Standard NCNN float32 weight flag (0x00000000 specifies raw 32-bit floating-point data)
    constexpr uint32_t NCNN_FLOAT32_FLAG = 0x00000000;

    auto append_conv_layer = [&](int in_c, int out_c, int k, bool is_subpixel_head) {
        bin_file.write(reinterpret_cast<const char*>(&NCNN_FLOAT32_FLAG), sizeof(uint32_t));

        const size_t total_weights = computeConvWeightsCount(in_c, out_c, k);
        std::vector<float> weights(total_weights, 0.0f);
        const int mid = k / 2;

        if (is_subpixel_head) {
            // Bilinear Sub-Pixel Expansion Kernel (Maps in_c to in_c * scale^2)
            for (int oc = 0; oc < out_c; ++oc) {
                const int color_ch = (oc / (scale * scale)) % in_c;
                const int sub_idx  = oc % (scale * scale);
                const int sub_y    = sub_idx / scale;
                const int sub_x    = sub_idx % scale;

                const float wx = (sub_x == 0) ? 0.75f : 0.25f;
                const float wy = (sub_y == 0) ? 0.75f : 0.25f;

                const size_t center_idx = (oc * in_c + color_ch) * (k * k) + (mid * k + mid);
                weights[center_idx] = wx * wy;

                if (sub_x > 0 && mid + 1 < k) {
                    weights[(oc * in_c + color_ch) * (k * k) + (mid * k + mid + 1)] = (1.0f - wx) * wy;
                }
                if (sub_y > 0 && mid + 1 < k) {
                    weights[(oc * in_c + color_ch) * (k * k) + ((mid + 1) * k + mid)] = wx * (1.0f - wy);
                }
            }
        } else {
            // Gaussian Sharpening Feature Extraction Kernel
            const float center_v = (k == 5) ? 0.40f : 0.60f;
            const float border_v = (1.0f - center_v) / static_cast<float>(k * k - 1);

            for (int oc = 0; oc < out_c; ++oc) {
                const int ic = oc % in_c;
                for (int y = 0; y < k; ++y) {
                    for (int x = 0; x < k; ++x) {
                        const size_t idx = (oc * in_c + ic) * (k * k) + (y * k + x);
                        weights[idx] = (y == mid && x == mid) ? center_v : border_v;
                    }
                }
            }
        }

        // C++20 std::as_bytes with std::span
        const auto weight_bytes = std::as_bytes(std::span{weights});
        bin_file.write(reinterpret_cast<const char*>(weight_bytes.data()), weight_bytes.size());

        // Write zero bias vector via std::span
        std::vector<float> biases(out_c, 0.0f);
        const auto bias_bytes = std::as_bytes(std::span{biases});
        bin_file.write(reinterpret_cast<const char*>(bias_bytes.data()), bias_bytes.size());
    };

    append_conv_layer(c_in, c_mid, k1, false);
    append_conv_layer(c_mid, c_mid, k2, false);
    append_conv_layer(c_mid, c_out, k3, true);

    bin_file.close();
    return true;
}

std::vector<fs::path> NCNNGenerator::syncWeights(
    const fs::path& param_path,
    const fs::path& bin_path,
    std::span<const fs::path> sync_dirs
) {
    std::vector<fs::path> synced;
    for (const auto& dir : sync_dirs) {
        try {
            if (dir.string() == "/app/weights" && !fs::exists("/app")) {
                continue;
            }
            fs::create_directories(dir);
            const auto dest_param = dir / param_path.filename();
            const auto dest_bin   = dir / bin_path.filename();
            std::error_code ec;
            if (!fs::equivalent(param_path, dest_param, ec)) {
                fs::copy_file(param_path, dest_param, fs::copy_options::overwrite_existing);
            }
            if (!fs::equivalent(bin_path, dest_bin, ec)) {
                fs::copy_file(bin_path, dest_bin, fs::copy_options::overwrite_existing);
            }
            synced.push_back(dest_param);
            synced.push_back(dest_bin);
        } catch (const std::exception& e) {
            std::cerr << std::format("[bawal::ml] Warning: Sync to {} failed: {}\n", dir.string(), e.what());
        }
    }
    return synced;
}

NCNNGenerationResult NCNNGenerator::generate(const NCNNModelConfig& config) {
    NCNNGenerationResult result;
    fs::create_directories(config.output_dir);

    result.param_path = config.output_dir / std::format("{}.param", config.model_name);
    result.bin_path   = config.output_dir / std::format("{}.bin", config.model_name);

    if (!generateParamFile(config, result.param_path)) {
        return result;
    }

    if (!generateBinFile(config, result.bin_path)) {
        return result;
    }

    result.param_size_bytes = fs::file_size(result.param_path);
    result.bin_size_bytes   = fs::file_size(result.bin_path);
    result.synced_paths     = syncWeights(result.param_path, result.bin_path, config.sync_dirs);
    result.success          = true;

    return result;
}

} // namespace bawal::ml

