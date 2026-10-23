#include "bawal/vision/simd_food_evaluator.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace bawal::vision {

SpoilageMetric evaluateSpoilageAVX2(const cv::Mat& food_roi, std::string_view food_class) {
    SpoilageMetric metric;
    if (food_roi.empty() || food_roi.cols < 8 || food_roi.rows < 8) {
        return metric;
    }

    // Crop to inner 75% core of food ROI to prevent desk surface, plate rims, and external cables from corrupting spoilage metrics
    int margin_x = static_cast<int>(food_roi.cols * 0.125f);
    int margin_y = static_cast<int>(food_roi.rows * 0.125f);
    int core_w = std::max(4, food_roi.cols - 2 * margin_x);
    int core_h = std::max(4, food_roi.rows - 2 * margin_y);
    cv::Rect core_rect(margin_x, margin_y, core_w, core_h);
    cv::Mat core_roi = food_roi(core_rect);

    cv::Mat roi_continuous = core_roi.isContinuous() ? core_roi : core_roi.clone();

    const int total_pixels = core_h * core_w;
    if (total_pixels <= 0) {
        return metric;
    }

    // Select the required channel BEFORE running cv::cvtColor.
    uint8_t base_thresh = 30;
    bool use_v_channel = false;

    if (food_class == "rice" || food_class == "tofu" || food_class == "tempeh") {
        base_thresh = 35;
    } else if (food_class == "spinach" || food_class == "water_spinach" ||
               food_class == "banana"  || food_class == "orange") {
        base_thresh = 25;
        use_v_channel = true;
    } else if (food_class == "chicken" || food_class == "egg") {
        base_thresh = 30;
    }

    // Run exactly one colour conversion for the selected food class.
    thread_local cv::Mat tl_converted;
    thread_local std::vector<cv::Mat> tl_planes(3);

    if (use_v_channel) {
        cv::cvtColor(roi_continuous, tl_converted, cv::COLOR_BGR2HSV);
    } else {
        cv::cvtColor(roi_continuous, tl_converted, cv::COLOR_BGR2Lab);
    }
    cv::split(tl_converted, tl_planes);
    const cv::Mat& active_channel = use_v_channel ? tl_planes[2] : tl_planes[0];

    // Pass 1: Vectorized accumulation of total luminance to establish dynamic illumination baseline
    uint64_t total_l_sum = 0;
#if defined(__AVX2__)
    const __m256i zero_vec = _mm256_setzero_si256();

    for (int y = 0; y < core_h; ++y) {
        const uint8_t* l_ptr = active_channel.ptr<uint8_t>(y);
        int x = 0;

        for (; x <= core_w - 32; x += 32) {
            __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(l_ptr + x));
            __m256i sad = _mm256_sad_epu8(pixels, zero_vec);
            const uint64_t sum_low = static_cast<uint64_t>(_mm256_extract_epi64(sad, 0)) + static_cast<uint64_t>(_mm256_extract_epi64(sad, 1));
            const uint64_t sum_high = static_cast<uint64_t>(_mm256_extract_epi64(sad, 2)) + static_cast<uint64_t>(_mm256_extract_epi64(sad, 3));
            total_l_sum += (sum_low + sum_high);
        }

        for (; x < core_w; ++x) {
            total_l_sum += l_ptr[x];
        }
    }
#else
    for (int y = 0; y < core_h; ++y) {
        const uint8_t* l_ptr = active_channel.ptr<uint8_t>(y);
        uint64_t local_sum = 0;

        #pragma omp simd reduction(+:local_sum)
        for (int x = 0; x < core_w; ++x) {
            local_sum += l_ptr[x];
        }
        total_l_sum += local_sum;
    }
#endif

    const float mean_lum = static_cast<float>(total_l_sum) / static_cast<float>(total_pixels);
    metric.avg_luminance = mean_lum;

    // Check for extreme underexposure or severe lens occlusion (e.g., hand covering camera or dark cast shadow)
    if (mean_lum < 18.0f) {
        metric.discoloration_ratio = 0.0f;
        metric.is_visually_spoiled = false;
        return metric;
    }

    // Dynamic Luminance Baseline:
    // Scale bad-pixel threshold according to local ambient luminance so cast shadows
    // from hands/moving objects do not cause false REJECT 100%.
    constexpr float ref_lum = 140.0f;
    const float scale_factor = std::clamp(mean_lum / ref_lum, 0.40f, 1.0f);
    const uint8_t dynamic_thresh = static_cast<uint8_t>(std::clamp(base_thresh * scale_factor, 12.0f, static_cast<float>(base_thresh)));

    // Pass 2: Vectorized accumulation of anomalous dark discoloration pixels using dynamic threshold
    int bad_pixels = 0;
