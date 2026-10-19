#include "bawal/vision/dark_detector.hpp"
#include "bawal/vision/dark_detector_hwy.hpp"
#include <algorithm>
#include <cmath>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace bawal::vision {

/**
 * @brief Detects underexposure/dark frames using AVX2 vectorization on BGR green channel.
 *
 * @param bgr_frame The input BGR image frame.
 * @param dark_thresh Luminance intensity threshold below which a pixel is considered dark.
 * @param dark_ratio_thresh Ratio of underexposed pixels required to classify the frame as dark.
 * @return DarkDetectionResult Metrics containing luminance, dark pixel ratio, flag, and gain.
 */
DarkDetectionResult detectDarkFrameAVX2(const cv::Mat& bgr_frame, 
                                       uint8_t dark_thresh, 
                                       float dark_ratio_thresh) {
    DarkDetectionResult result{};
    if (bgr_frame.empty() || bgr_frame.cols <= 0 || bgr_frame.rows <= 0) {
        return result;
    }

    const int width = bgr_frame.cols;
    const int height = bgr_frame.rows;
    const int total_pixels = width * height;

    uint64_t total_luminance_sum = 0;
    uint32_t total_dark_pixels = 0;

#if defined(__AVX2__)
    // Initialize AVX2 constant vectors
    const __m256i thresh_vec = _mm256_set1_epi8(static_cast<char>(dark_thresh));
    const __m256i zero_vec   = _mm256_setzero_si256();

    // Shuffle mask to extract the Green (G) channel from 48 bytes (16 BGR pixels)
    // Green channel offset pattern in interleaved BGR: 1, 4, 7, 10, 13, 16, ...
    const __m128i m0_g = _mm_setr_epi8(1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i m1_g = _mm_setr_epi8(-1, -1, -1, -1, -1, 0, 3, 6, 9, 12, 15, -1, -1, -1, -1, -1);
    const __m128i m2_g = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 2, 5, 8, 11, 14);

    for (int y = 0; y < height; ++y) {
        const uint8_t* row_ptr = bgr_frame.ptr<uint8_t>(y);
        int x = 0;

        for (; x <= width - 32; x += 32) {
            // --- Block 1: First 16 pixels (48 BGR bytes -> 16 Green bytes) ---
            __m128i in0_a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + x * 3));
            __m128i in1_a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + x * 3 + 16));
            __m128i in2_a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + x * 3 + 32));

            __m128i g0_a = _mm_shuffle_epi8(in0_a, m0_g);
            __m128i g1_a = _mm_shuffle_epi8(in1_a, m1_g);
            __m128i g2_a = _mm_shuffle_epi8(in2_a, m2_g);
            __m128i green_16_a = _mm_or_si128(_mm_or_si128(g0_a, g1_a), g2_a);

            // --- Block 2: Next 16 pixels (48 BGR bytes -> 16 Green bytes) ---
            __m128i in0_b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + (x + 16) * 3));
            __m128i in1_b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + (x + 16) * 3 + 16));
            __m128i in2_b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + (x + 16) * 3 + 32));

            __m128i g0_b = _mm_shuffle_epi8(in0_b, m0_g);
            __m128i g1_b = _mm_shuffle_epi8(in1_b, m1_g);
            __m128i g2_b = _mm_shuffle_epi8(in2_b, m2_g);
            __m128i green_16_b = _mm_or_si128(_mm_or_si128(g0_b, g1_b), g2_b);

            // Combine into one 256-bit YMM register (32 bytes of Green intensities)
            __m256i green_32 = _mm256_set_m128i(green_16_b, green_16_a);

            // 1. Accumulate total luminance (Unpack 8-bit -> 16-bit -> 32-bit to prevent overflow)
            __m256i low_16  = _mm256_unpacklo_epi8(green_32, zero_vec);
            __m256i high_16 = _mm256_unpackhi_epi8(green_32, zero_vec);

            __m256i sum_32a = _mm256_add_epi32(_mm256_unpacklo_epi16(low_16, zero_vec), 
                                               _mm256_unpackhi_epi16(low_16, zero_vec));
            __m256i sum_32b = _mm256_add_epi32(_mm256_unpacklo_epi16(high_16, zero_vec), 
                                               _mm256_unpackhi_epi16(high_16, zero_vec));
            __m256i sum_all = _mm256_add_epi32(sum_32a, sum_32b);

            alignas(32) int32_t temp_sums[8];
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(temp_sums), sum_all);
            for (int i = 0; i < 8; ++i) {
                total_luminance_sum += static_cast<uint64_t>(temp_sums[i]);
            }

            __m256i diff = _mm256_subs_epu8(thresh_vec, green_32);
            __m256i mask_dark = _mm256_cmpgt_epi8(diff, zero_vec);

            uint32_t bitmask = static_cast<uint32_t>(_mm256_movemask_epi8(mask_dark));
            total_dark_pixels += _mm_popcnt_u32(bitmask);
        }

        // Scalar remainder for pixels at row end
        for (; x < width; ++x) {
            uint8_t g = row_ptr[x * 3 + 1];
            total_luminance_sum += g;
            if (g < dark_thresh) {
                total_dark_pixels++;
            }
        }
    }
