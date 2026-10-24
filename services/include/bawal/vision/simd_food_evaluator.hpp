#pragma once

#include <opencv2/core.hpp>
#include <string_view>
#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace bawal::vision {

struct alignas(4) SpoilageMetric {
    float discoloration_ratio{0.0f}; // 4 byte (0.0 fresh - 1.0 spoiled)
    float avg_luminance{0.0f};        // 4 byte (Average luminance in L / V channel)
    bool is_visually_spoiled{false};  // 1 byte
    uint8_t reserved[3]{0, 0, 0};     // 3 byte explicit padding (12 bytes total)
};

struct alignas(4) TextureMetric {
    float edge_energy{0.0f};           // Mean spatial gradient across ROI (4 byte)
    float std_dev{0.0f};               // Standard deviation of luminance (4 byte)
    bool is_valid_food_texture{false}; // True if ROI contains genuine food micro-texture/contrast (1 byte)
    uint8_t reserved[3]{0, 0, 0};      // 3 byte explicit padding (12 bytes total)
};

static_assert(sizeof(SpoilageMetric) == 12, "SpoilageMetric must be exactly 12 bytes");
static_assert(alignof(SpoilageMetric) == 4, "SpoilageMetric must be 4-byte aligned");
static_assert(sizeof(TextureMetric) == 12, "TextureMetric must be exactly 12 bytes");
static_assert(alignof(TextureMetric) == 4, "TextureMetric must be 4-byte aligned");

/**
 * @brief Evaluates spatial gradient energy and luminance variance using AVX2 SIMD to reject flat walls and smooth backgrounds.
 * 
 * @param food_roi Cropped food bounding box BGR image matrix.
 * @return TextureMetric Computed edge energy, standard deviation, and validity verdict.
 */
[[nodiscard]] TextureMetric evaluateTextureAVX2(const cv::Mat& food_roi);

/**
 * @brief Evaluates color degradation, hue anomalies, and freshness of detected food RoIs using SIMD (AVX2/NEON/OpenMP).
 * 
 * @param food_roi Cropped food bounding box BGR image matrix.
 * @param food_class Detected class name (e.g., "banana", "chicken", "egg", "orange", "rice", "spinach", "tempeh", "tofu", "water_spinach", "apple").
 * @return SpoilageMetric Computed spoilage metrics and visual pass/fail verdict.
 */
[[nodiscard]] SpoilageMetric evaluateSpoilageAVX2(const cv::Mat& food_roi, std::string_view food_class);

/**
 * @brief Batch quality vector statistics reduced in one SIMD pass.
 *
 * Used by the ZIP batch inspector to fold hundreds of detection scores into
 * per-tray, per-class and per-batch aggregates without scalar loops.
 */
struct QualityVectorStats {
    float mean_similarity{0.0f}; ///< Mean detector confidence in [0, 1].
    float mean_quality{0.0f};    ///< Mean SIMD freshness score in [0, 100].
    float std_quality{0.0f};     ///< Population standard deviation of quality.
    float min_quality{0.0f};     ///< Lowest freshness score in the window.
    float max_quality{0.0f};     ///< Highest freshness score in the window.
};

static_assert(sizeof(QualityVectorStats) == 20,
              "QualityVectorStats must stay a packed 5 x float POD: no padding added");
static_assert(alignof(QualityVectorStats) == 4,
              "QualityVectorStats must be 4-byte aligned (scalar float access only)");

/**
 * @brief Reduces parallel similarity/quality score arrays with AVX2 (8 floats
 *        per iteration) or an OpenMP SIMD fallback on non-AVX2 targets.
 *
 * Both spans must hold `count` floats; they are read with unaligned loads and
 * never written. Returns zero-initialized statistics when `count == 0`.
 *
 * @param similarity Contiguous detector confidence array.
 * @param quality Contiguous freshness score array in [0, 100].
 * @param count Number of valid entries in both arrays.
 * @return QualityVectorStats Mean/stddev/min/max quality and mean similarity.
 */
[[nodiscard]] QualityVectorStats computeQualityStatsSIMD(const float* similarity,
                                                         const float* quality,
                                                         size_t count);

} // namespace bawal::vision
