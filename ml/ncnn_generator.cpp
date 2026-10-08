#include "bawal/ml/ncnn_generator.hpp"

#include <iostream>
#include <string_view>
#include <span>
#include <format>

using namespace bawal::ml;

int main(int argc, char** argv) {
    const std::span<char*> args(argv, argc);

    if (args.size() > 1) {
        const std::string_view first_arg{args[1]};
        if (first_arg == "--help" || first_arg == "-h") {
            std::cout << std::format("Usage: {} [model_name] [scale] [output_dir]\n"
                                     "Example: {} realesr-animevideov3-x2 2 weights\n",
                                     args[0], args[0]);
            return 0;
        }
    }

    NCNNModelConfig config;
    if (args.size() > 1) config.model_name = args[1];
    if (args.size() > 2) config.scale = std::stoi(args[2]);
    if (args.size() > 3) config.output_dir = args[3];

    std::cout << std::format("[bawal::ml::NCNNGenerator] Generating model: {} ({}x Scale, Sub-Pixel LeakyReLU Graph)\n",
                             config.model_name, config.scale);

    const NCNNGenerationResult res = NCNNGenerator::generate(config);
    if (!res.success) {
        std::cerr << "[bawal::ml::NCNNGenerator] Error: Model synthesis failed!\n";
        return 1;
    }

    std::cout << std::format("[✓] Param generated: {} ({} bytes)\n"
                             "[✓] Bin generated:   {} ({} bytes)\n",
                             res.param_path.string(), res.param_size_bytes,
                             res.bin_path.string(), res.bin_size_bytes);

    for (const auto& path : res.synced_paths) {
        std::cout << std::format("    -> Synced: {}\n", path.string());
    }

    return 0;
}