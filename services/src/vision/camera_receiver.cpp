#include "bawal/vision/camera_receiver.hpp"
#include <cstring>
#include <iostream>
#include <sstream>

namespace bawal::vision {

static const std::string b64_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static inline bool is_base64(unsigned char c) {
    return (isalnum(c) || (c == '+') || (c == '/'));
}

CameraReceiver::CameraReceiver(const ReceiverConfig& config)
    : m_config(config) {
    if (m_config.prealloc_buffer_size > 0) {
        m_scratch_buffer.reserve(m_config.prealloc_buffer_size);
    }
}

void CameraReceiver::resetMetrics() noexcept {
    m_metrics = ReceiverMetrics{};
}

void CameraReceiver::decodeBase64ToScratch(const std::string& in) {
    m_scratch_buffer.clear();
    size_t in_len = in.size();
    int i = 0;
    int j = 0;
    int in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];

    while (in_len-- && (in[in_] != '=') && is_base64(in[in_])) {
        char_array_4[i++] = in[in_]; in_++;
        if (i == 4) {
            for (i = 0; i < 4; i++) {
                char_array_4[i] = static_cast<unsigned char>(b64_chars.find(char_array_4[i]));
            }
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

            for (i = 0; (i < 3); i++) {
                m_scratch_buffer.push_back(char_array_3[i]);
            }
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 4; j++) {
            char_array_4[j] = 0;
        }
        for (j = 0; j < 4; j++) {
            char_array_4[j] = static_cast<unsigned char>(b64_chars.find(char_array_4[j]));
        }
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

        for (j = 0; (j < i - 1); j++) {
            m_scratch_buffer.push_back(char_array_3[j]);
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

// Lightweight zero-dependency string extraction helper
static std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";

    size_t start = json.find('"', pos);
    if (start == std::string::npos) return "";

    size_t end = json.find('"', start + 1);
    if (end == std::string::npos) return "";

    return json.substr(start + 1, end - start - 1);
}

static uint64_t extractJsonNumber(const std::string& json, const std::string& key, uint64_t default_val = 0) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return default_val;

    pos = json.find(':', pos);
    if (pos == std::string::npos) return default_val;

    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) pos++;

    size_t end = pos;
    while (end < json.size() && (isdigit(json[end]) || json[end] == '-')) end++;

    if (end > pos) {
        try {
            return std::stoull(json.substr(pos, end - pos));
        } catch (...) {}
    }
    return default_val;
}

bool CameraReceiver::decodeFromJson(const std::string& json_str, DecodedFrame& out_frame) {
    out_frame.is_valid = false;

    try {
        CameraFrameHeader header{};
        header.frame_id = static_cast<uint32_t>(extractJsonNumber(json_str, "frame_id", 0));
        header.width = static_cast<uint32_t>(extractJsonNumber(json_str, "width", 320));
        header.height = static_cast<uint32_t>(extractJsonNumber(json_str, "height", 240));
        header.format = static_cast<uint32_t>(extractJsonNumber(json_str, "format", 0));
        header.buffer_len = static_cast<uint32_t>(extractJsonNumber(json_str, "buffer_len", 0));
        header.timestamp_ms = static_cast<int64_t>(extractJsonNumber(json_str, "timestamp_ms", 0));

        std::string b64_str = extractJsonString(json_str, "image_base64");
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
