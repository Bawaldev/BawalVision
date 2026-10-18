#include <bawal/server/http_utils.hpp>
#include <curl/curl.h>
#include <algorithm>
#include <iostream>

namespace bawal::server {

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total_bytes = size * nmemb;
    auto* output_buffer = static_cast<std::vector<uint8_t>*>(userp);
    const auto* byte_data = static_cast<const uint8_t*>(contents);

    output_buffer->insert(output_buffer->end(), byte_data, byte_data + total_bytes);
    return total_bytes;
}

bool fetchLatestFrame(CURL* curl, std::string_view url, std::vector<uint8_t>& buffer) {
    if (!curl || url.empty()) {
        return false;
    }

    const std::string url_str(url);
    buffer.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        return false;
    }

    long http_status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status_code);
    return (http_status_code == 200 && !buffer.empty());
}

std::string discoverActiveDeviceID(CURL* curl, std::string_view backend_base_url) {
    if (!curl || backend_base_url.empty()) {
        return "";
    }

    const std::string discovery_url = std::string(backend_base_url) + "/api/v1/camera/active";
    std::vector<uint8_t> response_buffer;

    if (!fetchLatestFrame(curl, discovery_url, response_buffer)) {
        return "";
    }

    const std::string_view json_payload(reinterpret_cast<const char*>(response_buffer.data()), response_buffer.size());

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

bool postInspectionPayload(CURL* curl, std::string_view backend_base_url, std::string_view json_payload) {
    if (!curl || backend_base_url.empty() || json_payload.empty()) {
        return false;
    }

    const std::string endpoint_url = std::string(backend_base_url) + "/api/v1/inspections";
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_payload.length()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    std::vector<uint8_t> response_buffer;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);

    const CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_POST, 0L);

    if (res != CURLE_OK) {
        return false;
    }

    long http_status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status_code);
    return (http_status_code >= 200 && http_status_code < 300);
}

bool postAnnotatedFrame(CURL* curl, std::string_view backend_base_url, std::string_view device_id, uint32_t frame_id, const std::vector<uint8_t>& jpeg_bytes) {
    if (!curl || backend_base_url.empty() || jpeg_bytes.empty()) {
        return false;
    }

    const std::string endpoint_url = std::string(backend_base_url) + "/api/v1/camera/annotated";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: image/jpeg");
    const std::string dev_header = "X-Device-ID: " + std::string(device_id.empty() ? "DEFAULT_CAMERA" : device_id);
    const std::string frame_header = "X-Frame-ID: " + std::to_string(frame_id);
    headers = curl_slist_append(headers, dev_header.c_str());
    headers = curl_slist_append(headers, frame_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jpeg_bytes.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jpeg_bytes.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    std::vector<uint8_t> response_buffer;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);

    const CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_POST, 0L);

    if (res != CURLE_OK) {
        return false;
    }

    long http_status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status_code);
    return (http_status_code >= 200 && http_status_code < 300);
}

bool postVideoPayload(
    CURL* curl,
    std::string_view backend_base_url,
    std::string_view device_id,
    const std::vector<uint8_t>& video_bytes,
    std::string_view content_type,
    long timeout_ms
) {
    if (!curl || backend_base_url.empty() || video_bytes.empty()) {
        return false;
    }

    const std::string endpoint_url = std::string(backend_base_url) + "/api/v1/video";

    struct curl_slist* headers = nullptr;
    const std::string ct_header = "Content-Type: " + std::string(content_type.empty() ? "video/mp4" : content_type);
    const std::string dev_header = "X-Device-ID: " + std::string(device_id.empty() ? "DEFAULT_CAMERA" : device_id);
    headers = curl_slist_append(headers, ct_header.c_str());
    headers = curl_slist_append(headers, dev_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, video_bytes.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(video_bytes.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 5000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    std::vector<uint8_t> response_buffer;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);

    const CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_POST, 0L);

    if (res != CURLE_OK) {
        return false;
    }

    long http_status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status_code);
    return (http_status_code >= 200 && http_status_code < 300);
}

bool postVideoMetadata(
    CURL* curl,
    std::string_view backend_base_url,
    std::string_view json_payload,
    long timeout_ms
) {
    if (!curl || backend_base_url.empty() || json_payload.empty()) {
        return false;
    }

    const std::string endpoint_url = std::string(backend_base_url) + "/api/v1/video";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_payload.length()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 1500L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    std::vector<uint8_t> response_buffer;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);

    const CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_POST, 0L);

    if (res != CURLE_OK) {
        return false;
    }

    long http_status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status_code);
    return (http_status_code >= 200 && http_status_code < 300);
}

float fetchDynamicThreshold(
    CURL* curl,
    std::string_view backend_base_url,
    float fallback_val
) {
    if (!curl || backend_base_url.empty()) {
        return fallback_val;
    }

    const std::string endpoint_url = std::string(backend_base_url) + "/api/v1/system/threshold";
    std::vector<uint8_t> response_buffer;

    if (!fetchLatestFrame(curl, endpoint_url, response_buffer)) {
        return fallback_val;
    }

    const std::string_view json(reinterpret_cast<const char*>(response_buffer.data()), response_buffer.size());
    constexpr std::string_view key = "\"threshold\":";
    const size_t pos = json.find(key);
    if (pos == std::string_view::npos) {
        return fallback_val;
    }

    const size_t val_start = pos + key.size();
    try {
        const float parsed = std::stof(std::string(json.substr(val_start, 10)));
        return std::clamp(parsed, 0.25f, 0.98f);
    } catch (...) {
        return fallback_val;
    }
}

} // namespace bawal::server