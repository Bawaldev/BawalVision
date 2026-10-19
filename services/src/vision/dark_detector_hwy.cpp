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
 * 
 * @param row_ptr Pointer to the beginning of the BGR row data (interleaved CV_8UC3).
 * @param width Number of pixels in the row.
 * @param dark_thresh Luminance intensity threshold below which a pixel is considered dark.
 * @param out_sum Reference to the accumulated green-channel luminance sum.
 * @param out_dark_count Reference to the accumulated count of underexposed pixels.
 * @return void (Accumulates results directly into out_sum and out_dark_count).
 */
void ScanDarkRow(const uint8_t* HWY_RESTRICT row_ptr, 
                 size_t width, 
                 uint8_t dark_thresh, 
                 uint64_t& out_sum, 
                 uint32_t& out_dark_count) {
    const hn::ScalableTag<uint8_t> d8;
    const hn::ScalableTag<uint64_t> d64;
    const size_t N = hn::Lanes(d8);

    size_t x = 0;
    uint64_t local_sum = 0;
    uint32_t local_dark = 0;

    const auto thresh_vec = hn::Set(d8, dark_thresh);

    // Vector SIMD loop: process N pixels per iteration via LoadInterleaved3
    for (; x + N <= width; x += N) {
        auto b = hn::Zero(d8);
        auto g = hn::Zero(d8);
        auto r = hn::Zero(d8);
        hn::LoadInterleaved3(d8, row_ptr + x * 3, b, g, r);

        // Dark pixel evaluation on green channel (dominant luminance component)
        const auto mask = hn::Lt(g, thresh_vec);
        local_dark += static_cast<uint32_t>(hn::CountTrue(d8, mask));

        // Horizontal lane-wide summation via SumsOf8 and ReduceSum
        const auto sums = hn::SumsOf8(g);
        local_sum += hn::ReduceSum(d64, sums);
    }

    // Scalar fallback loop for remaining tail pixels
    for (; x < width; ++x) {
        const uint8_t g = row_ptr[x * 3 + 1];
        local_sum += g;
        if (g < dark_thresh) {
            ++local_dark;
        }
    }

    out_sum += local_sum;
    out_dark_count += local_dark;
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
 * @param out_sum Reference to the accumulated green-channel luminance sum.
 * @param out_dark_count Reference to the accumulated count of underexposed pixels.
 * @return void (Accumulates results directly into out_sum and out_dark_count).
 */
void DispatchScanDarkRow(const uint8_t* row_ptr, 
                         size_t width, 
                         uint8_t dark_thresh, 
                         uint64_t& out_sum, 
                         uint32_t& out_dark_count) {
    HWY_DYNAMIC_DISPATCH(ScanDarkRow)(row_ptr, width, dark_thresh, out_sum, out_dark_count);
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
 * @param out_sum Reference to the accumulated green-channel luminance sum.
 * @param out_dark_count Reference to the accumulated count of underexposed pixels.
 * @return void (Accumulates results directly into out_sum and out_dark_count).
 */
void DispatchScanDarkRow(const uint8_t* row_ptr, 
                         size_t width, 
                         uint8_t dark_thresh, 
                         uint64_t& out_sum, 
                         uint32_t& out_dark_count) {
    uint64_t local_sum = 0;
    uint32_t local_dark = 0;
    for (size_t x = 0; x < width; ++x) {
        const uint8_t g = row_ptr[x * 3 + 1];
        local_sum += g;
        if (g < dark_thresh) {
            ++local_dark;
        }
    }
    out_sum += local_sum;
    out_dark_count += local_dark;
}

} // namespace bawal::vision::detail

#endif