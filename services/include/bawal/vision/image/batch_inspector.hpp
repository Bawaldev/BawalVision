#pragma once

#include "bawal/vision/classifier.hpp"
#include "bawal/vision/image/batch_types.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

namespace bawal::vision::image {

/**
 * @brief Classifies one detection into PASS / REVIEW / REJECT.
 *
 * Single source of truth for the batch overlay, the JSON API and the LaTeX
 * report so all three artifacts can never disagree on a verdict.
 */
[[nodiscard]] inline QualityStatus classifyQuality(
    const bawal::vision::DetectionResult& detection) noexcept {
    const float spoilage_pct =
        std::clamp(detection.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);
    if (detection.spoilage.is_visually_spoiled || spoilage_pct >= 45.0f) {
        return QualityStatus::Reject;
    }
    if (spoilage_pct >= 25.0f || detection.confidence < 0.70f) {
        return QualityStatus::Review;
    }
    return QualityStatus::Pass;
}

/** SIMD freshness score in [0, 100] (100 = pristine, 0 = fully discoloured). */
[[nodiscard]] inline float freshnessScore(
    const bawal::vision::DetectionResult& detection) noexcept {
    return std::clamp(
        100.0f - detection.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);
}

/**
 * @brief One inspected tray.
 *
 * Layout contract: 8-byte members first (strings/vector), then the scalar
 * metadata, then the 1-byte verdict flag and an explicit tail pad, so no
 * padding holes sit between the scalars. Not size-asserted because std::string
 * and std::vector are ABI-sized; standard-layout is validated instead.
 */
struct BatchItemResult {
    std::string filename;
    std::string annotated_image_path;
    std::vector<bawal::vision::DetectionResult> detections;

    uint32_t index{0};
    uint32_t pass_detections{0};
    uint32_t review_detections{0};
    uint32_t reject_detections{0};
    float surface_temp{62.5f};
    float t_safe_hours{4.0f};
    float avg_similarity{0.0f};
    float avg_quality{0.0f};
    bool accepted{true};
    uint8_t reserved[7]{0}; ///< Explicit tail pad after the 1-byte flag.
};

static_assert(std::is_standard_layout_v<BatchItemResult>,
              "BatchItemResult must stay standard-layout for stable field offsets");

/**
 * @brief Completed batch audit payload.
 *
 * Layout contract: the 64-byte `BatchQualityMetrics` POD leads (it forces the
 * struct to 64-byte alignment), then 8-byte containers/strings, then scalar
 * metadata and an explicit tail pad. Returned by value; move-friendly because
 * the heavy members are heap-backed.
 */
struct BatchReportSummary {
    BatchQualityMetrics quality; ///< 64-byte aligned POD, first for cache-line start.

    GrafanaAnalytics grafana;
    std::vector<BatchClassStats> class_breakdown;
    std::vector<uint32_t> quality_histogram; ///< 10 buckets: [0-10) .. [90-100].
    std::vector<BatchItemResult> items;

    std::string batch_id;
    std::string inspection_type{std::string(kBatchInspectionType)};
    std::string generated_at;   ///< ISO-8601 UTC completion timestamp.
    std::string model_arch;     ///< Active ONNX architecture, e.g. YOLOv8s-INT8.
    std::string engine;         ///< Human-readable compute engine string.
    std::string master_pdf_path;
    std::string master_tex_path;
    std::string report_checksum; ///< FNV-1a 64 of the batch payload.

    size_t total_images{0};
    size_t accepted_count{0};
    size_t rejected_count{0};
    double total_processing_time_ms{0.0};
    double throughput_images_per_sec{0.0};
    int omp_threads{1};        ///< OpenMP worker threads used by the batch job.
    uint8_t reserved[4]{0};    ///< Explicit tail pad after the 4-byte int.
};

static_assert(std::is_standard_layout_v<BatchReportSummary>,
              "BatchReportSummary must stay standard-layout for stable field offsets");
static_assert(alignof(BatchReportSummary) == 64,
              "BatchReportSummary must inherit the 64-byte quality-metrics alignment");

class BatchInspector {
public:
    static constexpr size_t kMaxZipBytes = 15 * 1024 * 1024;      // Batas aman 15 MB
    static constexpr size_t kMaxExtractedBytes = 60 * 1024 * 1024; // Anti-decompression bomb 60 MB
    static constexpr size_t kMaxImageFiles = 30;                   // Batas 30 citra baki

    /**
     * @brief Extracts, inspects and reports a ZIP archive of food tray photos.
     *
     * @param zip_data Raw ZIP bytes (already size-checked by the HTTP layer).
     * @param zip_size Number of valid bytes in zip_data.
     * @param classifier Dedicated batch FoodClassifier (never the live one).
     * @param template_path LaTeX template with #@PLACEHOLDER@ tokens.
     * @param grafana Optional fleet analytics embedded as a report section.
     * @return BatchReportSummary Fully populated summary (empty on failure).
     */
    [[nodiscard]] static BatchReportSummary processZipBuffer(
        const uint8_t* zip_data,
        size_t zip_size,
        bawal::vision::FoodClassifier& classifier,
        const std::filesystem::path& template_path = "./services/templates/batch_report.tex.in",
        const GrafanaAnalytics* grafana = nullptr);

private:
    /**
     * @brief Mutable per-class accumulator used while folding detections.
     *
     * Lives in the header (not as a function-local struct in the .cpp) so the
     * aggregation layout stays inspectable. Heap-backed vectors, so no
     * cache-line padding is added: only the layout contract is validated.
     */
    struct ClassAccumulator {
        std::vector<float> similarities;
        std::vector<float> qualities;
        uint32_t pass{0};
        uint32_t review{0};
        uint32_t reject{0};
    };

    static_assert(std::is_standard_layout_v<ClassAccumulator>,
                  "ClassAccumulator must stay standard-layout for stable offsets");
};

} // namespace bawal::vision::image
