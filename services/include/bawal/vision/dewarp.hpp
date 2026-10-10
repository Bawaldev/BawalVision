#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <vector>
#include <span>

namespace bawal::vision {

struct alignas(4) DewarpConfig {
    cv::Size target_size{640, 480};  ///< 8 bytes (width, height)
    bool enable_undistort{false};    ///< 1 byte
    bool enable_perspective{false};  ///< 1 byte
    uint8_t reserved[2]{0, 0};       ///< 2 bytes explicit padding
};

static_assert(sizeof(DewarpConfig) == 12, "DewarpConfig must be exactly 12 bytes");
static_assert(alignof(DewarpConfig) == 4, "DewarpConfig must be 4-byte aligned");

class DewarpEngine {
public:
    DewarpEngine() = default;
    ~DewarpEngine() = default;

    /**
     * @brief Initialize camera lens calibration maps for optical undistortion.
     * 
     * @param camera_matrix Camera 3x3 intrinsic matrix K.
     * @param dist_coeffs Distortion coefficients D (1x4, 1x5, or 1x8).
     * @param image_size Original input frame resolution.
     * @return true if precomputed maps were successfully allocated, false otherwise.
     */
    [[nodiscard]] bool initCameraCalibration(const cv::Mat& camera_matrix, 
                                             const cv::Mat& dist_coeffs, 
                                             cv::Size image_size);

    /**
     * @brief Set 4-corner perspective transformation matrix for food tray.
     * 
     * @param src_quad 4 corners of the tray in input frame (TL, TR, BR, BL).
     * @param target_size Destination top-down perspective output size.
     * @return true if homography is valid and invertible, false if degenerate.
     */
    [[nodiscard]] bool setTrayPerspective(std::span<const cv::Point2f, 4> src_quad, 
                                          cv::Size target_size);

    /**
     * @brief Execute lens distortion correction and perspective orientation dewarping.
     * 
     * @param in_frame Raw input source frame.
     * @param out_frame Destination cv::Mat for dewarped output.
     * @return true if dewarping succeeded, false if input is empty.
     */
    [[nodiscard]] bool process(const cv::Mat& in_frame, cv::Mat& out_frame);

    /**
     * @brief Check availability of undistortion calibration map.
     * 
     * @return true if remapping maps are ready in memory.
     */
    [[nodiscard]] bool isCalibrated() const noexcept {
        return m_has_undistort_map;
    }

private:
    cv::Mat m_map1, m_map2;
    cv::Mat m_perspective_matrix;
    DewarpConfig m_config;
    bool m_has_undistort_map{false};
    bool m_has_perspective{false};
};

} // namespace bawal::vision