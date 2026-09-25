#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <curl/curl.h>

namespace bawal::server{

struct CameraConfig {
    std::string backend_url;
    std::string device_id;
    int timeout_ms = 400;
};

// Free Functions / Metode Biasa (Tanpa Class, Tanpa Private)
bool fetchLatestFrame(CURL* curl, const std::string& url, std::vector<uint8_t>& buffer);
std::string discoverActiveDeviceID(CURL* curl, const std::string& backend_base_url);
}