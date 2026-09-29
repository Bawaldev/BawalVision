#include <bawal/server/http_utils.hpp>
#include <curl/curl.h>
#include <iostream>

namespace bawal::server {

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total_bytes = size * nmemb;
    auto* output_buffer = static_cast<std::vector<uint8_t>*>(userp);
    const auto* byte_data = static_cast<const uint8_t*>(contents);

    output_buffer->insert(output_buffer->end(), byte_data, byte_data + total_bytes);
    return total_bytes;
}

bool fetchLatestFrame(CURL* curl, const std::string& url, std::vector<uint8_t>& buffer) {
    if (!curl || url.empty()) {
        return false;
    }

    buffer.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
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

std::string discoverActiveDeviceID(CURL* curl, const std::string& backend_base_url) {
    if (!curl || backend_base_url.empty()) {
        return "";
    }

    const std::string discovery_url = backend_base_url + "/api/v1/camera/active";
    std::vector<uint8_t> response_buffer;

    if (!fetchLatestFrame(curl, discovery_url, response_buffer)) {
        return "";
    }

    const std::string json_payload(response_buffer.begin(), response_buffer.end());

    const std::string key = "\"device_id\"";
    const size_t key_pos = json_payload.find(key);
    if (key_pos == std::string::npos) {
        return "";
    }

    const size_t colon_pos = json_payload.find(':', key_pos + key.length());
    if (colon_pos == std::string::npos) {
        return "";
    }

    const size_t start_quote = json_payload.find('"', colon_pos + 1);
    if (start_quote == std::string::npos) {
        return "";
    }

    const size_t end_quote = json_payload.find('"', start_quote + 1);
    if (end_quote == std::string::npos) {
        return "";
    }

    return json_payload.substr(start_quote + 1, end_quote - start_quote - 1);
}

bool postInspectionPayload(CURL* curl, const std::string& backend_base_url, const std::string& json_payload) {
    if (!curl || backend_base_url.empty() || json_payload.empty()) {
        return false;
    }

    const std::string endpoint_url = backend_base_url + "/api/v1/inspections";
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.c_str());
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

} // namespace bawal::server