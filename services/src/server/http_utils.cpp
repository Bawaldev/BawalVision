


#include <bawal/server/http_utils.hpp>

namespace bawal::server {

namespace {
// Callback internal libcurl (Anonymous Namespace/Static scope khusus file ini)
size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* mem = static_cast<std::vector<uint8_t>*>(userp);
    auto* bytes = static_cast<const uint8_t*>(contents);
    mem->insert(mem->end(), bytes, bytes + total);
    return total;
}
} // anonymous namespace

bool fetchLatestFrame(CURL* curl, const std::string& url, std::vector<uint8_t>& buffer) {
    if (!curl) return false;

    buffer.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 400L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) return false;

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    return (http_code == 200 && !buffer.empty());
}

std::string discoverActiveDeviceID(CURL* curl, const std::string& backend_base_url) {
    std::string discovery_url = backend_base_url + "/api/v1/camera/active";
    std::vector<uint8_t> response_buffer;

    if (fetchLatestFrame(curl, discovery_url, response_buffer)) {
        std::string json_payload(response_buffer.begin(), response_buffer.end());

        // Parsing manual sederhana untuk "device_id":"VALUE"
        size_t key_pos = json_payload.find("\"device_id\"");
        if (key_pos != std::string::npos) {
            size_t start_quote = json_payload.find("\"", key_pos + 11);
            size_t end_quote = json_payload.find("\"", start_quote + 1);
            if (start_quote != std::string::npos && end_quote != std::string::npos) {
                return json_payload.substr(start_quote + 1, end_quote - start_quote - 1);
            }
        }
    }
    return "";
}

} // namespace bawal::server