#include <bawal/server/http_utils.hpp>
#include <httplib.h>
#include <fmt/core.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

/**
 * @brief Splits a full HTTP URL into its scheme://host:port base and request path.
 *
 * @param full_url Complete URL string (e.g. "http://localhost:8080/api/v1/camera/active").
 * @return std::pair<std::string, std::string> Pair of (base_url, path).
 */
std::pair<std::string, std::string> splitUrlBaseAndPath(std::string_view full_url) {
    if (full_url.empty()) {
        return {"http://localhost:8080", "/"};
    }

    size_t path_start = std::string_view::npos;
    const size_t scheme_pos = full_url.find("://");
    if (scheme_pos != std::string_view::npos) {
        path_start = full_url.find('/', scheme_pos + 3);
    } else if (full_url.front() == '/') {
        return {"http://localhost:8080", std::string(full_url)};
    } else {
        path_start = full_url.find('/');
    }

    if (path_start == std::string_view::npos) {
        return {std::string(full_url), "/"};
    }

    std::string base(full_url.substr(0, path_start));
    std::string path(full_url.substr(path_start));
    if (path.empty()) {
        path = "/";
    }
    return {std::move(base), std::move(path)};
}

} // namespace

namespace bawal::server {

/**
 * @brief Returns a thread-local persistent httplib::Client configured with Keep-Alive and TCP_NODELAY.
 *
 * Maintains one persistent socket per thread per backend origin to eliminate TCP 3-way handshake
 * overhead and ephemeral port exhaustion during 30 FPS streaming and telemetry dispatch.
 *
 * @param backend_base_url Base URL of the backend service (e.g. "http://localhost:8080").
 * @param conn_timeout_ms Connection timeout in milliseconds.
 * @param rw_timeout_ms Read/Write socket timeout in milliseconds.
 * @return httplib::Client& Reference to the thread-local persistent HTTP client.
 */
httplib::Client& getPersistentHttpClient(
    std::string_view backend_base_url,
    int conn_timeout_ms,
    int rw_timeout_ms
) {
    thread_local std::unordered_map<std::string, std::unique_ptr<httplib::Client>> tls_clients;
    thread_local std::string tls_last_key;
    thread_local httplib::Client* tls_last_client = nullptr;

    // Fast path: production URLs are stable, so avoid re-splitting and map
    // lookup (and its temporary std::string) on every 30 FPS request.
    if (tls_last_client != nullptr && backend_base_url == tls_last_key) {
        tls_last_client->set_connection_timeout(std::chrono::milliseconds(conn_timeout_ms > 0 ? conn_timeout_ms : 800));
        tls_last_client->set_read_timeout(std::chrono::milliseconds(rw_timeout_ms > 0 ? rw_timeout_ms : 1500));
        tls_last_client->set_write_timeout(std::chrono::milliseconds(rw_timeout_ms > 0 ? rw_timeout_ms : 1500));
        return *tls_last_client;
    }

    const auto [base_key, _] = splitUrlBaseAndPath(backend_base_url);
    auto it = tls_clients.find(base_key);
    if (it == tls_clients.end() || !it->second) {
        auto cli = std::make_unique<httplib::Client>(base_key);
        cli->set_keep_alive(true);
        cli->set_tcp_nodelay(true);
        it = tls_clients.emplace(base_key, std::move(cli)).first;
    }

    httplib::Client& client = *(it->second);
    tls_last_key.assign(backend_base_url);
    tls_last_client = &client;
    client.set_connection_timeout(std::chrono::milliseconds(conn_timeout_ms > 0 ? conn_timeout_ms : 800));
    client.set_read_timeout(std::chrono::milliseconds(rw_timeout_ms > 0 ? rw_timeout_ms : 1500));
    client.set_write_timeout(std::chrono::milliseconds(rw_timeout_ms > 0 ? rw_timeout_ms : 1500));
    return client;
}

/**
 * @brief Synchronously fetches the latest frame byte payload from the specified endpoint URL via httplib::Client.
 *
 * @param curl Legacy handle pointer (unused; persistent httplib::Client is used for keep-alive connectivity).
 * @param url Target endpoint URL.
 * @param buffer Output buffer to receive raw image bytes.
 * @return true if HTTP status 200 OK was received with non-empty payload.
 */
bool fetchLatestFrame([[maybe_unused]] CURL* curl, std::string_view url, std::vector<uint8_t>& buffer) {
    if (url.empty()) {
        return false;
    }

    buffer.clear();
    const auto [base_url, path] = splitUrlBaseAndPath(url);
    auto& cli = getPersistentHttpClient(base_url, 800, 1500);

    const httplib::Headers headers = {
        {"Accept", "image/jpeg, application/octet-stream, */*"},
        {"Cache-Control", "no-cache"}
    };

    auto res = cli.Get(path, headers);
    if (!res || res->status != 200 || res->body.empty()) {
        return false;
    }

    buffer.assign(
        reinterpret_cast<const uint8_t*>(res->body.data()),
        reinterpret_cast<const uint8_t*>(res->body.data()) + res->body.size()
    );
    return !buffer.empty();
}

/**
 * @brief Discovers the active ESP32 or Mock device identifier by querying /api/v1/camera/active via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of the backend service (e.g. "http://localhost:8080").
 * @return std::string Device ID string if discovered, empty string otherwise.
 */
std::string discoverActiveDeviceID([[maybe_unused]] CURL* curl, std::string_view backend_base_url) {
    if (backend_base_url.empty()) {
        return "";
    }

    auto& cli = getPersistentHttpClient(backend_base_url, 800, 1200);
    auto res = cli.Get("/api/v1/camera/active");
    if (!res || res->status != 200 || res->body.empty()) {
        return "";
    }

    const std::string_view json_payload(res->body);
    constexpr std::string_view key = "\"device_id\"";
    const size_t key_pos = json_payload.find(key);
    if (key_pos == std::string_view::npos) {
        return "";
    }

    const size_t colon_pos = json_payload.find(':', key_pos + key.length());
    if (colon_pos == std::string_view::npos) {
        return "";
    }

    const size_t start_quote = json_payload.find('"', colon_pos + 1);
    if (start_quote == std::string_view::npos) {
        return "";
    }

    const size_t end_quote = json_payload.find('"', start_quote + 1);
    if (end_quote == std::string_view::npos) {
        return "";
    }

    return std::string(json_payload.substr(start_quote + 1, end_quote - start_quote - 1));
}

/**
 * @brief Sends a Live ESP32-S3 telemetry JSON payload (TelemetryIngressPayload) to /api/v1/telemetry via httplib.
 *
 * @param backend_base_url Base URL of the backend service.
 * @param json_payload JSON string formatted with fmt::format containing TelemetryIngressPayload.
 * @return true if backend returned HTTP 2xx status.
 */
bool postTelemetryPayload(std::string_view backend_base_url, std::string_view json_payload) {
    if (backend_base_url.empty() || json_payload.empty()) {
        return false;
    }

    auto& cli = getPersistentHttpClient(backend_base_url, 800, 1200);
    const httplib::Headers headers = {
        {"X-Stream-Source", "LIVE_ESP32"},
        {"X-Telemetry-Mode", "LIVE_HARDWARE"},
        {"X-Telemetry-Struct", "TelemetryIngressPayload"}
    };

    auto res = cli.Post(
        "/api/v1/telemetry",
        headers,
        json_payload.data(),
        json_payload.size(),
        "application/json"
    );
    return (res && res->status >= 200 && res->status < 300);
}

/**
 * @brief Sends a Mock Video simulation telemetry JSON payload (MockTelemetryPayload) to /api/v1/telemetry/mock via httplib.
 *
 * @param backend_base_url Base URL of the backend service.
 * @param json_payload JSON string formatted with fmt::format containing MockTelemetryPayload.
 * @return true if backend returned HTTP 2xx status.
 */
bool postMockTelemetryPayload(std::string_view backend_base_url, std::string_view json_payload) {
    if (backend_base_url.empty() || json_payload.empty()) {
        return false;
    }

    auto& cli = getPersistentHttpClient(backend_base_url, 800, 1200);
    const httplib::Headers headers = {
        {"X-Stream-Source", "MOCK_VIDEO"},
        {"X-Telemetry-Mode", "MOCK_SIMULATION"},
        {"X-Telemetry-Struct", "MockTelemetryPayload"}
    };

    auto res = cli.Post(
        "/api/v1/telemetry/mock",
        headers,
        json_payload.data(),
        json_payload.size(),
        "application/json"
    );
    return (res && res->status >= 200 && res->status < 300);
}

/**
 * @brief Sends a Live ESP32-S3 inspection JSON payload (IngressInspectionRequest) to /api/v1/inspections via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of the backend service.
 * @param json_payload JSON string containing IngressInspectionRequest payload.
 * @return true if backend returned HTTP 2xx status.
 */
bool postInspectionPayload([[maybe_unused]] CURL* curl, std::string_view backend_base_url, std::string_view json_payload) {
    if (backend_base_url.empty() || json_payload.empty()) {
        return false;
    }

    auto& cli = getPersistentHttpClient(backend_base_url, 800, 1500);
    const httplib::Headers headers = {
        {"X-Stream-Source", "LIVE_ESP32"},
        {"X-Inspection-Source", "LIVE_ESP32"},
        {"X-Inspection-Struct", "IngressInspectionRequest"},
        {"X-Telemetry-Struct", "TelemetryIngressPayload"}
    };

    auto res = cli.Post(
        "/api/v1/inspections",
        headers,
        json_payload.data(),
        json_payload.size(),
        "application/json"
    );
    return (res && res->status >= 200 && res->status < 300);
}

/**
 * @brief Sends a Mock Video inspection JSON payload (MockInspectionRequest) to /api/v1/inspections/mock via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of the backend service.
 * @param json_payload JSON string containing MockInspectionRequest payload.
 * @return true if backend returned HTTP 2xx status.
 */
bool postMockInspectionPayload([[maybe_unused]] CURL* curl, std::string_view backend_base_url, std::string_view json_payload) {
    if (backend_base_url.empty() || json_payload.empty()) {
        return false;
    }

    auto& cli = getPersistentHttpClient(backend_base_url, 800, 1500);
    const httplib::Headers headers = {
        {"X-Stream-Source", "MOCK_VIDEO"},
        {"X-Inspection-Source", "MOCK_VIDEO"},
        {"X-Inspection-Struct", "MockInspectionRequest"},
        {"X-Telemetry-Struct", "MockTelemetryPayload"}
    };

    auto res = cli.Post(
        "/api/v1/inspections/mock",
        headers,
        json_payload.data(),
        json_payload.size(),
        "application/json"
    );
    return (res && res->status >= 200 && res->status < 300);
}

/**
 * @brief Transmits an annotated JPEG frame with visual bounding boxes to /api/v1/camera/annotated via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of the backend service.
 * @param device_id Target device identifier.
 * @param frame_id Monotonic frame sequence ID.
 * @param jpeg_bytes JPEG encoded annotated image bytes.
 * @return true if backend returned HTTP 2xx status.
 */
bool postAnnotatedFrame(
    [[maybe_unused]] CURL* curl,
    std::string_view backend_base_url,
    std::string_view device_id,
    uint32_t frame_id,
    const std::vector<uint8_t>& jpeg_bytes
) {
    if (backend_base_url.empty() || jpeg_bytes.empty()) {
        return false;
    }

    auto& cli = getPersistentHttpClient(backend_base_url, 500, 1000);
    const httplib::Headers headers = {
        {"X-Device-ID", device_id.empty() ? "DEFAULT_CAMERA" : std::string(device_id)},
        {"X-Frame-ID", fmt::format("{}", frame_id)}
    };

    auto res = cli.Post(
        "/api/v1/camera/annotated",
        headers,
        reinterpret_cast<const char*>(jpeg_bytes.data()),
        jpeg_bytes.size(),
        "image/jpeg"
    );
    return (res && res->status >= 200 && res->status < 300);
}

/**
 * @brief Transmits a recorded video container (MP4/H.264 binary) to /api/v1/video via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of backend service.
 * @param device_id Unique identifier of transmitting device.
 * @param video_bytes Binary buffer containing full video file.
 * @param content_type MIME type of the video.
 * @param timeout_ms Upload transfer timeout in milliseconds.
 * @return true if server responds with HTTP 2xx status.
 */
bool postVideoPayload(
    [[maybe_unused]] CURL* curl,
    std::string_view backend_base_url,
    std::string_view device_id,
    const std::vector<uint8_t>& video_bytes,
    std::string_view content_type,
    long timeout_ms
) {
    if (backend_base_url.empty() || video_bytes.empty()) {
        return false;
    }

    auto& cli = getPersistentHttpClient(backend_base_url, 1500, static_cast<int>(timeout_ms > 0 ? timeout_ms : 5000));
    const httplib::Headers headers = {
        {"X-Device-ID", device_id.empty() ? "DEFAULT_CAMERA" : std::string(device_id)}
    };

    auto res = cli.Post(
        "/api/v1/video",
        headers,
        reinterpret_cast<const char*>(video_bytes.data()),
        video_bytes.size(),
        content_type.empty() ? "video/mp4" : std::string(content_type)
    );
    return (res && res->status >= 200 && res->status < 300);
}

/**
 * @brief Sends video manifest metadata in JSON format to /api/v1/video via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of backend service.
 * @param json_payload JSON string containing video metadata information.
 * @param timeout_ms Transmission timeout limit in milliseconds.
 * @return true if server responds with HTTP 2xx status.
 */
bool postVideoMetadata(
    [[maybe_unused]] CURL* curl,
    std::string_view backend_base_url,
    std::string_view json_payload,
    long timeout_ms
) {
    if (backend_base_url.empty() || json_payload.empty()) {
        return false;
    }

    auto& cli = getPersistentHttpClient(backend_base_url, 1000, static_cast<int>(timeout_ms > 0 ? timeout_ms : 1500));
    auto res = cli.Post(
        "/api/v1/video",
        json_payload.data(),
        json_payload.size(),
        "application/json"
    );
    return (res && res->status >= 200 && res->status < 300);
}

/**
 * @brief Queries the backend for dynamic detection confidence threshold via httplib.
 *
 * @param curl Legacy handle pointer.
 * @param backend_base_url Base URL of backend service.
 * @param fallback_val Default fallback threshold.
 * @return float Dynamic threshold value clamped between 0.25f and 0.98f.
 */
float fetchDynamicThreshold(
    [[maybe_unused]] CURL* curl,
    std::string_view backend_base_url,
    float fallback_val
) {
    if (backend_base_url.empty()) {
        return fallback_val;
    }

    auto& cli = getPersistentHttpClient(backend_base_url, 800, 1200);
    auto res = cli.Get("/api/v1/system/threshold");
    if (!res || res->status != 200 || res->body.empty()) {
        return fallback_val;
    }

    const std::string_view json(res->body);
    constexpr std::string_view key = "\"threshold\":";
    const size_t pos = json.find(key);
    if (pos == std::string_view::npos) {
        return fallback_val;
    }

    const size_t val_start = pos + key.size();
    float parsed = fallback_val;
    const char* begin = json.data() + val_start;
    const char* end = json.data() + json.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr == begin) {
        return fallback_val;
    }
    return std::clamp(parsed, 0.25f, 0.98f);
}

} // namespace bawal::server