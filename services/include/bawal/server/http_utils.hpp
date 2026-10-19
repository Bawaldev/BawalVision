#pragma once

#include <httplib.h>
#include <fmt/core.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Forward declaration for legacy libcurl handle compatibility
typedef void CURL;

namespace bawal::server {

/**
 * @brief Remote camera connection parameters.
 */
struct alignas(8) CameraConfig {
    std::string backend_url;        ///< 32 bytes (offset 0..31)
    std::string device_id;          ///< 32 bytes (offset 32..63)
    int32_t timeout_ms{400};        ///< 4 bytes (offset 64..67)
    uint8_t reserved[4]{0};         ///< 4 bytes padding (offset 68..71)
};

static_assert(alignof(CameraConfig) == 8, "CameraConfig must be 8-byte aligned");
#if defined(__GLIBCXX__) || defined(__x86_64__)
static_assert(sizeof(CameraConfig) == 72, "CameraConfig must have 72 bytes in 64-bit ABI");
#endif

/**
 * @brief Returns a thread-local persistent httplib::Client configured with Keep-Alive and TCP_NODELAY.
 *
 * @param backend_base_url Base URL of the backend service (e.g. "http://localhost:8080").
 * @param conn_timeout_ms Connection timeout in milliseconds.
 * @param rw_timeout_ms Read/Write socket timeout in milliseconds.
 * @return httplib::Client& Reference to the thread-local persistent HTTP client.
 */
[[nodiscard]] httplib::Client& getPersistentHttpClient(
    std::string_view backend_base_url,
    int conn_timeout_ms = 800,
    int rw_timeout_ms = 1500
);

/**
 * @brief Synchronously fetches the latest frame byte payload from the specified endpoint URL via httplib::Client.
 *
 * @param curl Legacy handle pointer (unused when using persistent httplib::Client; retained for ABI compatibility).
 * @param url Target endpoint URL (full URL or path).
 * @param buffer Output buffer to receive raw image bytes.
 * @return true if HTTP status 200 OK was received with non-empty payload.
 */
[[nodiscard]] bool fetchLatestFrame(CURL* curl, std::string_view url, std::vector<uint8_t>& buffer);

/**
 * @brief Discovers the active ESP32 or Mock device identifier by querying the backend discovery endpoint via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of the backend service (e.g. "http://localhost:8080").
 * @return std::string Device ID string if discovered, empty string otherwise.
 */
[[nodiscard]] std::string discoverActiveDeviceID(CURL* curl, std::string_view backend_base_url);

/**
 * @brief Sends a Live ESP32-S3 telemetry JSON payload (TelemetryIngressPayload) to /api/v1/telemetry via httplib and fmt.
 *
 * @param backend_base_url Base URL of the backend service.
 * @param json_payload JSON string formatted with fmt::format containing TelemetryIngressPayload.
 * @return true if backend returned HTTP 2xx status.
 */
[[nodiscard]] bool postTelemetryPayload(std::string_view backend_base_url, std::string_view json_payload);

/**
 * @brief Sends a Mock Video simulation telemetry JSON payload (MockTelemetryPayload) to /api/v1/telemetry/mock via httplib and fmt.
 *
 * @param backend_base_url Base URL of the backend service.
 * @param json_payload JSON string formatted with fmt::format containing MockTelemetryPayload.
 * @return true if backend returned HTTP 2xx status.
 */
[[nodiscard]] bool postMockTelemetryPayload(std::string_view backend_base_url, std::string_view json_payload);

/**
 * @brief Sends a Live ESP32-S3 inspection JSON payload with food detections to /api/v1/inspections via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of the backend service (e.g. "http://localhost:8080").
 * @param json_payload JSON string containing IngressInspectionRequest payload.
 * @return true if backend returned HTTP 201 Created or HTTP 200 OK.
 */
[[nodiscard]] bool postInspectionPayload(CURL* curl, std::string_view backend_base_url, std::string_view json_payload);

/**
 * @brief Sends a Mock Video inspection JSON payload (MockInspectionRequest) to /api/v1/inspections/mock via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of the backend service (e.g. "http://localhost:8080").
 * @param json_payload JSON string containing MockInspectionRequest payload.
 * @return true if backend returned HTTP 201 Created or HTTP 200 OK.
 */
[[nodiscard]] bool postMockInspectionPayload(CURL* curl, std::string_view backend_base_url, std::string_view json_payload);

/**
 * @brief Transmits an annotated JPEG frame with visual bounding boxes to /api/v1/camera/annotated via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of the backend service.
 * @param device_id Target device identifier.
 * @param frame_id Monotonic frame sequence ID.
 * @param jpeg_bytes JPEG encoded annotated image bytes.
 * @return true if backend returned HTTP 200 OK.
 */
[[nodiscard]] bool postAnnotatedFrame(
    CURL* curl,
    std::string_view backend_base_url,
    std::string_view device_id,
    uint32_t frame_id,
    const std::vector<uint8_t>& jpeg_bytes
);

/**
 * @brief Transmits a recorded video container (MP4/H.264 binary) to /api/v1/video via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of backend service (e.g. "http://localhost:8080").
 * @param device_id Unique identifier of transmitting device (attached in X-Device-ID header).
 * @param video_bytes Binary buffer containing full video file (e.g. encoded MP4).
 * @param content_type MIME type of the video (default: "video/mp4").
 * @param timeout_ms Upload transfer timeout in milliseconds (default: 5000 ms).
 * @return true if server responds with HTTP 200 OK or HTTP 201 Created, false on error.
 */
[[nodiscard]] bool postVideoPayload(
    CURL* curl,
    std::string_view backend_base_url,
    std::string_view device_id,
    const std::vector<uint8_t>& video_bytes,
    std::string_view content_type = "video/mp4",
    long timeout_ms = 5000
);

/**
 * @brief Sends video manifest metadata in JSON format to /api/v1/video via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of backend service.
 * @param json_payload JSON string containing video metadata information.
 * @param timeout_ms Transmission timeout limit in milliseconds (default: 1500 ms).
 * @return true if server responds with HTTP 200 OK or HTTP 201 Created.
 */
[[nodiscard]] bool postVideoMetadata(
    CURL* curl,
    std::string_view backend_base_url,
    std::string_view json_payload,
    long timeout_ms = 1500
);

/**
 * @brief Queries the backend for dynamic detection confidence threshold via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of backend service.
 * @param fallback_val Default fallback threshold (default 0.60f).
 * @return float Dynamic threshold value clamped between 0.25f and 0.98f.
 */
[[nodiscard]] float fetchDynamicThreshold(
    CURL* curl,
    std::string_view backend_base_url,
    float fallback_val = 0.60f
);

} // namespace bawal::server