#pragma once
#include <cstddef>
#include <cstdint>

namespace bawal::vision::detail {

/**
 * @brief Dispatches the row scanning kernel to the optimal CPU vector target via Google Highway dynamic dispatch.
 * 
 * @param row_ptr Pointer to the beginning of the BGR row data (interleaved CV_8UC3 format).
 * @param width Number of pixels in the row.
 * @param dark_thresh Luminance intensity threshold below which a pixel is considered dark.
 * @param out_sum Reference to the accumulated green-channel luminance sum.
 * @param out_dark_count Reference to the accumulated count of underexposed pixels.
 * @return void (Results accumulated into out_sum and out_dark_count).
 */
void DispatchScanDarkRow(const uint8_t* row_ptr, 
                         size_t width, 
                         uint8_t dark_thresh, 
                         uint64_t& out_sum, 
                         uint32_t& out_dark_count);

} // namespace bawal::vision::detail
