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

struct __attribute__((__packed__)) CameraFrameHeader {
    uint32_t frame_id{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t format{0};
    uint32_t buffer_len{0};
    int64_t  timestamp_ms{0};
};

struct alignas(16) DecodedFrame {
    CameraFrameHeader header{};  ///< 28 bytes (offset 0..27)
    uint32_t frame_id{0};        ///< 4 bytes  (offset 28..31, aligns image to 32 bytes)
    cv::Mat image;               ///< 96 bytes (offset 32..127)
    bool is_valid{false};        ///< 1 byte   (offset 128)
    uint8_t reserved[15]{0};     ///< 15 bytes explicit padding
};

static_assert(sizeof(DecodedFrame) == 144, "DecodedFrame must be exactly 144 bytes");
static_assert(alignof(DecodedFrame) == 16, "DecodedFrame must be 16-byte aligned");

} // namespace bawal::vision
