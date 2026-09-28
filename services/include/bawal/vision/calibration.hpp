#pragma once

#include <opencv2/core.hpp>

namespace bawal::vision {

/**
 * @brief Camera Intrinsic Matrix K and Distortion Coefficients D for ESP32-S3 OV5640.
 */
struct CameraCalibrationParams {
    cv::Mat camera_matrix; ///< 3x3 intrinsic matrix
    cv::Mat dist_coeffs;   ///< 1x5 distortion coefficients
    cv::Size image_size{640, 480};

    static CameraCalibrationParams createDefaultOV5640(cv::Size size = cv::Size(640, 480)) {
        CameraCalibrationParams params;
        params.image_size = size;

        params.camera_matrix = (cv::Mat_<double>(3, 3) <<
            530.0,   0.0, size.width / 2.0,
              0.0, 530.0, size.height / 2.0,
              0.0,   0.0, 1.0);

        params.dist_coeffs = (cv::Mat_<double>(1, 5) << -0.25, 0.07, 0.0, 0.0, 0.0);

        return params;
    }
};

} // namespace bawal::vision
