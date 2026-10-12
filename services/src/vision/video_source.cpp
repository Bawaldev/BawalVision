#include "bawal/vision/video_source.hpp"
#include "bawal/server/http_utils.hpp"
#include <opencv2/imgcodecs.hpp>
#include <curl/curl.h>
#include <cctype>

namespace bawal::vision {

VideoSource::VideoSource() = default;

VideoSource::~VideoSource() {
    if (curl_handle) {
        curl_easy_cleanup(curl_handle);
        curl_handle = nullptr;
    }
}

VideoSource::VideoSource(VideoSource&& other) noexcept
    : mode(other.mode),
      cap(std::move(other.cap)),
      curl_handle(other.curl_handle),
      stream_url(std::move(other.stream_url)),
      frame_buffer(std::move(other.frame_buffer)) {
    other.curl_handle = nullptr;
}

VideoSource& VideoSource::operator=(VideoSource&& other) noexcept {
    if (this != &other) {
        if (curl_handle) {
            curl_easy_cleanup(curl_handle);
        }
        mode = other.mode;
        cap = std::move(other.cap);
        curl_handle = other.curl_handle;
        stream_url = std::move(other.stream_url);
        frame_buffer = std::move(other.frame_buffer);
        other.curl_handle = nullptr;
    }
    return *this;
}

bool VideoSource::open(SourceMode new_mode, const std::string& uri) {
    mode = new_mode;
    stream_url = uri;

    if (mode == SourceMode::LOCAL_MP4 || mode == SourceMode::WEBCAM_USB) {
        // If device path is an integer index (e.g. "0") in WEBCAM_USB mode, open by index
        if (mode == SourceMode::WEBCAM_USB && !uri.empty() && std::isdigit(static_cast<unsigned char>(uri[0])) && uri.find('/') == std::string::npos) {
            return cap.open(std::stoi(uri));
        }
        return cap.open(uri);
    }

    if (mode == SourceMode::ESP32_STREAM) {
        if (!curl_handle) {
            curl_handle = curl_easy_init();
        }
        // Pre-allocate 128 KB buffer to prevent repeated heap allocations
        frame_buffer.reserve(128 * 1024);
        return (curl_handle != nullptr);
    }

    return false;
}

bool VideoSource::grabNextFrame(cv::Mat& out_frame) {
    // 1. Local MP4 mock video mode with automatic looping on EOF
    if (mode == SourceMode::LOCAL_MP4) {
        if (!cap.read(out_frame) || out_frame.empty()) {
            cap.set(cv::CAP_PROP_POS_FRAMES, 0); // Rewind video to start
            return cap.read(out_frame);
        }
        return true;
    }

    // 2. Physical USB camera mode
    if (mode == SourceMode::WEBCAM_USB) {
        return cap.read(out_frame);
    }

    // 3. HTTP pull ingestion mode from Go backend (/api/v1/camera/frame)
    if (mode == SourceMode::ESP32_STREAM) {
        if (!curl_handle) {
            return false;
        }

        // Fetch single latest JPEG frame from backend
        if (server::fetchLatestFrame(curl_handle, stream_url, frame_buffer)) {
            if (!frame_buffer.empty()) {
                // Decode binary JPEG into cv::Mat
                out_frame = cv::imdecode(frame_buffer, cv::IMREAD_COLOR);
                return !out_frame.empty();
            }
        }
        return false;
    }

    return false;
}

} // namespace bawal::vision