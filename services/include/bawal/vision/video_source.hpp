#pragma once

#include <opencv2/videoio.hpp>
#include <string>
#include <span>

namespace bawal::vision {

enum class SourceMode {
    ESP32_STREAM,  // Live MJPEG HTTP Ingestion
    LOCAL_MP4,     // Mock Video Testing (Offline loop)
    WEBCAM_USB     // Direct V4L2 / UVC Camera
};

class VideoSource {


    SourceMode m_mode{SourceMode::LOCAL_MP4};
    cv::VideoCapture m_cap;

    /**
     * @brief Initialize video source (local MP4 file, USB webcam, or ESP32 stream).
     * 
     * @param mode Camera/video ingestion mode.
     * @param uri Video file path, IP stream URI, or V4L2 device path.
     * @return true if hardware handle or stream opened successfully, false otherwise.
     */
    [[nodiscard]] bool open(SourceMode mode, const std::string& uri);

    /**
     * @brief Retrieve the next frame (automatically rewinds if LOCAL_MP4 reaches EOF).
     * 
     * @param out_frame Destination matrix for the retrieved frame image.
     * @return true if a valid frame was read, false if empty or on failure.
     */
    [[nodiscard]] bool grabNextFrame(cv::Mat& out_frame);
};

} // namespace bawal::vision