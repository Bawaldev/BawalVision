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
 * @brief Sends a Live ESP32-S3 inspection JSON payload with food detections to the backend service.
 * 
 * Target endpoint: POST {backend_base_url}/api/v1/inspections
 * Headers: X-Inspection-Source: LIVE_ESP32, X-Inspection-Struct: IngressInspectionRequest
 * 
 * @param curl Active libcurl easy handle.
 * @param backend_base_url Base URL of the backend service (e.g. http://localhost:8080).
 * @param json_payload JSON string containing IngressInspectionRequest payload.
 * @return true if backend returned HTTP 201 Created or HTTP 200 OK.
 */
[[nodiscard]] bool postInspectionPayload(CURL* curl, std::string_view backend_base_url, std::string_view json_payload);

/**
 * @brief Sends a Mock Video inspection JSON payload (MockInspectionRequest) to the backend service.
 * 
 * Target endpoint: POST {backend_base_url}/api/v1/inspections/mock
 * Headers: X-Inspection-Source: MOCK_VIDEO, X-Inspection-Struct: MockInspectionRequest, X-Telemetry-Struct: MockTelemetryPayload
 * 
 * @param curl Active libcurl easy handle.
 * @param backend_base_url Base URL of the backend service (e.g. http://localhost:8080).
 * @param json_payload JSON string containing MockInspectionRequest payload.
 * @return true if backend returned HTTP 201 Created or HTTP 200 OK.
 */
[[nodiscard]] bool postMockInspectionPayload(CURL* curl, std::string_view backend_base_url, std::string_view json_payload);


/**
 * @brief Transmits an annotated JPEG frame with visual bounding boxes to the backend.
 * 
 * Target endpoint: POST {backend_base_url}/api/v1/camera/annotated
 * 
 * @param curl Active libcurl easy handle.
 * @param backend_base_url Base URL of the backend service.
 * @param device_id Target device identifier.
 * @param frame_id Monotonic frame sequence ID.
 * @param jpeg_bytes JPEG encoded annotated image bytes.
 * @return true if backend returned HTTP 200 OK.
 */
[[nodiscard]] bool postAnnotatedFrame(CURL* curl, std::string_view backend_base_url, std::string_view device_id, uint32_t frame_id, const std::vector<uint8_t>& jpeg_bytes);

/**
 * @brief Transmits a recorded video container (MP4/H.264 binary) to the backend service.
 * 
 * Target endpoint: POST {backend_base_url}/api/v1/video
 * Sends raw video binary stream with custom metadata headers.
 * 
 * @param curl Active libcurl easy handle.
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
 * @brief Sends video manifest metadata in JSON format to /api/v1/video.
 * 
 * Used when video upload utilizes presigned S3 URLs or streaming metadata manifests.
 * 
 * @param curl Active libcurl easy handle.
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
 * @brief Queries the backend for dynamic detection confidence threshold.
 * 
 * Target endpoint: GET {backend_base_url}/api/v1/system/threshold
 * 
 * @param curl Active libcurl easy handle.
 * @param backend_base_url Base URL of backend service.
 * @param fallback_val Default fallback threshold (default 0.60f).
 * @return Dynamic threshold value clamped between 0.45f and 0.98f.
 */
[[nodiscard]] float fetchDynamicThreshold(
    CURL* curl,
    std::string_view backend_base_url,
    float fallback_val = 0.60f
);

} // namespace bawal::server