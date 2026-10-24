#include "bawal/vision/dark_detector.hpp"
<<<<<<< HEAD
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
 * Simultaneously evaluates blown-out pixels (RGB > blown_thresh) using hardware SIMD mask
 * operations. If the blown-out ratio exceeds blown_ratio_thresh, suppresses the blind 2.5x gain
 * multiplier to prevent misclassifying localized glare/backlight hotspots as low-light scenes.
 *
 * @param bgr_frame The input BGR image frame (CV_8UC3).
 * @param dark_thresh Luminance intensity threshold below which a pixel is considered dark.
 * @param dark_ratio_thresh Ratio of underexposed pixels required to classify the frame as dark.
 * @param blown_thresh Intensity threshold above which all RGB channels are considered blown-out.
 * @param blown_ratio_thresh Blown-out pixel ratio threshold above which backlight conflict suppresses 2.5x gain.
 * @return DarkDetectionResult Metrics containing luminance, dark pixel ratio, hotspot flag, and gain.
 */
DarkDetectionResult detectDarkFrameAVX2(const cv::Mat& bgr_frame, 
                                       uint8_t dark_thresh, 
                                       float dark_ratio_thresh,
                                       uint8_t blown_thresh,
                                       float blown_ratio_thresh) {
    DarkDetectionResult result{};
    if (bgr_frame.empty() || bgr_frame.cols <= 0 || bgr_frame.rows <= 0) {
        return result;
    }

    const int width = bgr_frame.cols;
    const int height = bgr_frame.rows;
    const int total_pixels = width * height;

    uint64_t total_luminance_sum = 0;
    uint32_t total_dark_pixels = 0;
    uint32_t total_blown_pixels = 0;

#if defined(__AVX2__)
    // Initialize AVX2 constant vectors
    const __m256i thresh_vec = _mm256_set1_epi8(static_cast<char>(dark_thresh));
    const __m256i zero_vec   = _mm256_setzero_si256();
    const __m128i zero_vec_128 = _mm_setzero_si128();
    const __m128i blown_vec_128 = _mm_set1_epi8(static_cast<char>(blown_thresh));

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

            // Blown-out evaluation for Block 1 (RGB > blown_thresh on all 3 channels)
            const uint32_t b0_a = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(_mm_subs_epu8(in0_a, blown_vec_128), zero_vec_128)));
            const uint32_t b1_a = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(_mm_subs_epu8(in1_a, blown_vec_128), zero_vec_128)));
            const uint32_t b2_a = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(_mm_subs_epu8(in2_a, blown_vec_128), zero_vec_128)));
            const uint64_t mask48_a = static_cast<uint64_t>(b0_a) | 
                                      (static_cast<uint64_t>(b1_a) << 16) | 
                                      (static_cast<uint64_t>(b2_a) << 32);
            const uint64_t blown_a = mask48_a & (mask48_a >> 1) & (mask48_a >> 2) & 0x249249249249ULL;
#if defined(_M_X64) || defined(__x86_64__)
            total_blown_pixels += _mm_popcnt_u64(blown_a);
#else
            total_blown_pixels += _mm_popcnt_u32(static_cast<uint32_t>(blown_a)) + 
                                  _mm_popcnt_u32(static_cast<uint32_t>(blown_a >> 32));
#endif

            // --- Block 2: Next 16 pixels (48 BGR bytes -> 16 Green bytes) ---
            __m128i in0_b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + (x + 16) * 3));
            __m128i in1_b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + (x + 16) * 3 + 16));
            __m128i in2_b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row_ptr + (x + 16) * 3 + 32));

            __m128i g0_b = _mm_shuffle_epi8(in0_b, m0_g);
            __m128i g1_b = _mm_shuffle_epi8(in1_b, m1_g);
            __m128i g2_b = _mm_shuffle_epi8(in2_b, m2_g);
            __m128i green_16_b = _mm_or_si128(_mm_or_si128(g0_b, g1_b), g2_b);

            // Blown-out evaluation for Block 2 (RGB > blown_thresh on all 3 channels)
            const uint32_t b0_b = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(_mm_subs_epu8(in0_b, blown_vec_128), zero_vec_128)));
            const uint32_t b1_b = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(_mm_subs_epu8(in1_b, blown_vec_128), zero_vec_128)));
            const uint32_t b2_b = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(_mm_subs_epu8(in2_b, blown_vec_128), zero_vec_128)));
            const uint64_t mask48_b = static_cast<uint64_t>(b0_b) | 
                                      (static_cast<uint64_t>(b1_b) << 16) | 
                                      (static_cast<uint64_t>(b2_b) << 32);
            const uint64_t blown_b = mask48_b & (mask48_b >> 1) & (mask48_b >> 2) & 0x249249249249ULL;
#if defined(_M_X64) || defined(__x86_64__)
            total_blown_pixels += _mm_popcnt_u64(blown_b);
#else
            total_blown_pixels += _mm_popcnt_u32(static_cast<uint32_t>(blown_b)) + 
                                  _mm_popcnt_u32(static_cast<uint32_t>(blown_b >> 32));
#endif

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
            const uint8_t b = row_ptr[x * 3 + 0];
            const uint8_t g = row_ptr[x * 3 + 1];
            const uint8_t r = row_ptr[x * 3 + 2];
            total_luminance_sum += g;
            if (g < dark_thresh) {
                total_dark_pixels++;
            }
            if (b > blown_thresh && g > blown_thresh && r > blown_thresh) {
                total_blown_pixels++;
            }
        }
    }
