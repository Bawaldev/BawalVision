#pragma once

#include "bawal/vision/classifier.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace bawal::vision::image {

struct BatchItemResult {
    uint32_t index{0};
    std::string filename;
    std::string annotated_image_path;
    std::vector<bawal::vision::DetectionResult> detections;
    float surface_temp{62.5f};
    float t_safe_hours{4.0f};
    bool accepted{true};
};

struct BatchReportSummary {
    std::string batch_id;
    size_t total_images{0};
    size_t accepted_count{0};
    size_t rejected_count{0};
    double total_processing_time_ms{0.0};
    std::string master_pdf_path;
    std::vector<BatchItemResult> items;
};

class BatchInspector {
public:
    static constexpr size_t kMaxZipBytes = 15 * 1024 * 1024;      // Batas aman 15 MB
    static constexpr size_t kMaxExtractedBytes = 60 * 1024 * 1024; // Anti-decompression bomb 60 MB
    static constexpr size_t kMaxImageFiles = 30;                   // Batas 30 citra baki

    [[nodiscard]] static BatchReportSummary processZipBuffer(
        const uint8_t* zip_data,
        size_t zip_size,
        bawal::vision::FoodClassifier& classifier,
        const std::filesystem::path& template_path = "./services/templates/batch_report.tex.in");
};

} // namespace bawal::vision::image