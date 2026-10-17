#include "bawal/vision/dewarp.hpp"
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>
namespace bawal::vision {

bool DewarpEngine::initCameraCalibration(const cv::Mat& camera_matrix, 
                                         const cv::Mat& dist_coeffs, 
                                         cv::Size image_size) {
    if (camera_matrix.empty() || dist_coeffs.empty() || image_size.empty()) {
        return false;
    }

    cv::Mat new_camera_matrix = cv::getOptimalNewCameraMatrix(
        camera_matrix, dist_coeffs, image_size, 0.0, image_size, nullptr
    );

    cv::initUndistortRectifyMap(
        camera_matrix, dist_coeffs, cv::Mat(),
        new_camera_matrix, image_size, CV_32FC1, m_map_x, m_map_y
    );
    cv::convertMaps(m_map_x, m_map_y, m_map1, m_map2, CV_16SC2);

    m_has_undistort_map = true;
    m_config.enable_undistort = true;
    updateFusedMaps();
    return true;
}

bool DewarpEngine::setTrayPerspective(std::span<const cv::Point2f, 4> src_quad, 
                                      cv::Size target_size) {
    if (target_size.empty()) {
        return false;
    }

    std::vector<cv::Point2f> src_pts(src_quad.begin(), src_quad.end());
    std::vector<cv::Point2f> dst_pts = {
        cv::Point2f(0.0f, 0.0f),
        cv::Point2f(static_cast<float>(target_size.width - 1), 0.0f),
        cv::Point2f(static_cast<float>(target_size.width - 1), static_cast<float>(target_size.height - 1)),
        cv::Point2f(0.0f, static_cast<float>(target_size.height - 1))
    };

    m_perspective_matrix = cv::getPerspectiveTransform(src_pts, dst_pts);
    if (m_perspective_matrix.empty()) {
        return false;
    }

    m_config.target_size = target_size;
    m_has_perspective = true;
    m_config.enable_perspective = true;
    updateFusedMaps();
    return true;
}

void DewarpEngine::updateFusedMaps() {
    if (m_has_undistort_map && m_has_perspective && !m_perspective_matrix.empty() && 
        m_config.target_size.width > 0 && m_config.target_size.height > 0) {
        cv::Mat fused_x, fused_y;
        cv::warpPerspective(m_map_x, fused_x, m_perspective_matrix, m_config.target_size,
                            cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(-1000.0));
        cv::warpPerspective(m_map_y, fused_y, m_perspective_matrix, m_config.target_size,
                            cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(-1000.0));
        cv::convertMaps(fused_x, fused_y, m_fused_map1, m_fused_map2, CV_16SC2);
        m_has_fused_map = true;
    } else {
        m_has_fused_map = false;
    }
}

bool DewarpEngine::process(const cv::Mat& in_frame, cv::Mat& out_frame) {
    if (in_frame.empty()) {
        return false;
    }

    if (m_has_fused_map) {
        cv::remap(in_frame, out_frame, m_fused_map1, m_fused_map2, cv::INTER_LINEAR);
    } else if (m_has_undistort_map && m_has_perspective) {
        cv::Mat intermediate;
        cv::remap(in_frame, intermediate, m_map1, m_map2, cv::INTER_LINEAR);
        cv::warpPerspective(intermediate, out_frame, m_perspective_matrix, 
                             m_config.target_size, cv::INTER_LINEAR);
    } else if (m_has_undistort_map) {
        cv::remap(in_frame, out_frame, m_map1, m_map2, cv::INTER_LINEAR);
    } else if (m_has_perspective) {
        cv::warpPerspective(in_frame, out_frame, m_perspective_matrix, 
                             m_config.target_size, cv::INTER_LINEAR);
    } else {
        out_frame = in_frame;
    }

    return !out_frame.empty();
}

} // namespace bawal::vision