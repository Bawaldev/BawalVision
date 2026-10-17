#pragma once

#include "bawal/server/http_utils.hpp"
#include <opencv2/core/mat.hpp>
#include <opencv2/videoio.hpp>
#include <string>
#include <vector>

namespace bawal::vision {

/**
 * @brief Video frame ingestion mode.
 */
enum class SourceMode {
    ESP32_STREAM,  ///< Ingest single snapshot frames from Go Backend via HTTP polling (/camera/frame)
    LOCAL_MP4,     ///< Offline local MP4 video file playback with automatic loop
    WEBCAM_USB     ///< Direct local V4L2 / UVC USB camera device
};

/**
 * @brief Unified video source abstraction supporting local MP4 files, USB webcams, and HTTP pull streams.
 */
struct VideoSource {
    SourceMode mode{SourceMode::LOCAL_MP4};
    cv::VideoCapture cap;

    // HTTP pull ingestion resources
    CURL* curl_handle{nullptr};
    std::string stream_url;
    std::vector<uint8_t> frame_buffer; // Reusable buffer to prevent heap reallocation
    bool did_rewind{false};             ///< Flag indicating the video wrapped around to frame 0

    VideoSource();
    ~VideoSource();

    // Disable copy semantics to prevent double curl cleanup
    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;

    // Enable move semantics
    VideoSource(VideoSource&& other) noexcept;
    VideoSource& operator=(VideoSource&& other) noexcept;

    /**
     * @brief Initialize and open the video source.
     * @param new_mode Video ingestion mode (ESP32_STREAM, LOCAL_MP4, or WEBCAM_USB).
     * @param uri MP4 file path, USB device index/path (e.g. "/dev/video0" or "0"), or Backend endpoint URL.
     * @return true if initialized and ready, false on failure.
     */
    [[nodiscard]] bool open(SourceMode new_mode, const std::string& uri);

    /**
     * @brief Fetch the next frame transparently with zero latency buffer accumulation.
     * @param out_frame Destination cv::Mat for decoded BGR frame.
     * @return true if a valid frame was obtained, false otherwise.
     */
    [[nodiscard]] bool grabNextFrame(cv::Mat& out_frame);
    [[nodiscard]] bool checkAndResetRewind() noexcept {
        const bool ret = did_rewind;
        did_rewind = false;
        return ret;
    }
};

} // namespace bawal::vision