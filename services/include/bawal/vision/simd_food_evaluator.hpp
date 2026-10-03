#pragma once

#include <opencv2/core.hpp>
#include <string_view>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace bawal::vision {

struct SpoilageMetric {
    float discoloration_ratio{0.0f}; // 0.0 (fresh) - 1.0 (spoiled)
    float avg_luminance{0.0f};        // Average luminance (L / V channel)
    bool is_visually_spoiled{false};
};

/**
 * @brief Evaluates color degradation, hue anomalies, and freshness of detected food RoIs using SIMD (AVX2/NEON/OpenMP).
 * 
 * @param food_roi Cropped food bounding box BGR image matrix.
 * @param food_class Detected class name (e.g., "banana", "chicken", "egg", "orange", "rice", "spinach", "tempeh", "tofu", "water_spinach").
 * @return SpoilageMetric Computed spoilage metrics and visual pass/fail verdict.
 */
[[nodiscard]] SpoilageMetric evaluateSpoilageAVX2(const cv::Mat& food_roi, std::string_view food_class);

} // namespace bawal::vision