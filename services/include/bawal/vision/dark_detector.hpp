#pragma once
#include <cstdint>
#include <opencv2/core.hpp>

namespace bawal::vision {

/**
 * @brief Evaluation result for low-light and underexposure scene analysis.
 */
struct alignas(4) DarkDetectionResult {
    float mean_luminance{0.0f};
    float underexposed_ratio{0.0f};
    float suggested_boost_gain{1.0f};
    uint8_t is_dark{0};
    uint8_t has_hotspot_conflict{0};
    uint8_t reserved[2]{0, 0};
};
static_assert(sizeof(DarkDetectionResult) == 16, "DarkDetectionResult must be exactly 16 bytes");
static_assert(alignof(DarkDetectionResult) == 4, "DarkDetectionResult must be 4-byte aligned");

/**
 * @brief OpenCV-based low-light evaluator.
 */
class DarkDetector {
public:
    DarkDetector() = default;
    ~DarkDetector() = default;

    [[nodiscard]] DarkDetectionResult detect(const cv::Mat& bgr_frame,
                                             uint8_t dark_thresh = 40,
                                             float dark_ratio_thresh = 0.35f,
                                             uint8_t blown_thresh = 245,
                                             float blown_ratio_thresh = 0.02f) const;
};

}  // namespace bawal::vision