#else
    // Fallback scalar scan if AVX2 is not enabled at compile time
    for (int y = 0; y < height; ++y) {
        const uint8_t* row_ptr = bgr_frame.ptr<uint8_t>(y);
        for (int x = 0; x < width; ++x) {
            const uint8_t b = row_ptr[x * 3 + 0];
            const uint8_t g = row_ptr[x * 3 + 1];
            const uint8_t r = row_ptr[x * 3 + 2];
            total_luminance_sum += g;
            if (g < dark_thresh) {
                total_dark_pixels++;
            }
            if (b > blown_thresh && g > blown_thresh && r > blown_thresh) {
                total_blown_pixels++;
            }
        }
    }
#endif

    // Compute ratios and darkness decision
    const float inv_total = 1.0f / static_cast<float>(total_pixels);
    result.mean_luminance = static_cast<float>(total_luminance_sum) * inv_total;
    result.underexposed_ratio = static_cast<float>(total_dark_pixels) * inv_total;
    const float blown_ratio = static_cast<float>(total_blown_pixels) * inv_total;

    // Detect backlight or localized glare hotspot conflict (RGB > blown_thresh)
    const bool is_hotspot_conflict = (blown_ratio > blown_ratio_thresh);
    result.has_hotspot_conflict = is_hotspot_conflict ? 1 : 0;

    // Dark condition check: mean luminance below threshold OR underexposed ratio exceeds limit
    const bool raw_is_dark = (result.mean_luminance < static_cast<float>(dark_thresh) || 
                              result.underexposed_ratio > dark_ratio_thresh);

    // If a significant portion of pixels are blown out, suppress the blind 2.5x gain multiplier
    // and prevent misclassifying the scene as low light due to local glare/backlight.
    if (raw_is_dark && !is_hotspot_conflict) {
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
 * @param blown_thresh Intensity threshold above which all RGB channels are considered blown-out.
 * @param blown_ratio_thresh Blown-out pixel ratio threshold above which backlight conflict suppresses 2.5x gain.
 * @return DarkDetectionResult Darkness metrics and recommended boost gain.
 */
DarkDetectionResult DarkDetector::detectDarkFrameAVX2(const cv::Mat& bgr_frame, 
                                                     uint8_t dark_thresh, 
                                                     float dark_ratio_thresh,
                                                     uint8_t blown_thresh,
                                                     float blown_ratio_thresh) const {
    return bawal::vision::detectDarkFrameAVX2(bgr_frame, dark_thresh, dark_ratio_thresh, blown_thresh, blown_ratio_thresh);
}

/**
 * @brief Analyzes frame illumination and detects dark conditions using SIMD/Highway acceleration.
 *
 * @param bgr_frame Input BGR image matrix.
 * @param dark_thresh Luminance threshold below which pixels are considered dark.
 * @param dark_ratio_thresh Underexposed pixel ratio threshold to trigger dark status.
 * @param blown_thresh Intensity threshold above which all RGB channels are considered blown-out.
 * @param blown_ratio_thresh Blown-out pixel ratio threshold above which backlight conflict suppresses 2.5x gain.
 * @return DarkDetectionResult Calculated illumination metrics and gain recommendation.
 */
DarkDetectionResult DarkDetector::detect(const cv::Mat& bgr_frame, 
                                         uint8_t dark_thresh, 
                                         float dark_ratio_thresh,
                                         uint8_t blown_thresh,
                                         float blown_ratio_thresh) const {
#if defined(__AVX2__)
    return detectDarkFrameAVX2(bgr_frame, dark_thresh, dark_ratio_thresh, blown_thresh, blown_ratio_thresh);
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
    uint32_t total_blown_pixels = 0;

    for (size_t y = 0; y < height; ++y) {
        const uint8_t* row_ptr = bgr_frame.ptr<uint8_t>(static_cast<int>(y));
        detail::DispatchScanDarkRow(row_ptr, width, dark_thresh, blown_thresh, total_luminance_sum, total_dark_pixels, total_blown_pixels);
    }

    const float inv_total = 1.0f / static_cast<float>(total_pixels);
    result.mean_luminance = static_cast<float>(total_luminance_sum) * inv_total;
    result.underexposed_ratio = static_cast<float>(total_dark_pixels) * inv_total;
    const float blown_ratio = static_cast<float>(total_blown_pixels) * inv_total;

    const bool is_hotspot_conflict = (blown_ratio > blown_ratio_thresh);
    result.has_hotspot_conflict = is_hotspot_conflict ? 1 : 0;

    const bool raw_is_dark = (result.mean_luminance < static_cast<float>(dark_thresh) || 
                              result.underexposed_ratio > dark_ratio_thresh);

    if (raw_is_dark && !is_hotspot_conflict) {
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
=======

#include <algorithm>

namespace bawal::vision {

DarkDetectionResult DarkDetector::detect(const cv::Mat& bgr_frame,
                                         uint8_t dark_thresh,
                                         float dark_ratio_thresh,
                                         uint8_t blown_thresh,
                                         float blown_ratio_thresh) const {
    (void)dark_ratio_thresh;
    (void)blown_thresh;
    (void)blown_ratio_thresh;

    DarkDetectionResult result{};
    if (bgr_frame.empty()) {
        return result;
    }

    const cv::Scalar mean = cv::mean(bgr_frame);
    const float luminance = 0.114f * static_cast<float>(mean[0]) +
                            0.587f * static_cast<float>(mean[1]) +
                            0.299f * static_cast<float>(mean[2]);

    result.mean_luminance = luminance;
    result.is_dark = (luminance < static_cast<float>(dark_thresh)) ? 1 : 0;
    result.suggested_boost_gain = result.is_dark
        ? std::clamp(128.0f / std::max(15.0f, luminance), 1.1f, 2.5f)
        : 1.0f;
    return result;
}

}  // namespace bawal::vision
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
