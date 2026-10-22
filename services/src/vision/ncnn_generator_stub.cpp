#include "bawal/ml/ncnn_generator.hpp"

#include <iostream>

namespace bawal::ml {

NCNNGenerationResult NCNNGenerator::generate(const NCNNModelConfig&) {
    std::cerr << "[NCNN] Super-resolution model generator is disabled in this build; "
                 "supply a pre-converted NCNN model instead.\n";
    return {};
}

bool NCNNGenerator::generateParamFile(const NCNNModelConfig&,
                                      const std::filesystem::path&) {
    return false;
}

bool NCNNGenerator::generateBinFile(const NCNNModelConfig&,
                                    const std::filesystem::path&) {
    return false;
}

std::vector<std::filesystem::path> NCNNGenerator::syncWeights(
    const std::filesystem::path&,
    const std::filesystem::path&,
    std::span<const std::filesystem::path>) {
    return {};
}

} // namespace bawal::ml
