#include "bawal/vision/video_source.hpp"
#include "bawal/server/http_utils.hpp"
#include "bawal/vision/camera_receiver.hpp"
#include "bawal/vision/enchanter.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <curl/curl.h>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

namespace bawal::vision {

namespace {

uint64_t computeFastFingerprint(const std::vector<uint8_t>& buf) noexcept {
    const size_t len = buf.size();
    if (len < 32) return len;
    uint64_t p0 = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0;
    std::memcpy(&p0, buf.data(), 8);
    std::memcpy(&p1, buf.data() + (len / 4), 8);
    std::memcpy(&p2, buf.data() + (len / 2), 8);
    std::memcpy(&p3, buf.data() + (len * 3 / 4), 8);
    std::memcpy(&p4, buf.data() + len - 8, 8);
    uint64_t h = len;
    h ^= p0 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= p1 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= p2 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= p3 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= p4 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

} // anonymous namespace

VideoSource::VideoSource() {
    clahe = cv::createCLAHE(1.5, cv::Size(8, 8));
}

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
      fallback_url(std::move(other.fallback_url)),
      frame_buffer(std::move(other.frame_buffer)),
      receiver(std::move(other.receiver)),
      last_decoded_frame(std::move(other.last_decoded_frame)),
      last_frame_hash(other.last_frame_hash),
      clahe(std::move(other.clahe)),
      target_width(other.target_width),
      target_height(other.target_height),
      enable_enchant(other.enable_enchant),
      did_rewind(other.did_rewind),
      is_duplicate_frame(other.is_duplicate_frame) {
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
        fallback_url = std::move(other.fallback_url);
        frame_buffer = std::move(other.frame_buffer);
        receiver = std::move(other.receiver);
        last_decoded_frame = std::move(other.last_decoded_frame);
        last_frame_hash = other.last_frame_hash;
        clahe = std::move(other.clahe);
        target_width = other.target_width;
        target_height = other.target_height;
        enable_enchant = other.enable_enchant;
        did_rewind = other.did_rewind;
        is_duplicate_frame = other.is_duplicate_frame;
        other.curl_handle = nullptr;
    }
    return *this;
}

bool VideoSource::open(SourceMode new_mode, const std::string& uri) {
    mode = new_mode;
    stream_url = uri;
    did_rewind = false;
    is_duplicate_frame = false;
    last_frame_hash = 0;

    if (!clahe) {
        clahe = cv::createCLAHE(1.5, cv::Size(8, 8));
    }

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
        // Pre-allocate 256 KB buffer to prevent repeated heap allocations
        frame_buffer.reserve(256 * 1024);
        return (curl_handle != nullptr);
    }

    return false;
}

bool VideoSource::grabNextFrame(cv::Mat& out_frame) {
    cv::Mat raw_frame;
    is_duplicate_frame = false;

    // 1. Local MP4 mock video mode with Real-Time Frame Pacing & Auto-Loop
    if (mode == SourceMode::LOCAL_MP4) {
        static auto s_last_frame_time = std::chrono::steady_clock::now();

        double fps = cap.get(cv::CAP_PROP_FPS);
        if (fps <= 1.0 || fps > 120.0) fps = 30.0; // Default 30 FPS if metadata is empty/invalid
        const auto target_duration = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / fps));

        if (!cap.read(raw_frame) || raw_frame.empty()) {
            cap.set(cv::CAP_PROP_POS_FRAMES, 0); // Rewind video to start
            did_rewind = true;
            if (!cap.read(raw_frame) || raw_frame.empty()) {
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
    }
    // 2. Physical USB camera mode
    else if (mode == SourceMode::WEBCAM_USB) {
        if (!cap.read(raw_frame) || raw_frame.empty()) {
            return false;
        }
    }
    // 3. HTTP pull ingestion mode from Go backend (/api/v1/camera/latest/<id>/raw)
    else if (mode == SourceMode::ESP32_STREAM) {
        if (!curl_handle) {
            return false;
        }

        bool fetched = server::fetchLatestFrame(curl_handle, stream_url, frame_buffer) && !frame_buffer.empty();
        if (!fetched && !fallback_url.empty() && fallback_url != stream_url) {
            fetched = server::fetchLatestFrame(curl_handle, fallback_url, frame_buffer) && !frame_buffer.empty();
        }
        if (!fetched) {
            frame_buffer.clear();
            return false;
        }

        const uint64_t cur_hash = computeFastFingerprint(frame_buffer);
        if (cur_hash == last_frame_hash && !last_decoded_frame.empty()) {
            is_duplicate_frame = true;
            raw_frame = last_decoded_frame;
        } else {
            DecodedFrame decoded;
            if (receiver.decodeMJPEG(frame_buffer.data(), frame_buffer.size(), decoded) && !decoded.image.empty()) {
                last_frame_hash = cur_hash;
                last_decoded_frame = decoded.image;
                raw_frame = last_decoded_frame;
            } else {
                // Fallback direct imdecode if binary header heuristic did not match
                cv::Mat raw_buf(1, static_cast<int>(frame_buffer.size()), CV_8UC1, frame_buffer.data());
                cv::imdecode(raw_buf, cv::IMREAD_COLOR, &last_decoded_frame);
                if (last_decoded_frame.empty()) {
                    return false;
                }
                last_frame_hash = cur_hash;
                raw_frame = last_decoded_frame;
            }
        }
    } else {
        return false;
    }

    if (raw_frame.empty()) {
        return false;
    }

    // Normalize frame resolution to target dimensions (640x480) before ISP / Vision pipeline
    cv::Mat normalized;
    if (target_width > 0 && target_height > 0 &&
        (raw_frame.cols != target_width || raw_frame.rows != target_height)) {
        cv::resize(raw_frame, normalized, cv::Size(target_width, target_height), 0, 0, cv::INTER_AREA);
    } else {
        normalized = raw_frame;
    }

    // Direct Enchanter ISP integration when enabled on VideoSource
    if (enable_enchant) {
        if (!clahe) {
            clahe = cv::createCLAHE(1.5, cv::Size(8, 8));
        }
        IlluminationEnhancerAI::process(normalized, out_frame, clahe);
    } else {
        out_frame = normalized;
    }

    return !out_frame.empty();
}

bool VideoSource::grabAndEnchantFrame(cv::Mat& out_frame, RealESRGANEngine* sr_engine) {
    const bool prev_enchant = enable_enchant;
    enable_enchant = false;
    cv::Mat base_frame;
    const bool ok = grabNextFrame(base_frame);
    enable_enchant = prev_enchant;
    if (!ok || base_frame.empty()) {
        return false;
    }

    if (!clahe) {
        clahe = cv::createCLAHE(1.5, cv::Size(8, 8));
    }

    cv::Mat isp_enhanced;
    IlluminationEnhancerAI::process(base_frame, isp_enhanced, clahe);

    if (sr_engine && sr_engine->isReady()) {
        cv::Mat sr_frame;
        if (sr_engine->process(isp_enhanced, sr_frame) && !sr_frame.empty()) {
            if (target_width > 0 && target_height > 0 &&
                (sr_frame.cols != target_width || sr_frame.rows != target_height)) {
                cv::resize(sr_frame, out_frame, cv::Size(target_width, target_height), 0, 0, cv::INTER_AREA);
            } else {
                out_frame = sr_frame;
            }
            return !out_frame.empty();
        }
    }

    out_frame = isp_enhanced;
    return !out_frame.empty();
}

} // namespace bawal::vision