#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <string_view>
// #include <curl/curl.h>
// #include <opencv2/core.hpp>

// forward declaration
typedef void CURL;

namespace bawal::server {

struct CameraConfig {
    std::string backend_url;
    std::string device_id;
    int timeout_ms{400};
};
/**
 * @param curl Active libcurl handle.
 * @param url Target endpoint URL.
 * @param buffer Output buffer to receive raw image bytes.
 * @return true if the HTTP request succeeded, false otherwise.
 */
[[nodiscard]] bool fetchLatestFrame(CURL* curl, std::string_view url, std::vector<uint8_t>& buffer);
/**
 * @param curl Active libcurl handle.
 * @param backend_base_url Base URL of the backend service.
 * @return Device ID string if discovered, empty string otherwise.
 */
[[nodiscard]] std::string discoverActiveDeviceID(CURL* curl, std::string_view backend_base_url);
/**
 * @brief Sends an inspection JSON payload with food detections to the backend service.
 * @param curl Active libcurl handle.
 * @param backend_base_url Base URL of the backend service (e.g. http://localhost:8080).
 * @param json_payload JSON string containing device_id and detections array.
 * @return true if backend returned HTTP 201 Created or HTTP 200 OK.
 */
[[nodiscard]] bool postInspectionPayload(CURL* curl, std::string_view backend_base_url, std::string_view json_payload);

} // namespace bawal::server