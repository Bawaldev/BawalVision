#include "bawal/vision/dark_detector.hpp"

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
