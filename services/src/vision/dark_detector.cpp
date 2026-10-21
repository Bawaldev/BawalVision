#include "bawal/vision/dark_detector.hpp"

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>

namespace bawal::vision {

DarkDetectionResult DarkDetector::detect(const cv::Mat& bgr_frame,
                                         uint8_t dark_thresh,
                                         float dark_ratio_thresh,
                                         uint8_t blown_thresh,
                                         float blown_ratio_thresh) const {
    DarkDetectionResult result{};
    if (bgr_frame.empty() || bgr_frame.cols <= 0 || bgr_frame.rows <= 0) {
        return result;
    }

    const double total_pixels = static_cast<double>(bgr_frame.cols) * bgr_frame.rows;

    const cv::Scalar channel_means = cv::mean(bgr_frame);
    result.mean_luminance = static_cast<float>(channel_means[1]);

    cv::Mat dark_mask;
    cv::Mat blown_mask;
    const int dark_high = std::max(0, static_cast<int>(dark_thresh) - 1);
    const int blown_low = std::min(255, static_cast<int>(blown_thresh) + 1);

    cv::inRange(bgr_frame, cv::Scalar(0, 0, 0), cv::Scalar(255, dark_high, 255), dark_mask);
    cv::inRange(bgr_frame, cv::Scalar(blown_low, blown_low, blown_low), cv::Scalar(255, 255, 255),
                blown_mask);

    const double dark_pixels = cv::countNonZero(dark_mask);
    const double blown_pixels = cv::countNonZero(blown_mask);

    result.underexposed_ratio = static_cast<float>(dark_pixels / total_pixels);
    const float blown_ratio = static_cast<float>(blown_pixels / total_pixels);
    const bool hotspot_conflict = blown_ratio > blown_ratio_thresh;
    result.has_hotspot_conflict = hotspot_conflict ? 1 : 0;

    const bool raw_is_dark = result.mean_luminance < static_cast<float>(dark_thresh) ||
                             result.underexposed_ratio > dark_ratio_thresh;

    if (raw_is_dark && !hotspot_conflict) {
        result.is_dark = 1;
        result.suggested_boost_gain =
            std::clamp(128.0f / std::max(15.0f, result.mean_luminance), 1.1f, 2.5f);
    } else {
        result.is_dark = 0;
        result.suggested_boost_gain = 1.0f;
    }

    return result;
}

}  // namespace bawal::vision
