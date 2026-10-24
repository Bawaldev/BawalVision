#pragma once
#include <cstdint>
#include <opencv2/core.hpp>

namespace bawal::vision {

/**
 * @brief Evaluation result for low-light and underexposure scene analysis.
<<<<<<< HEAD
 * 
 * Aligned to 4-byte boundaries with explicit padding to guarantee exact 16-byte struct layout.
 */
struct alignas(4) DarkDetectionResult {
    float mean_luminance{0.0f};       ///< Mean scene luminance in range [0.0..255.0] (offset 0..3)
    float underexposed_ratio{0.0f};   ///< Ratio of pixels below dark threshold in range [0.0..1.0] (offset 4..7)
    float suggested_boost_gain{1.0f}; ///< Recommended exposure boost multiplier in range [1.0..2.5] (offset 8..11)
    uint8_t is_dark{0};               ///< Boolean flag indicating low-light underexposed condition (offset 12)
    uint8_t has_hotspot_conflict{0};  ///< Boolean flag indicating local glare / backlight hotspot conflict (offset 13)
    uint8_t reserved[2]{0, 0};        ///< Explicit padding bytes to preserve 16-byte alignment (offset 14..15)
=======
 */
struct alignas(4) DarkDetectionResult {
    float mean_luminance{0.0f};
    float underexposed_ratio{0.0f};
    float suggested_boost_gain{1.0f};
    uint8_t is_dark{0};
    uint8_t has_hotspot_conflict{0};
    uint8_t reserved[2]{0, 0};
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
};
static_assert(sizeof(DarkDetectionResult) == 16, "DarkDetectionResult must be exactly 16 bytes");
static_assert(alignof(DarkDetectionResult) == 4, "DarkDetectionResult must be 4-byte aligned");

/**
<<<<<<< HEAD
 * @brief Evaluates camera frame underexposure & low-light conditions using hardware SIMD (AVX2 / Google Highway).
=======
 * @brief OpenCV-based low-light evaluator.
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
 */
class DarkDetector {
public:
    DarkDetector() = default;
    ~DarkDetector() = default;

<<<<<<< HEAD
    /**
     * @brief Evaluates low-light condition on a BGR frame (dispatches to AVX2 when available or Highway SIMD).
     * 
     * @param bgr_frame Input BGR image matrix (CV_8UC3).
     * @param dark_thresh Luminance intensity threshold below which a pixel is considered dark (default: 40).
     * @param dark_ratio_thresh Ratio threshold [0.0..1.0] above which the frame is marked as dark (default: 0.35f / 35%).
     * @param blown_thresh Intensity threshold above which all RGB channels are considered blown-out (default: 245).
     * @param blown_ratio_thresh Blown-out pixel ratio threshold above which backlight/hotspot conflict suppresses 2.5x gain (default: 0.02f / 2%).
     * @return DarkDetectionResult containing mean luminance, underexposed ratio, hotspot flag, and suggested boost gain.
     */
    [[nodiscard]] DarkDetectionResult detect(const cv::Mat& bgr_frame, 
                                             uint8_t dark_thresh = 40, 
                                             float dark_ratio_thresh = 0.35f,
                                             uint8_t blown_thresh = 245,
                                             float blown_ratio_thresh = 0.02f) const;

    /**
     * @brief Direct Intel AVX2 SIMD low-light evaluation with backlight hotspot conflict detection.
     * 
     * @param bgr_frame Input BGR image matrix (CV_8UC3).
     * @param dark_thresh Luminance intensity threshold below which a pixel is considered dark (default: 40).
     * @param dark_ratio_thresh Ratio threshold [0.0..1.0] above which the frame is marked as dark (default: 0.35f / 35%).
     * @param blown_thresh Intensity threshold above which all RGB channels are considered blown-out (default: 245).
     * @param blown_ratio_thresh Blown-out pixel ratio threshold above which backlight/hotspot conflict suppresses 2.5x gain (default: 0.02f / 2%).
     * @return DarkDetectionResult containing mean luminance, underexposed ratio, hotspot flag, and suggested boost gain.
     */
    [[nodiscard]] DarkDetectionResult detectDarkFrameAVX2(const cv::Mat& bgr_frame, 
                                                          uint8_t dark_thresh = 40, 
                                                          float dark_ratio_thresh = 0.35f,
                                                          uint8_t blown_thresh = 245,
                                                          float blown_ratio_thresh = 0.02f) const;
};

/**
 * @brief Standalone function for AVX2 low-light frame evaluation with backlight hotspot suppression.
 * 
 * @param bgr_frame Input BGR image matrix (CV_8UC3).
 * @param dark_thresh Luminance intensity threshold below which a pixel is considered dark (default: 40).
 * @param dark_ratio_thresh Ratio threshold [0.0..1.0] above which the frame is marked as dark (default: 0.35f / 35%).
 * @param blown_thresh Intensity threshold above which all RGB channels are considered blown-out (default: 245).
 * @param blown_ratio_thresh Blown-out pixel ratio threshold above which backlight/hotspot conflict suppresses 2.5x gain (default: 0.02f / 2%).
 * @return DarkDetectionResult containing mean luminance, underexposed ratio, hotspot flag, and suggested boost gain.
 */
[[nodiscard]] DarkDetectionResult detectDarkFrameAVX2(const cv::Mat& bgr_frame, 
                                                      uint8_t dark_thresh = 40, 
                                                      float dark_ratio_thresh = 0.35f,
                                                      uint8_t blown_thresh = 245,
                                                      float blown_ratio_thresh = 0.02f);

} // namespace bawal::vision
=======
    [[nodiscard]] DarkDetectionResult detect(const cv::Mat& bgr_frame,
                                             uint8_t dark_thresh = 40,
                                             float dark_ratio_thresh = 0.35f,
                                             uint8_t blown_thresh = 245,
                                             float blown_ratio_thresh = 0.02f) const;
};

}  // namespace bawal::vision
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
