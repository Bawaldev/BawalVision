#include "bawal/vision/camera_receiver.hpp"
#include <charconv>
#include <cstring>
#include <iostream>
#include <sstream>

namespace bawal::vision {

CameraReceiver::CameraReceiver(const ReceiverConfig& config)
    : m_config(config) {
    m_scratch_buffer.reserve(m_config.prealloc_buffer_size);
}

void CameraReceiver::resetMetrics() noexcept {
    m_metrics = ReceiverMetrics{};
}

static const int8_t b64_table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

void CameraReceiver::decodeBase64ToScratch(std::string_view in) {
    m_scratch_buffer.clear();
    const size_t in_len = in.size();
    if (in_len == 0) return;

    m_scratch_buffer.reserve((in_len / 4 + 1) * 3);

    uint32_t buf = 0;
    int bits = 0;

    for (const char ch : in) {
        if (ch == '=') break;
        const int8_t val = b64_table[static_cast<uint8_t>(ch)];
        if (val >= 0) {
            buf = (buf << 6) | static_cast<uint32_t>(val);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                m_scratch_buffer.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
            }
        }
    }
}

bool CameraReceiver::decode(const CameraFrameHeader& header, const uint8_t* raw_bytes, size_t len, DecodedFrame& out_frame) {
    out_frame.header = header;
    out_frame.is_valid = false;

    if (!raw_bytes || len == 0) {
        m_metrics.corrupted_frames++;
        return false;
    }

    try {
        switch (static_cast<FrameFormat>(header.format)) {
            case FrameFormat::JPEG: {
                cv::Mat raw_buf(1, static_cast<int>(len), CV_8UC1, const_cast<uint8_t*>(raw_bytes));
                out_frame.image = cv::imdecode(raw_buf, cv::IMREAD_COLOR);
                out_frame.is_valid = !out_frame.image.empty();
                break;
            }
            case FrameFormat::RGB888: {
                if (header.width > 0 && header.height > 0) {
                    cv::Mat rgb_mat(static_cast<int>(header.height), static_cast<int>(header.width), CV_8UC3, const_cast<uint8_t*>(raw_bytes));
                    if (m_config.auto_convert_bgr) {
                        cv::cvtColor(rgb_mat, out_frame.image, cv::COLOR_RGB2BGR);
                    } else {
                        out_frame.image = rgb_mat.clone();
                    }
                    out_frame.is_valid = !out_frame.image.empty();
                }
                break;
            }
            case FrameFormat::RGB565: {
                if (header.width > 0 && header.height > 0) {
                    cv::Mat rgb565_mat(static_cast<int>(header.height), static_cast<int>(header.width), CV_8UC2, const_cast<uint8_t*>(raw_bytes));
                    cv::cvtColor(rgb565_mat, out_frame.image, cv::COLOR_BGR5652BGR);
                    out_frame.is_valid = !out_frame.image.empty();
                }
                break;
            }
            case FrameFormat::GRAYSCALE: {
                if (header.width > 0 && header.height > 0) {
                    cv::Mat gray_mat(static_cast<int>(header.height), static_cast<int>(header.width), CV_8UC1, const_cast<uint8_t*>(raw_bytes));
                    out_frame.image = gray_mat.clone();
                    out_frame.is_valid = !out_frame.image.empty();
                }
                break;
            }
            default:
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "[CameraReceiver] Decode error: " << e.what() << std::endl;
        out_frame.is_valid = false;
    }

    if (out_frame.is_valid) {
        m_metrics.total_received_frames++;
        m_metrics.last_timestamp_ms = header.timestamp_ms;
        return true;
    } else {
        m_metrics.corrupted_frames++;
        return false;
    }
}

bool CameraReceiver::decodeFromBinaryPacket(const uint8_t* packet_bytes, size_t packet_len, DecodedFrame& out_frame) {
    out_frame.is_valid = false;

    if (!packet_bytes || packet_len < sizeof(CameraFrameHeader)) {
        m_metrics.corrupted_frames++;
        return false;
    }

    CameraFrameHeader header;
    std::memcpy(&header, packet_bytes, sizeof(CameraFrameHeader));

    const uint8_t* payload_ptr = packet_bytes + sizeof(CameraFrameHeader);
    size_t payload_len = packet_len - sizeof(CameraFrameHeader);

    return decode(header, payload_ptr, payload_len, out_frame);
}

static size_t findJsonKey(std::string_view json, std::string_view key) {
    size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string_view::npos) {
        if (pos > 0 && json[pos - 1] == '"' && 
            (pos + key.size()) < json.size() && json[pos + key.size()] == '"') {
            return pos + key.size() + 1;
        }
        pos += key.size();
    }
    return std::string_view::npos;
}

