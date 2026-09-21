#pragma once 

#include <cstdint>
#include <string>
#include <vector>
#include <opencv2/core.hpp>

namespace bawal::vision {

enum class FrameFormat : uint32_t {
    JPEG = 0,
    RGB565 = 1,
    RGB888 = 2,
    GRAYSCALE = 3,
    RAW = 4
};

#pragma pack(push, 1)
/**
 * @brief Packed camera frame header matching ESP-IDF camera_frame_header_t (zero padding).
 */
struct CameraFrameHeader {
    uint32_t frame_id;         ///< Monotonic sequence identifier
    uint32_t width;            ///< Width in pixels
    uint32_t height;           ///< Height in pixels
    uint32_t format;           ///< Pixel format enum (FrameFormat)
    uint32_t buffer_len;       ///< Exact byte length of image payload
    int64_t  timestamp_ms;     ///< Capture millisecond epoch/uptime
};

/**
 * @brief Packed telemetry data structure matching ESP-IDF telemetry_data_t (zero padding).
 */
struct TelemetryPacket {
    char device_id[64];
    char firmware_version[54];
    int64_t timestamp;
    float temperature;
    float humidity;
    float gas_voc;
    float pressure;
    uint8_t current_cpu_load;
    char device_name[100];
    uint8_t device_core;
};
#pragma pack(pop)

/**
 * @brief Decoded OpenCV frame container with synchronized metadata.
 */
struct DecodedFrame {
    CameraFrameHeader header;
    cv::Mat image;             ///< OpenCV image matrix (BGR / Grayscale)
    bool is_valid = false;
};

} // namespace bawal::vision