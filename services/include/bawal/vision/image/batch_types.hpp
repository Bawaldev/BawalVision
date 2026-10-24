#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bawal::vision::image {

/**
 * @brief Inspection taxonomy stamped on every batch artifact.
 *
 * The single-frame live pipeline is "VIDEO_LIVE"; operator ZIP uploads are
 * image-batch jobs and are recorded as "IMG_BATCH" in the JSON API, the
 * PostgreSQL audit trail and the master PDF report.
 */
inline constexpr std::string_view kBatchInspectionType = "IMG_BATCH";

/** Fixed class-name capacity, mirroring DetectionSoA::kClassNameBytes. */
inline constexpr std::size_t kBatchClassNameBytes = 32;

/** Grafana datasource identity shared by the JSON API, PDF and dashboard. */
inline constexpr std::string_view kGrafanaDatasource = "PostgreSQL (Grafana datasource)";

/** Provisioned Grafana dashboard deep-link embedded in reports. */
inline constexpr std::string_view kGrafanaDashboardUrl =
    "http://localhost:3002/d/bawalvision_analytics";

/** Per-detection quality verdict shared by the overlay, JSON API and PDF. */
enum class QualityStatus : std::uint8_t {
    Pass = 0,
    Review = 1,
    Reject = 2,
};

static_assert(sizeof(QualityStatus) == 1,
              "QualityStatus must be exactly 1 byte (SoA/JSON enum)");

[[nodiscard]] inline const char* qualityStatusName(QualityStatus status) noexcept {
    switch (status) {
        case QualityStatus::Pass: return "PASS";
        case QualityStatus::Review: return "REVIEW";
        case QualityStatus::Reject: return "REJECT";
    }
    return "UNKNOWN";
}

/**
 * @brief Copies into a fixed-size, always NUL-terminated char buffer.
 *
 * One bounded memcpy, no allocation: safe to use on hot aggregation paths and
 * in POD structs that must stay trivially inspectable.
 */
template <std::size_t N>
inline void assignFixedString(char (&destination)[N], std::string_view value) noexcept {
    static_assert(N > 1, "fixed string buffer needs room for the NUL terminator");
    const std::size_t length = value.size() < N - 1 ? value.size() : N - 1;
    if (length > 0) {
        std::memcpy(destination, value.data(), length);
    }
    destination[length] = '\0';
}

/** Bounded read of a fixed-size NUL-terminated char buffer. */
template <std::size_t N>
[[nodiscard]] inline std::string_view fixedStringView(const char (&data)[N]) noexcept {
    return std::string_view(data, strnlen(data, N));
}

/**
 * @brief Composite quality analytics folded over every detection in the batch.
 *
 * POD, no strings/vectors: the hot scalar aggregate is padded to exactly one
 * 64-byte cache line so per-tray and per-class folds never split a line.
 */
struct alignas(64) BatchQualityMetrics {
    float avg_similarity{0.0f};  ///< Mean YOLOv8 confidence in [0, 1].
    float avg_quality{0.0f};     ///< Mean SIMD freshness score in [0, 100].
    float min_quality{0.0f};     ///< Lowest single-detection freshness score.
    float max_quality{0.0f};     ///< Highest single-detection freshness score.
    float stddev_quality{0.0f};  ///< Population standard deviation of quality.
    float quality_index{0.0f};   ///< Weighted 0..100 composite, reject-penalised.
    uint32_t total_detections{0};
    uint32_t pass_detections{0};
    uint32_t review_detections{0};
    uint32_t reject_detections{0};
    uint32_t reserved[6]{0};     ///< Explicit pad: 40-byte payload -> 64-byte line.
};

static_assert(sizeof(BatchQualityMetrics) == 64,
              "BatchQualityMetrics must be exactly 64 bytes (1 cache line)");
static_assert(alignof(BatchQualityMetrics) == 64,
              "BatchQualityMetrics must be 64-byte aligned");

/**
 * @brief Aggregated detector confidence / freshness scores for one food class.
 *
 * POD with a fixed class-name buffer (like DetectionSoA) so the report table
 * can be walked as aligned 64-byte records without heap indirection.
 */
struct alignas(64) BatchClassStats {
    char class_name[kBatchClassNameBytes]{};  ///< 32 bytes, NUL-terminated.
    uint32_t detections{0};
    uint32_t pass_count{0};
    uint32_t review_count{0};
    uint32_t reject_count{0};
    float avg_similarity{0.0f};  ///< Mean YOLOv8 confidence in [0, 1].
    float avg_quality{0.0f};     ///< Mean SIMD freshness score in [0, 100].
    uint32_t reserved[2]{0};     ///< Explicit pad: 56-byte payload -> 64-byte line.
};

static_assert(sizeof(BatchClassStats) == 64,
              "BatchClassStats must be exactly 64 bytes (1 cache line)");
static_assert(alignof(BatchClassStats) == 64,
              "BatchClassStats must be 64-byte aligned");

/** One row of the fleet-wide per-class breakdown served to Grafana. */
struct alignas(64) GrafanaClassStat {
    char class_name[kBatchClassNameBytes]{};  ///< 32 bytes, NUL-terminated.
    uint64_t detections{0};                   ///< 8 bytes.
    double avg_similarity_pct{0.0};           ///< 8 bytes.
    double avg_spoilage_pct{0.0};             ///< 8 bytes.
    double pass_rate{0.0};                    ///< 8 bytes: payload is exactly 64.
};

static_assert(sizeof(GrafanaClassStat) == 64,
              "GrafanaClassStat must be exactly 64 bytes (1 cache line)");
static_assert(alignof(GrafanaClassStat) == 64,
              "GrafanaClassStat must be 64-byte aligned");

/** One day of the 7-day fleet pass-rate trend. Compact 24-byte POD. */
struct GrafanaDailyPoint {
    char day[8]{"--"};         ///< "MM-DD" label, NUL-terminated.
    uint64_t inspections{0};
    double pass_rate{0.0};
};

static_assert(sizeof(GrafanaDailyPoint) == 24,
              "GrafanaDailyPoint must stay a packed 24-byte POD (no padding added)");
static_assert(alignof(GrafanaDailyPoint) == 8,
              "GrafanaDailyPoint must be 8-byte aligned for uint64/double access");

/**
 * @brief Fleet-wide analytics mirrored from the PostgreSQL datasource that
 *        backs the provisioned Grafana dashboard (uid: bawalvision_analytics).
 *
 * Layout contract: a 64-byte scalar prefix (one cache line) followed by the
 * heap-backed vectors. `available == false` means PostgreSQL was unreachable
 * and the report renders the section as "offline" instead of failing the batch.
 *
 * No total-size assert: std::vector is ABI-sized, so only the scalar-prefix
 * offset and the standard-layout contract are validated.
 */
struct GrafanaAnalytics {
    uint64_t total_inspections{0};
    uint64_t inspections_24h{0};
    uint64_t tracked_devices{0};
    double avg_similarity_pct{0.0};
    double fresh_food_rate{0.0};
    double avg_spoilage_pct{0.0};
    bool available{false};
    uint8_t reserved[15]{0};  ///< Explicit pad: 49-byte payload -> 64-byte prefix.

    std::vector<GrafanaClassStat> class_stats;
    std::vector<GrafanaDailyPoint> daily;
};

static_assert(std::is_standard_layout_v<GrafanaAnalytics>,
              "GrafanaAnalytics must stay standard-layout for offsetof validation");
static_assert(offsetof(GrafanaAnalytics, class_stats) == 64,
              "GrafanaAnalytics scalar prefix must occupy exactly one 64-byte cache line");

} // namespace bawal::vision::image
