#pragma once

#include <string>
#include <vector>
#include <cstdint>
// #include <curl/curl.h>
// #include <opencv2/core.hpp>

// forward declaration
typedef void CURL;

namespace bawal::server {

struct CameraConfig {
    std::string backend_url;
    std::string device_id;
    int timeout_ms = 400;
};
/**
 * @param curl Active libcurl handle.
 * @param url Target endpoint URL.
 * @param buffer Output buffer to receive raw image bytes.
 * @return true if the HTTP request succeeded, false otherwise.
 */
bool fetchLatestFrame(CURL* curl, const std::string& url, std::vector<uint8_t>& buffer);
/**
 * @param curl Active libcurl handle.
 * @param backend_base_url Base URL of the backend service.
 * @return Device ID string if discovered, empty string otherwise.
 */
std::string discoverActiveDeviceID(CURL* curl, const std::string& backend_base_url);

} // namespace bawal::server