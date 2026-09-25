#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <curl/curl.h>
#include <opencv2/core.hpp>

namespace bawal::server {

struct CameraConfig {
    std::string backend_url;
    std::string device_id;
    int timeout_ms = 400;
};

// Free Functions
bool fetchLatestFrame(CURL* curl, const std::string& url, std::vector<uint8_t>& buffer);
std::string discoverActiveDeviceID(CURL* curl, const std::string& backend_base_url);
cv::Mat decodeMJPEGFrame(const std::vector<uint8_t>& raw_bytes);

} // namespace bawal::server