static std::string_view extractJsonString(std::string_view json, std::string_view key) {
    size_t pos = findJsonKey(json, key);
    if (pos == std::string_view::npos) return {};

    pos = json.find(':', pos);
    if (pos == std::string_view::npos) return {};

    size_t start = json.find('"', pos);
    if (start == std::string_view::npos) return {};

    size_t end = json.find('"', start + 1);
    if (end == std::string_view::npos) return {};

    return json.substr(start + 1, end - start - 1);
}

static int64_t extractJsonNumber(std::string_view json, std::string_view key, int64_t default_val = 0) {
    size_t pos = findJsonKey(json, key);
    if (pos == std::string_view::npos) return default_val;

    pos = json.find(':', pos);
    if (pos == std::string_view::npos) return default_val;

    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;

    size_t end = pos;
    while (end < json.size() && (isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) end++;

    if (end > pos) {
        int64_t val = default_val;
        auto [ptr, ec] = std::from_chars(json.data() + pos, json.data() + end, val);
        if (ec == std::errc()) {
            return val;
        }
    }
    return default_val;
}

bool CameraReceiver::decodeFromJson(std::string_view json_str, DecodedFrame& out_frame) {
    out_frame.is_valid = false;

    try {
        CameraFrameHeader header{};
        header.frame_id = static_cast<uint32_t>(extractJsonNumber(json_str, "frame_id", 0));
        header.width = static_cast<uint32_t>(extractJsonNumber(json_str, "width", 320));
        header.height = static_cast<uint32_t>(extractJsonNumber(json_str, "height", 240));
        header.format = static_cast<uint32_t>(extractJsonNumber(json_str, "format", 0));
        header.buffer_len = static_cast<uint32_t>(extractJsonNumber(json_str, "buffer_len", 0));
        header.timestamp_ms = static_cast<int64_t>(extractJsonNumber(json_str, "timestamp_ms", 0));

        std::string_view b64_str = extractJsonString(json_str, "image_base64");
        if (!b64_str.empty()) {
            decodeBase64ToScratch(b64_str);
            if (!m_scratch_buffer.empty()) {
                return decode(header, m_scratch_buffer.data(), m_scratch_buffer.size(), out_frame);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[CameraReceiver] JSON decode error: " << e.what() << std::endl;
    }

    m_metrics.corrupted_frames++;
    return false;
}

bool CameraReceiver::decodeMJPEG(const uint8_t* raw_bytes, size_t len, DecodedFrame& out_frame) {
    out_frame.is_valid = false;
    if (!raw_bytes || len == 0) {
        m_metrics.corrupted_frames++;
        return false;
    }

    CameraFrameHeader header{};
    header.format = static_cast<uint32_t>(FrameFormat::JPEG);
    header.buffer_len = static_cast<uint32_t>(len);
    return decode(header, raw_bytes, len, out_frame);
}

bool CameraReceiver::decodeMJPEG(const std::vector<uint8_t>& raw_bytes, DecodedFrame& out_frame) {
    return decodeMJPEG(raw_bytes.data(), raw_bytes.size(), out_frame);
}

} // namespace bawal::vision
