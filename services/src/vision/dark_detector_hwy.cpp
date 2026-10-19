#include "bawal/vision/dark_detector_hwy.hpp"

#if defined(BAWAL_HAVE_HWY) || __has_include(<hwy/highway.h>)

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "vision/dark_detector_hwy.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace bawal::vision::detail::HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

/**
 * @brief Dynamic vector Highway SIMD kernel scanning a single interleaved BGR row.
 * 
 * Extracts green channel luminance intensities and compares them against dark_thresh
 * using hardware vector registers dynamically dispatched per CPU target.
 * Simultaneously identifies blown-out pixels (RGB > blown_thresh).
 * 
 * @param row_ptr Pointer to the beginning of the BGR row data (interleaved CV_8UC3).
 * @param width Number of pixels in the row.
 * @param dark_thresh Luminance intensity threshold below which a pixel is considered dark.
 * @param blown_thresh Intensity threshold above which all RGB channels are considered blown-out.
 * @param out_sum Reference to the accumulated green-channel luminance sum.
 * @param out_dark_count Reference to the accumulated count of underexposed pixels.
 * @param out_blown_count Reference to the accumulated count of blown-out pixels.
 * @return void (Accumulates results directly into out_sum, out_dark_count, and out_blown_count).
 */
void ScanDarkRow(const uint8_t* HWY_RESTRICT row_ptr, 
                 size_t width, 
                 uint8_t dark_thresh, 
                 uint8_t blown_thresh,
                 uint64_t& out_sum, 
                 uint32_t& out_dark_count,
                 uint32_t& out_blown_count) {
    const hn::ScalableTag<uint8_t> d8;
    const hn::ScalableTag<uint64_t> d64;
    const size_t N = hn::Lanes(d8);

    size_t x = 0;
    uint64_t local_sum = 0;
    uint32_t local_dark = 0;
    uint32_t local_blown = 0;

    const auto thresh_vec = hn::Set(d8, dark_thresh);
    const auto blown_vec = hn::Set(d8, blown_thresh);

    // Vector SIMD loop: process N pixels per iteration via LoadInterleaved3
    for (; x + N <= width; x += N) {
        auto b = hn::Zero(d8);
        auto g = hn::Zero(d8);
        auto r = hn::Zero(d8);
        hn::LoadInterleaved3(d8, row_ptr + x * 3, b, g, r);

        // Dark pixel evaluation on green channel (dominant luminance component)
        const auto dark_mask = hn::Lt(g, thresh_vec);
        local_dark += static_cast<uint32_t>(hn::CountTrue(d8, dark_mask));

        // Blown-out evaluation: min(r, min(g, b)) > blown_thresh (all 3 RGB channels > threshold)
        const auto min_rgb = hn::Min(hn::Min(r, g), b);
        const auto blown_mask = hn::Gt(min_rgb, blown_vec);
        local_blown += static_cast<uint32_t>(hn::CountTrue(d8, blown_mask));

        // Horizontal lane-wide summation via SumsOf8 and ReduceSum
        const auto sums = hn::SumsOf8(g);
        local_sum += hn::ReduceSum(d64, sums);
    }

    // Scalar fallback loop for remaining tail pixels
    for (; x < width; ++x) {
        const uint8_t b = row_ptr[x * 3 + 0];
        const uint8_t g = row_ptr[x * 3 + 1];
        const uint8_t r = row_ptr[x * 3 + 2];
        local_sum += g;
        if (g < dark_thresh) {
            ++local_dark;
        }
        if (b > blown_thresh && g > blown_thresh && r > blown_thresh) {
            ++local_blown;
        }
    }

    out_sum += local_sum;
    out_dark_count += local_dark;
    out_blown_count += local_blown;
}

} // namespace bawal::vision::detail::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace bawal::vision::detail {

HWY_EXPORT(ScanDarkRow);

/**
 * @brief Dynamic runtime dispatcher for ScanDarkRow targeting active CPU architecture.
 * 
 * @param row_ptr Pointer to the beginning of the BGR row data (interleaved CV_8UC3).
 * @param width Number of pixels in the row.
 * @param dark_thresh Luminance intensity threshold below which a pixel is considered dark.
 * @param blown_thresh Intensity threshold above which all RGB channels are considered blown-out.
 * @param out_sum Reference to the accumulated green-channel luminance sum.
 * @param out_dark_count Reference to the accumulated count of underexposed pixels.
 * @param out_blown_count Reference to the accumulated count of blown-out pixels.
 * @return void (Accumulates results directly into out_sum, out_dark_count, and out_blown_count).
 */
void DispatchScanDarkRow(const uint8_t* row_ptr, 
                         size_t width, 
                         uint8_t dark_thresh, 
                         uint8_t blown_thresh,
                         uint64_t& out_sum, 
                         uint32_t& out_dark_count,
                         uint32_t& out_blown_count) {
    HWY_DYNAMIC_DISPATCH(ScanDarkRow)(row_ptr, width, dark_thresh, blown_thresh, out_sum, out_dark_count, out_blown_count);
}

} // namespace bawal::vision::detail
#endif

#else // Fallback when Google Highway headers are unavailable

namespace bawal::vision::detail {

/**
 * @brief Scalar fallback dispatcher for ScanDarkRow when Google Highway is not installed.
 * 
 * @param row_ptr Pointer to the beginning of the BGR row data (interleaved CV_8UC3).
 * @param width Number of pixels in the row.
 * @param dark_thresh Luminance intensity threshold below which a pixel is considered dark.
 * @param blown_thresh Intensity threshold above which all RGB channels are considered blown-out.
 * @param out_sum Reference to the accumulated green-channel luminance sum.
 * @param out_dark_count Reference to the accumulated count of underexposed pixels.
 * @param out_blown_count Reference to the accumulated count of blown-out pixels.
 * @return void (Accumulates results directly into out_sum, out_dark_count, and out_blown_count).
 */
void DispatchScanDarkRow(const uint8_t* row_ptr, 
                         size_t width, 
                         uint8_t dark_thresh, 
                         uint8_t blown_thresh,
                         uint64_t& out_sum, 
                         uint32_t& out_dark_count,
                         uint32_t& out_blown_count) {
    uint64_t local_sum = 0;
    uint32_t local_dark = 0;
    uint32_t local_blown = 0;
    for (size_t x = 0; x < width; ++x) {
        const uint8_t b = row_ptr[x * 3 + 0];
        const uint8_t g = row_ptr[x * 3 + 1];
        const uint8_t r = row_ptr[x * 3 + 2];
        local_sum += g;
        if (g < dark_thresh) {
            ++local_dark;
        }
        if (b > blown_thresh && g > blown_thresh && r > blown_thresh) {
            ++local_blown;
        }
    }
    out_sum += local_sum;
    out_dark_count += local_dark;
    out_blown_count += local_blown;
}

} // namespace bawal::vision::detail

#endif