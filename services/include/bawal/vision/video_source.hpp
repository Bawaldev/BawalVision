#pragma once

#include "bawal/server/http_utils.hpp"
#include "bawal/vision/camera_receiver.hpp"
#include "bawal/vision/enchanter.hpp"
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <chrono>
#include <cstdint>
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
 * @brief Unified video source abstraction supporting local MP4 files, USB webcams, and HTTP pull streams
 * with integrated ISP IlluminationEnhancerAI and RealESRGANEngine support.
 */
struct VideoSource {
    SourceMode mode{SourceMode::LOCAL_MP4};
    cv::VideoCapture cap;

    // HTTP pull ingestion & MJPEG/binary frame decoding resources
    CURL* curl_handle{nullptr};
    std::string stream_url;
    std::string fallback_url;
    std::vector<uint8_t> frame_buffer; // Reusable buffer to prevent heap reallocation
    CameraReceiver receiver;           // Integrated binary header & MJPEG decoder
    cv::Mat last_decoded_frame;        // Cached decoded frame for duplicate fingerprint skipping
    uint64_t last_frame_hash{0};       // O(1) 24-byte frame fingerprint

    // Integrated Vision Enchanter (ISP / CLAHE) resources
    cv::Ptr<cv::CLAHE> clahe;
    int target_width{640};
    int target_height{480};
    bool enable_enchant{false};        ///< If true, grabNextFrame automatically applies IlluminationEnhancerAI
    bool did_rewind{false};            ///< Flag indicating the video wrapped around to frame 0
    bool is_duplicate_frame{false};    ///< True if the latest HTTP frame matched last_frame_hash

    // Per-instance MP4 pacing clock. A function-local static here would make all
    // VideoSource instances share (and race on) the same scheduler.
    std::chrono::steady_clock::time_point last_frame_time{};

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
     * @brief Update the stream ingestion URL and optional fallback URL dynamically without recreating CURL handle.
     */
    void setStreamUrl(const std::string& primary_url, const std::string& backup_url = "") noexcept {
        if (stream_url != primary_url) {
            stream_url = primary_url;
            last_frame_hash = 0;
        }
        fallback_url = backup_url;
    }

    /**
     * @brief Fetch the next frame transparently with zero latency buffer accumulation.
     * @param out_frame Destination cv::Mat for decoded (and optionally enchanted) BGR frame.
     * @return true if a valid frame was obtained, false otherwise.
     */
    [[nodiscard]] bool grabNextFrame(cv::Mat& out_frame);

    /**
     * @brief Fetch the next frame and directly run IlluminationEnhancerAI + optional RealESRGAN super-resolution.
     * @param out_frame Destination cv::Mat for enchanted BGR frame.
     * @param sr_engine Optional RealESRGANEngine pointer for neural/SIMD super-resolution.
     * @return true if a valid frame was grabbed and enchanted, false otherwise.
     */
    [[nodiscard]] bool grabAndEnchantFrame(cv::Mat& out_frame, RealESRGANEngine* sr_engine = nullptr);

    [[nodiscard]] bool didRewind() noexcept {
        const bool ret = did_rewind;
        did_rewind = false;
        return ret;
    }
    [[nodiscard]] bool checkAndResetRewind() noexcept {
        const bool ret = did_rewind;
        did_rewind = false;
        return ret;
    }
};

} // namespace bawal::vision