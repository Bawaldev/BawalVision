#pragma once

#include "bawal/vision/types.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace bawal::vision {

/**
 * @brief Configuration parameters for CameraReceiver.
 */
struct ReceiverConfig {
    bool auto_convert_bgr = true;              ///< Automatically convert RGB/RGB565 to standard OpenCV BGR
    size_t prealloc_buffer_size = 256 * 1024;  ///< Pre-allocate 256 KB scratch buffer to avoid heap reallocation
    bool track_telemetry = true;
};

/**
 * @brief Performance statistics tracking state for high-throughput video ingestion.
 */
struct ReceiverMetrics {
    uint64_t total_received_frames{0};
    uint64_t corrupted_frames{0};
    int64_t  last_timestamp_ms{0};
};

/**
 * @brief High-performance stateful Camera Receiver using mixed struct + OOP model.
 * Maintains pre-allocated memory buffers to eliminate heap jitter in hot streaming loops.
 */
class CameraReceiver {
public:
    explicit CameraReceiver(const ReceiverConfig& config = ReceiverConfig{});
    ~CameraReceiver() = default;

    // Prevent expensive copies; allow move semantics
    CameraReceiver(const CameraReceiver&) = delete;
    CameraReceiver& operator=(const CameraReceiver&) = delete;
    CameraReceiver(CameraReceiver&&) noexcept = default;
    CameraReceiver& operator=(CameraReceiver&&) noexcept = default;

    /**
     * @brief Decode camera frame directly from raw byte buffer using packed header.
     * Reuses internal pre-allocated scratch memory.
     * 
     * @param header Zero-padded frame header
     * @param raw_bytes Pointer to raw compressed (JPEG) or uncompressed pixel bytes
     * @param len Length of raw_bytes
     * @param out_frame Destination DecodedFrame
     * @return true on successful decode, false on corrupted/invalid data.
     */
    bool decode(const CameraFrameHeader& header, const uint8_t* raw_bytes, size_t len, DecodedFrame& out_frame);

    /**
     * @brief Decode camera frame directly from a contiguous binary packet (header + payload).
     * 
     * @param packet_bytes Pointer to packet buffer (starts with CameraFrameHeader)
     * @param packet_len Total length of packet
     * @param out_frame Destination DecodedFrame
     * @return true on successful decode, false on corrupted/invalid data.
     */
    bool decodeFromBinaryPacket(const uint8_t* packet_bytes, size_t packet_len, DecodedFrame& out_frame);

    /**
     * @brief Decode camera frame from ESP-IDF JSON string.
     * 
     * @param json_str JSON string containing header attributes and optional base64 image data.
     * @param out_frame Destination DecodedFrame
     * @return true on successful decode, false on error.
     */
    bool decodeFromJson(const std::string& json_str, DecodedFrame& out_frame);

    /**
     * @brief Decode raw MJPEG / JPEG buffer from HTTP stream with automatic header repair.
     * 
     * @param raw_bytes Raw compressed JPEG payload
     * @param out_frame Destination DecodedFrame
     * @return true on successful decode, false on error
     */
    bool decodeMJPEG(const uint8_t* raw_bytes, size_t len, DecodedFrame& out_frame);
    bool decodeMJPEG(const std::vector<uint8_t>& raw_bytes, DecodedFrame& out_frame);

    /**
     * @brief Access runtime metrics.
     */
    [[nodiscard]] const ReceiverMetrics& getMetrics() const noexcept { return m_metrics; }
    void resetMetrics() noexcept;

private:
    ReceiverConfig m_config;
    ReceiverMetrics m_metrics;
    std::vector<uint8_t> m_scratch_buffer; ///< Reusable internal scratch buffer to avoid per-frame allocation

    void decodeBase64ToScratch(const std::string& in);
};

} // namespace bawal::vision
