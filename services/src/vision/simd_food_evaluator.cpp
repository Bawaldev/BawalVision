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
    uint8_t lum_thresh_val = 30;
    bool use_v_channel = false;

    if (food_class == "rice") {
        lum_thresh_val = 35;
    } else if (food_class == "spinach" || food_class == "water_spinach") {
        lum_thresh_val = 25;
        use_v_channel = true;
    } else if (food_class == "banana" || food_class == "orange") {
        lum_thresh_val = 25;
        use_v_channel = true;
    } else if (food_class == "tofu" || food_class == "tempeh") {
        lum_thresh_val = 35;
    } else if (food_class == "chicken" || food_class == "egg") {
        lum_thresh_val = 30;
    }

#if defined(__AVX2__)
    const __m256i thresh_vec = _mm256_set1_epi8(static_cast<char>(lum_thresh_val));
    const __m256i zero_vec   = _mm256_setzero_si256();

    for (int y = 0; y < food_roi.rows; ++y) {
        const uint8_t* l_ptr = use_v_channel ? v_channel.ptr<uint8_t>(y) : l_channel.ptr<uint8_t>(y);
        int x = 0;

        for (; x <= food_roi.cols - 32; x += 32) {
            __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(l_ptr + x));

            // Single hardware instruction PSADBW (_mm256_sad_epu8) computes 4x 64-bit horizontal sums in 1 cycle
            __m256i sad = _mm256_sad_epu8(pixels, zero_vec);
            const uint64_t sum_low = static_cast<uint64_t>(_mm256_extract_epi64(sad, 0)) + static_cast<uint64_t>(_mm256_extract_epi64(sad, 1));
            const uint64_t sum_high = static_cast<uint64_t>(_mm256_extract_epi64(sad, 2)) + static_cast<uint64_t>(_mm256_extract_epi64(sad, 3));
            total_l_sum += (sum_low + sum_high);

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

    // Food is considered visually spoiled if > 35% pixels are degraded
    metric.is_visually_spoiled = (metric.discoloration_ratio > 0.35f);

    return metric;
}

