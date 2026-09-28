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

struct DecodedFrame {
    CameraFrameHeader header{};
    uint32_t frame_id{0};
    cv::Mat image;
    bool is_valid{false};
};

} // namespace bawal::vision
