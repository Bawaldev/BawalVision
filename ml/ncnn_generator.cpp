#include "bawal/ml/ncnn_generator.hpp"
#include <iostream>

using namespace bawal::ml;

int main(int argc, char** argv) {
    NCNNModelConfig config;
    if (argc > 1) config.model_name = argv[1];
    if (argc > 2) config.scale = std::stoi(argv[2]);
    if (argc > 3) config.output_dir = argv[3];

    std::cout << "[bawal::ml::NCNNGenerator] Generating model: " << config.model_name 
              << " (" << config.scale << "x Scale, Sub-Pixel LeakyReLU Graph)\n";

    NCNNGenerationResult res = NCNNGenerator::generate(config);
    if (!res.success) {
        std::cerr << "[bawal::ml::NCNNGenerator] Error: Model synthesis failed!\n";
        return 1;
    }

    std::cout << "[✓] Param generated: " << res.param_path << " (" << res.param_size_bytes << " bytes)\n"
              << "[✓] Bin generated:   " << res.bin_path << " (" << res.bin_size_bytes << " bytes)\n";
    for (const auto& path : res.synced_paths) {
        std::cout << "    -> Synced: " << path << "\n";
    }

    return 0;
}