TextureMetric evaluateTextureAVX2(const cv::Mat& food_roi) {
    TextureMetric metric;
    if (food_roi.empty() || food_roi.cols < 16 || food_roi.rows < 16) {
        return metric;
    }

    cv::Mat gray;
    if (food_roi.channels() == 3) {
        cv::cvtColor(food_roi, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = food_roi;
    }

    const int rows = gray.rows;
    const int cols = gray.cols;
    uint64_t total_grad_sum = 0;
    uint64_t sum_val = 0;
    uint64_t sum_sq_val = 0;
    const int inner_pixels = (rows - 2) * (cols - 2);

    if (inner_pixels <= 0) {
        return metric;
    }

#if defined(__AVX2__)
    const __m256i zero_vec = _mm256_setzero_si256();

    for (int y = 1; y < rows - 1; ++y) {
        const uint8_t* row_prev = gray.ptr<uint8_t>(y - 1);
        const uint8_t* row_curr = gray.ptr<uint8_t>(y);
        const uint8_t* row_next = gray.ptr<uint8_t>(y + 1);

        int x = 1;
        for (; x <= cols - 33; x += 32) {
            __m256i c_pix = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row_curr + x));
            __m256i l_pix = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row_curr + x - 1));
            __m256i r_pix = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row_curr + x + 1));
            __m256i t_pix = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row_prev + x));
            __m256i b_pix = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row_next + x));

            __m256i h_diff1 = _mm256_subs_epu8(r_pix, l_pix);
            __m256i h_diff2 = _mm256_subs_epu8(l_pix, r_pix);
            __m256i h_grad = _mm256_or_si256(h_diff1, h_diff2);

            __m256i v_diff1 = _mm256_subs_epu8(b_pix, t_pix);
            __m256i v_diff2 = _mm256_subs_epu8(t_pix, b_pix);
            __m256i v_grad = _mm256_or_si256(v_diff1, v_diff2);

            __m256i grad = _mm256_adds_epu8(h_grad, v_grad);

            // Accumulate total gradients using PSADBW
            __m256i sad_grad = _mm256_sad_epu8(grad, zero_vec);
            total_grad_sum += static_cast<uint64_t>(_mm256_extract_epi64(sad_grad, 0)) +
                              static_cast<uint64_t>(_mm256_extract_epi64(sad_grad, 1)) +
                              static_cast<uint64_t>(_mm256_extract_epi64(sad_grad, 2)) +
                              static_cast<uint64_t>(_mm256_extract_epi64(sad_grad, 3));

            // Accumulate pixel sums using PSADBW
            __m256i sad_val = _mm256_sad_epu8(c_pix, zero_vec);
            sum_val += static_cast<uint64_t>(_mm256_extract_epi64(sad_val, 0)) +
                       static_cast<uint64_t>(_mm256_extract_epi64(sad_val, 1)) +
                       static_cast<uint64_t>(_mm256_extract_epi64(sad_val, 2)) +
                       static_cast<uint64_t>(_mm256_extract_epi64(sad_val, 3));

            // Accumulate sum of squares using maddubs
            __m256i c_low = _mm256_unpacklo_epi8(c_pix, zero_vec);
            __m256i c_high = _mm256_unpackhi_epi8(c_pix, zero_vec);
            __m256i c_sq_low = _mm256_madd_epi16(c_low, c_low);
            __m256i c_sq_high = _mm256_madd_epi16(c_high, c_high);
            __m256i c_sq_all = _mm256_add_epi32(c_sq_low, c_sq_high);

            alignas(32) int32_t sq_temp[8];
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(sq_temp), c_sq_all);
            for (int i = 0; i < 8; ++i) sum_sq_val += sq_temp[i];
        }

        for (; x < cols - 1; ++x) {
            int h_d = std::abs(static_cast<int>(row_curr[x + 1]) - static_cast<int>(row_curr[x - 1]));
            int v_d = std::abs(static_cast<int>(row_next[x]) - static_cast<int>(row_prev[x]));
            total_grad_sum += (h_d + v_d);
            int v = row_curr[x];
            sum_val += v;
            sum_sq_val += (v * v);
        }
    }
#else
    for (int y = 1; y < rows - 1; ++y) {
        const uint8_t* row_prev = gray.ptr<uint8_t>(y - 1);
        const uint8_t* row_curr = gray.ptr<uint8_t>(y);
        const uint8_t* row_next = gray.ptr<uint8_t>(y + 1);

        #pragma omp simd reduction(+:total_grad_sum, sum_val, sum_sq_val)
        for (int x = 1; x < cols - 1; ++x) {
            int h_d = std::abs(static_cast<int>(row_curr[x + 1]) - static_cast<int>(row_curr[x - 1]));
            int v_d = std::abs(static_cast<int>(row_next[x]) - static_cast<int>(row_prev[x]));
            total_grad_sum += (h_d + v_d);
            int v = row_curr[x];
            sum_val += v;
            sum_sq_val += (v * v);
        }
    }
#endif

    const float inv_n = 1.0f / static_cast<float>(inner_pixels);
    metric.edge_energy = static_cast<float>(total_grad_sum) * inv_n;

    const float mean = static_cast<float>(sum_val) * inv_n;
    const float mean_sq = static_cast<float>(sum_sq_val) * inv_n;
    const float variance = std::max(0.0f, mean_sq - (mean * mean));
    metric.std_dev = std::sqrt(variance);

    // Wall & flat surface rejection:
    // Real food objects have grain, porous texture, contours, and color variance.
    // Flat walls and smooth uniform backgrounds are rejected.
    metric.is_valid_food_texture = (metric.edge_energy >= 4.5f || metric.std_dev >= 8.0f);

    return metric;
}

} // namespace bawal::vision