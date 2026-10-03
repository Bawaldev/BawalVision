#include "bawal/vision/simd_food_evaluator.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <vector>

namespace bawal::vision {

SpoilageMetric evaluateSpoilageAVX2(const cv::Mat& food_roi, std::string_view food_class) {
    SpoilageMetric metric;
    if (food_roi.empty() || food_roi.cols < 8 || food_roi.rows < 8) {
        return metric;
    }

    cv::Mat roi_continuous = food_roi.isContinuous() ? food_roi : food_roi.clone();

    cv::Mat lab, hsv;
    cv::cvtColor(roi_continuous, lab, cv::COLOR_BGR2Lab);
    cv::cvtColor(roi_continuous, hsv, cv::COLOR_BGR2HSV);

    std::vector<cv::Mat> lab_planes, hsv_planes;
    cv::split(lab, lab_planes);
    cv::split(hsv, hsv_planes);

    const cv::Mat& l_channel = lab_planes[0]; // Luminance (0 - 255)
    const cv::Mat& v_channel = hsv_planes[2]; // Value (0 - 255)

    const int total_pixels = food_roi.rows * food_roi.cols;
    int bad_pixels = 0;
    uint64_t total_l_sum = 0;

    // Thresholds calibrated for 9 food categories:
    // - rice: L < 115 (slimy / yellowing / grey)
    // - spinach / water_spinach: V < 45 (dark decaying mush)
    // - banana: V < 50 (overripe blackened peel)
    // - tofu / tempeh: L < 70 or V < 40 (black mold / degradation)
    // - chicken / egg: L < 60 (dark oxidation / burning)
    uint8_t lum_thresh_val = 50;
    bool use_v_channel = false;

    if (food_class == "rice") {
        lum_thresh_val = 115;
    } else if (food_class == "spinach" || food_class == "water_spinach") {
        lum_thresh_val = 45;
        use_v_channel = true;
    } else if (food_class == "banana" || food_class == "orange") {
        lum_thresh_val = 40;
        use_v_channel = true;
    } else if (food_class == "tofu" || food_class == "tempeh") {
        lum_thresh_val = 65;
    } else if (food_class == "chicken" || food_class == "egg") {
        lum_thresh_val = 55;
    }

#if defined(__AVX2__)
    const __m256i thresh_vec = _mm256_set1_epi8(static_cast<char>(lum_thresh_val));
    const __m256i zero_vec   = _mm256_setzero_si256();

    for (int y = 0; y < food_roi.rows; ++y) {
        const uint8_t* l_ptr = use_v_channel ? v_channel.ptr<uint8_t>(y) : l_channel.ptr<uint8_t>(y);
        int x = 0;

        for (; x <= food_roi.cols - 32; x += 32) {
            __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(l_ptr + x));

            __m256i l_low  = _mm256_unpacklo_epi8(pixels, zero_vec);
            __m256i l_high = _mm256_unpackhi_epi8(pixels, zero_vec);
            
            __m256i sum32_a = _mm256_add_epi32(_mm256_unpacklo_epi16(l_low, zero_vec), _mm256_unpackhi_epi16(l_low, zero_vec));
            __m256i sum32_b = _mm256_add_epi32(_mm256_unpacklo_epi16(l_high, zero_vec), _mm256_unpackhi_epi16(l_high, zero_vec));
            __m256i sum_all = _mm256_add_epi32(sum32_a, sum32_b);

            alignas(32) int32_t temp_sums[8];
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(temp_sums), sum_all);
            for (int i = 0; i < 8; ++i) total_l_sum += temp_sums[i];

            __m256i diff = _mm256_subs_epu8(thresh_vec, pixels);
            __m256i mask_bad = _mm256_cmpgt_epi8(diff, zero_vec);

            uint32_t bitmask = static_cast<uint32_t>(_mm256_movemask_epi8(mask_bad));
            bad_pixels += _mm_popcnt_u32(bitmask);
        }

        for (; x < food_roi.cols; ++x) {
            uint8_t val = l_ptr[x];
            total_l_sum += val;
            if (val < lum_thresh_val) {
                bad_pixels++;
            }
        }
    }
#else
    // OpenMP SIMD vector loop (Cross-platform x86 / ARM / Graviton fallback)
    for (int y = 0; y < food_roi.rows; ++y) {
        const uint8_t* l_ptr = use_v_channel ? v_channel.ptr<uint8_t>(y) : l_channel.ptr<uint8_t>(y);
        int local_bad = 0;
        uint64_t local_sum = 0;

        #pragma omp simd reduction(+:local_bad, local_sum)
        for (int x = 0; x < food_roi.cols; ++x) {
            uint8_t val = l_ptr[x];
            local_sum += val;
            if (val < lum_thresh_val) {
                local_bad++;
            }
        }
        bad_pixels += local_bad;
        total_l_sum += local_sum;
    }
#endif

    if (total_pixels > 0) {
        const float inv_total = 1.0f / static_cast<float>(total_pixels);
        metric.avg_luminance = static_cast<float>(total_l_sum) * inv_total;
        metric.discoloration_ratio = static_cast<float>(bad_pixels) * inv_total;
    }

    // Food is considered visually spoiled if > 18% pixels are degraded
    metric.is_visually_spoiled = (metric.discoloration_ratio > 0.18f);

    return metric;
}

} // namespace bawal::vision