#else
    // Fallback scalar scan if AVX2 is not enabled at compile time
    for (int y = 0; y < height; ++y) {
        const uint8_t* row_ptr = bgr_frame.ptr<uint8_t>(y);
        for (int x = 0; x < width; ++x) {
            uint8_t g = row_ptr[x * 3 + 1];
            total_luminance_sum += g;
            if (g < dark_thresh) {
                total_dark_pixels++;
            }
        }
    }
#endif

    // Compute ratios and darkness decision
    const float inv_total = 1.0f / static_cast<float>(total_pixels);
    result.mean_luminance = static_cast<float>(total_luminance_sum) * inv_total;
    result.underexposed_ratio = static_cast<float>(total_dark_pixels) * inv_total;

    // Dark condition check: mean luminance below threshold OR underexposed ratio exceeds limit
    if (result.mean_luminance < static_cast<float>(dark_thresh) || 
        result.underexposed_ratio > dark_ratio_thresh) {
        result.is_dark = 1;
        // Compute adaptive exposure boost gain (range 1.1x to 2.5x)
        result.suggested_boost_gain = std::clamp(128.0f / std::max(15.0f, result.mean_luminance), 1.1f, 2.5f);
    } else {
        result.is_dark = 0;
        result.suggested_boost_gain = 1.0f;
    }

    return result;
}

/**
 * @brief Evaluates dark frame conditions using the AVX2 specialized path.
 *
 * @param bgr_frame The input BGR frame to analyze.
 * @param dark_thresh Luminance threshold for dark pixel classification.
 * @param dark_ratio_thresh Ratio threshold to classify the frame as dark.
 * @return DarkDetectionResult Darkness metrics and recommended boost gain.
 */
DarkDetectionResult DarkDetector::detectDarkFrameAVX2(const cv::Mat& bgr_frame, 
                                                     uint8_t dark_thresh, 
                                                     float dark_ratio_thresh) const {
    return bawal::vision::detectDarkFrameAVX2(bgr_frame, dark_thresh, dark_ratio_thresh);
}

/**
 * @brief Analyzes frame illumination and detects dark conditions using SIMD/Highway acceleration.
 *
 * @param bgr_frame Input BGR image matrix.
 * @param dark_thresh Luminance threshold below which pixels are considered dark.
 * @param dark_ratio_thresh Underexposed pixel ratio threshold to trigger dark status.
 * @return DarkDetectionResult Calculated illumination metrics and gain recommendation.
 */
DarkDetectionResult DarkDetector::detect(const cv::Mat& bgr_frame, 
                                         uint8_t dark_thresh, 
                                         float dark_ratio_thresh) const {
#if defined(__AVX2__)
    return detectDarkFrameAVX2(bgr_frame, dark_thresh, dark_ratio_thresh);
#else
    DarkDetectionResult result{};
    if (bgr_frame.empty() || bgr_frame.cols <= 0 || bgr_frame.rows <= 0) {
        return result;
    }

    const size_t width = static_cast<size_t>(bgr_frame.cols);
    const size_t height = static_cast<size_t>(bgr_frame.rows);
    const size_t total_pixels = width * height;

    uint64_t total_luminance_sum = 0;
    uint32_t total_dark_pixels = 0;

    for (size_t y = 0; y < height; ++y) {
        const uint8_t* row_ptr = bgr_frame.ptr<uint8_t>(static_cast<int>(y));
        detail::DispatchScanDarkRow(row_ptr, width, dark_thresh, total_luminance_sum, total_dark_pixels);
    }

    const float inv_total = 1.0f / static_cast<float>(total_pixels);
    result.mean_luminance = static_cast<float>(total_luminance_sum) * inv_total;
    result.underexposed_ratio = static_cast<float>(total_dark_pixels) * inv_total;

    if (result.mean_luminance < static_cast<float>(dark_thresh) || 
        result.underexposed_ratio > dark_ratio_thresh) {
        result.is_dark = 1;
        result.suggested_boost_gain = std::clamp(128.0f / std::max(15.0f, result.mean_luminance), 1.1f, 2.5f);
    } else {
        result.is_dark = 0;
        result.suggested_boost_gain = 1.0f;
    }

    return result;
#endif
}

} // namespace bawal::vision