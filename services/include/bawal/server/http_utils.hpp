#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Forward declaration for libcurl handle
typedef void CURL;

namespace bawal::server {

/**
 * @brief Remote camera connection parameters.
 */
struct CameraConfig {
    std::string backend_url;
    std::string device_id;
    int timeout_ms{400};
};

/**
 * @brief Synchronously fetches the latest frame byte payload from the specified endpoint URL.
 * 
 * @param curl Active libcurl easy handle.
 * @param url Target endpoint URL.
 * @param buffer Output buffer to receive raw image bytes.
 * @return true if HTTP status 200 OK was received with non-empty payload.
 */
[[nodiscard]] bool fetchLatestFrame(CURL* curl, std::string_view url, std::vector<uint8_t>& buffer);

/**
 * @brief Discovers the active ESP32 device identifier by querying the backend discovery endpoint.
 * 
 * @param curl Active libcurl easy handle.
 * @param backend_base_url Base URL of the backend service (e.g. http://localhost:8080).
 * @return Device ID string if discovered, empty string otherwise.
 */
[[nodiscard]] std::string discoverActiveDeviceID(CURL* curl, std::string_view backend_base_url);

/**
 * @brief Sends an inspection JSON payload with food detections to the backend service.
 * 
 * @param curl Active libcurl easy handle.
 * @param backend_base_url Base URL of the backend service (e.g. http://localhost:8080).
 * @param json_payload JSON string containing device_id and detections array.
 * @return true if backend returned HTTP 201 Created or HTTP 200 OK.
 */
[[nodiscard]] bool postInspectionPayload(CURL* curl, std::string_view backend_base_url, std::string_view json_payload);

/**
 * @brief Transmits an annotated JPEG frame with visual bounding boxes to the backend.
 * 
 * @param curl Active libcurl easy handle.
 * @param backend_base_url Base URL of the backend service.
 * @param device_id Target device identifier.
 * @param frame_id Monotonic frame sequence ID.
 * @param jpeg_bytes JPEG encoded annotated image bytes.
 * @return true if backend returned HTTP 200 OK.
 */
[[nodiscard]] bool postAnnotatedFrame(CURL* curl, std::string_view backend_base_url, std::string_view device_id, uint32_t frame_id, const std::vector<uint8_t>& jpeg_bytes);

} // namespace bawal::server