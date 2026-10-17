#include "bawal/vision/video_source.hpp"
#include "bawal/server/http_utils.hpp"
#include <opencv2/imgcodecs.hpp>
#include <curl/curl.h>
#include <cctype>
#include <chrono>
#include <thread>

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
      frame_buffer(std::move(other.frame_buffer)),
      did_rewind(other.did_rewind) {
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
        did_rewind = other.did_rewind;
        other.curl_handle = nullptr;
    }
    return *this;
}

bool VideoSource::open(SourceMode new_mode, const std::string& uri) {
    mode = new_mode;
    stream_url = uri;
    did_rewind = false;

    if (mode == SourceMode::LOCAL_MP4 || mode == SourceMode::WEBCAM_USB) {
        bool opened = false;
        if (mode == SourceMode::WEBCAM_USB && !uri.empty() &&
            std::isdigit(static_cast<unsigned char>(uri[0])) && uri.find('/') == std::string::npos) {
            opened = cap.open(std::stoi(uri), cv::CAP_V4L2);
            if (!opened) opened = cap.open(std::stoi(uri));
        } else {
            opened = cap.open(uri);
        }

        if (opened && mode == SourceMode::WEBCAM_USB) {
            // Prevent internal V4L2 queue buildup so USB camera always serves latest frame
            cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
        }
        return opened;
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
    // 1. Local MP4 mock video mode with Real-Time Frame Pacing & Auto-Loop
    if (mode == SourceMode::LOCAL_MP4) {
        static auto s_last_frame_time = std::chrono::steady_clock::now();

        double fps = cap.get(cv::CAP_PROP_FPS);
        if (fps <= 1.0 || fps > 120.0) fps = 30.0; // Default 30 FPS if metadata is empty/invalid
        const auto target_duration = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / fps));

        if (!cap.read(out_frame) || out_frame.empty()) {
            cap.set(cv::CAP_PROP_POS_FRAMES, 0); // Rewind video to start
            did_rewind = true;
            if (!cap.read(out_frame) || out_frame.empty()) {
                return false;
            }
        } else {
            did_rewind = false;
        }

        // Pace MP4 frame reading to match live camera speed (prevents 100% CPU and fast-forward)
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - s_last_frame_time;
        if (elapsed < target_duration) {
            std::this_thread::sleep_for(target_duration - elapsed);
        }
        s_last_frame_time = std::chrono::steady_clock::now();
        return true;
    }

    // 2. Physical USB camera mode
    if (mode == SourceMode::WEBCAM_USB) {
        return cap.read(out_frame) && !out_frame.empty();
    }

    // 3. HTTP pull ingestion mode from Go backend (/api/v1/camera/frame)
    if (mode == SourceMode::ESP32_STREAM) {
        if (!curl_handle) {
            return false;
        }

        // Fetch single latest JPEG frame from backend
        if (server::fetchLatestFrame(curl_handle, stream_url, frame_buffer)) {
            if (!frame_buffer.empty()) {
                // Decode in-place into out_frame matrix to recycle allocated memory
                cv::Mat raw_buf(1, static_cast<int>(frame_buffer.size()), CV_8UC1, frame_buffer.data());
                cv::imdecode(raw_buf, cv::IMREAD_COLOR, &out_frame);
                return !out_frame.empty();
            }
        }
        return false;
    }

    return false;
}

} // namespace bawal::vision