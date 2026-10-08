#include "bawal/ml/ncnn_generator.hpp"

#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstring>

namespace fs = std::filesystem;

namespace bawal::ml {

bool NCNNGenerator::generateParamFile(const NCNNModelConfig& config, const fs::path& param_path) {
    const int c_in  = config.in_channels;
    const int c_mid = config.mid_channels;
    const int c_out = config.in_channels * (config.scale * config.scale);

    const int k1 = 5, k2 = 3, k3 = 3;
    const size_t w1_count = c_mid * c_in * k1 * k1;
    const size_t w2_count = c_mid * c_mid * k2 * k2;
    const size_t w3_count = c_out * c_mid * k3 * k3;

    std::ofstream param_file(param_path);
    if (!param_file.is_open()) {
        std::cerr << "[bawal::ml] Error: Failed to open param file for writing: " << param_path << "\n";
        return false;
    }

    param_file << "7767517\n"
               << "7 7\n"
               << "Input            data             0 1 data\n"
               << "Convolution      conv1            1 1 data conv1 0=" << c_mid << " 1=" << k1 << " 2=1 3=1 4=" << (k1 / 2) << " 5=1 6=" << w1_count << "\n"
               << "ReLU             relu1            1 1 conv1 conv1_act 0=" << config.leaky_relu_slope << "\n"
               << "Convolution      conv2            1 1 conv1_act conv2 0=" << c_mid << " 1=" << k2 << " 2=1 3=1 4=" << (k2 / 2) << " 5=1 6=" << w2_count << "\n"
               << "ReLU             relu2            1 1 conv2 conv2_act 0=" << config.leaky_relu_slope << "\n"
               << "Convolution      conv3            1 1 conv2_act conv3 0=" << c_out << " 1=" << k3 << " 2=1 3=1 4=" << (k3 / 2) << " 5=1 6=" << w3_count << "\n"
               << "PixelShuffle     output           1 1 conv3 output 0=" << config.scale << "\n";
    param_file.close();
    return true;
}

bool NCNNGenerator::generateBinFile(const NCNNModelConfig& config, const fs::path& bin_path) {
    std::ofstream bin_file(bin_path, std::ios::binary);
    if (!bin_file.is_open()) {
        std::cerr << "[bawal::ml] Error: Failed to open bin file for writing: " << bin_path << "\n";
        return false;
    }

    const int c_in  = config.in_channels;
    const int c_mid = config.mid_channels;
    const int c_out = config.in_channels * (config.scale * config.scale);
    const int scale = config.scale;

    const int k1 = 5, k2 = 3, k3 = 3;

    // Standard NCNN float32 weight flag (0x00000000 specifies raw 32-bit floating-point data)
    const uint32_t NCNN_FLOAT32_FLAG = 0x00000000;

    auto append_conv_layer = [&](int in_c, int out_c, int k, bool is_subpixel_head) {
        bin_file.write(reinterpret_cast<const char*>(&NCNN_FLOAT32_FLAG), sizeof(uint32_t));

        const size_t total_weights = static_cast<size_t>(out_c * in_c * k * k);
        std::vector<float> weights(total_weights, 0.0f);
        const int mid = k / 2;

        if (is_subpixel_head) {
            // Bilinear Sub-Pixel Expansion Kernel (Maps 3 channels to 12 channels)
            for (int oc = 0; oc < out_c; ++oc) {
                int color_ch = (oc / (scale * scale)) % in_c;
                int sub_idx  = oc % (scale * scale);
                int sub_y    = sub_idx / scale;
                int sub_x    = sub_idx % scale;

                float wx = (sub_x == 0) ? 0.75f : 0.25f;
                float wy = (sub_y == 0) ? 0.75f : 0.25f;

                size_t center_idx = (oc * in_c + color_ch) * (k * k) + (mid * k + mid);
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
                int ic = oc % in_c;
                for (int y = 0; y < k; ++y) {
                    for (int x = 0; x < k; ++x) {
                        size_t idx = (oc * in_c + ic) * (k * k) + (y * k + x);
                        weights[idx] = (y == mid && x == mid) ? center_v : border_v;
                    }
                }
            }
        }

        bin_file.write(reinterpret_cast<const char*>(weights.data()), weights.size() * sizeof(float));

        // Write zero bias vector
        std::vector<float> biases(out_c, 0.0f);
        bin_file.write(reinterpret_cast<const char*>(biases.data()), biases.size() * sizeof(float));
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
    const std::vector<fs::path>& sync_dirs
) {
    std::vector<fs::path> synced;
    for (const auto& dir : sync_dirs) {
        try {
            fs::create_directories(dir);
            auto dest_param = dir / param_path.filename();
            auto dest_bin   = dir / bin_path.filename();
            fs::copy_file(param_path, dest_param, fs::copy_options::overwrite_existing);
            fs::copy_file(bin_path, dest_bin, fs::copy_options::overwrite_existing);
            synced.push_back(dest_param);
            synced.push_back(dest_bin);
        } catch (const std::exception& e) {
            std::cerr << "[bawal::ml] Warning: Sync to " << dir << " failed: " << e.what() << "\n";
        }
    }
    return synced;
}

NCNNGenerationResult NCNNGenerator::generate(const NCNNModelConfig& config) {
    NCNNGenerationResult result;
    fs::create_directories(config.output_dir);

    result.param_path = config.output_dir / (config.model_name + ".param");
    result.bin_path   = config.output_dir / (config.model_name + ".bin");

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