#if defined(__AVX2__)
    const __m256i thresh_vec = _mm256_set1_epi8(static_cast<char>(dynamic_thresh));

    for (int y = 0; y < core_h; ++y) {
        const uint8_t* l_ptr = active_channel.ptr<uint8_t>(y);
        int x = 0;

        for (; x <= core_w - 32; x += 32) {
            __m256i pixels = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(l_ptr + x));
            __m256i diff = _mm256_subs_epu8(thresh_vec, pixels);
            __m256i mask_bad = _mm256_cmpgt_epi8(diff, zero_vec);

            uint32_t bitmask = static_cast<uint32_t>(_mm256_movemask_epi8(mask_bad));
            bad_pixels += _mm_popcnt_u32(bitmask);
        }

        for (; x < core_w; ++x) {
            if (l_ptr[x] < dynamic_thresh) {
                bad_pixels++;
            }
        }
    }
#else
    for (int y = 0; y < core_h; ++y) {
        const uint8_t* l_ptr = active_channel.ptr<uint8_t>(y);
        int local_bad = 0;

        #pragma omp simd reduction(+:local_bad)
        for (int x = 0; x < core_w; ++x) {
            if (l_ptr[x] < dynamic_thresh) {
                local_bad++;
            }
        }
        bad_pixels += local_bad;
    }
#endif

    const float inv_total = 1.0f / static_cast<float>(total_pixels);
    metric.discoloration_ratio = static_cast<float>(bad_pixels) * inv_total;

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

QualityVectorStats computeQualityStatsSIMD(const float* similarity,
                                           const float* quality,
                                           size_t count) {
    QualityVectorStats stats;
    if (similarity == nullptr || quality == nullptr || count == 0) {
        return stats;
    }

    float sum_similarity = 0.0f;
    float sum_quality = 0.0f;
    float sum_sq_quality = 0.0f;
    float min_quality = std::numeric_limits<float>::max();
    float max_quality = std::numeric_limits<float>::lowest();

#if defined(__AVX2__)
    // 8 lanes per iteration: running sums, sum-of-squares and min/max.
    __m256 v_sum_sim = _mm256_setzero_ps();
    __m256 v_sum_qual = _mm256_setzero_ps();
    __m256 v_sum_sq = _mm256_setzero_ps();
    __m256 v_min = _mm256_set1_ps(std::numeric_limits<float>::max());
    __m256 v_max = _mm256_set1_ps(std::numeric_limits<float>::lowest());

    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 v_sim = _mm256_loadu_ps(similarity + i);
        const __m256 v_qual = _mm256_loadu_ps(quality + i);
        v_sum_sim = _mm256_add_ps(v_sum_sim, v_sim);
        v_sum_qual = _mm256_add_ps(v_sum_qual, v_qual);
        v_sum_sq = _mm256_add_ps(v_sum_sq, _mm256_mul_ps(v_qual, v_qual));
        v_min = _mm256_min_ps(v_min, v_qual);
        v_max = _mm256_max_ps(v_max, v_qual);
    }

    alignas(32) float lane_sum_sim[8];
    alignas(32) float lane_sum_qual[8];
    alignas(32) float lane_sum_sq[8];
    alignas(32) float lane_min[8];
    alignas(32) float lane_max[8];
    _mm256_store_ps(lane_sum_sim, v_sum_sim);
    _mm256_store_ps(lane_sum_qual, v_sum_qual);
    _mm256_store_ps(lane_sum_sq, v_sum_sq);
    _mm256_store_ps(lane_min, v_min);
    _mm256_store_ps(lane_max, v_max);

    #pragma omp simd reduction(+:sum_similarity, sum_quality, sum_sq_quality)
    for (int lane = 0; lane < 8; ++lane) {
        sum_similarity += lane_sum_sim[lane];
        sum_quality += lane_sum_qual[lane];
        sum_sq_quality += lane_sum_sq[lane];
        min_quality = std::min(min_quality, lane_min[lane]);
        max_quality = std::max(max_quality, lane_max[lane]);
    }

    // Scalar tail for the final < 8 detections.
    for (; i < count; ++i) {
        sum_similarity += similarity[i];
        sum_quality += quality[i];
        sum_sq_quality += quality[i] * quality[i];
        min_quality = std::min(min_quality, quality[i]);
        max_quality = std::max(max_quality, quality[i]);
    }
#else
    // OpenMP SIMD fallback (aarch64 NEON / portable builds).
    #pragma omp simd reduction(+:sum_similarity, sum_quality, sum_sq_quality) \
                         reduction(min:min_quality) reduction(max:max_quality)
    for (size_t i = 0; i < count; ++i) {
        sum_similarity += similarity[i];
        sum_quality += quality[i];
        sum_sq_quality += quality[i] * quality[i];
        min_quality = std::min(min_quality, quality[i]);
        max_quality = std::max(max_quality, quality[i]);
    }
#endif

    const float inv_count = 1.0f / static_cast<float>(count);
    stats.mean_similarity = sum_similarity * inv_count;
    stats.mean_quality = sum_quality * inv_count;
    const float variance =
        std::max(0.0f, sum_sq_quality * inv_count - stats.mean_quality * stats.mean_quality);
    stats.std_quality = std::sqrt(variance);
    stats.min_quality = min_quality;
    stats.max_quality = max_quality;
    return stats;
}

} // namespace bawal::vision
