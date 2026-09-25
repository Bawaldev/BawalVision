#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <curl/curl.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include "bawal/vision.hpp"

using namespace bawal::vision;

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* mem = static_cast<std::vector<uint8_t>*>(userp);
    mem->insert(mem->end(), static_cast<uint8_t*>(contents), static_cast<uint8_t*>(contents) + total);
    return total;
}

static bool fetchLatestFrame(CURL* curl, const std::string& url, std::vector<uint8_t>& buffer) {
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

// Fungsi Helper untuk melakukan Auto-Discovery ID Device dari Backend
static std::string discoverActiveDeviceID(CURL* curl, const std::string& backend_base_url) {
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

int main(int argc, char** argv) {
    std::cout << "====================================================\n";
    std::cout << "  BawalVision C++ OpenCV Vision Processing Service  \n";
    std::cout << "  Source: ESP32-S3 Logitech USB Stream (via Network)\n";
    std::cout << "====================================================\n\n";

    curl_global_init(CURL_GLOBAL_ALL);
    CURL* curl = curl_easy_init();

    bool headless = false;
    if (const char* env_headless = std::getenv("HEADLESS")) {
        if (std::string(env_headless) == "1") headless = true;
    }

    if (headless) {
        std::cout << "[INFO] Running in HEADLESS mode.\n";
    } else {
        std::cout << "[INFO] Running in LOCAL GUI mode.\n";
    }

    std::string backend_host = "localhost";
    std::string backend_port = "8080";

    if (const char* env_host = std::getenv("BACKEND_HOST")) backend_host = env_host;
    if (const char* env_port = std::getenv("BACKEND_PORT")) backend_port = env_port;

    std::string backend_base_url = "http://" + backend_host + ":" + backend_port;
    std::string device_id = "";

    // -------------------------------------------------------------
    // DYNAMIC RESOLUTION PIPELINE FOR DEVICE ID
    // -------------------------------------------------------------
    
    // 1. Priority 1: Argumen CLI
    if (argc > 1 && std::string(argv[1]).length() > 0) {
        device_id = argv[1];
        std::cout << "[CONFIG] Device ID resolved from CLI Argument: " << device_id << "\n";
    } 
    // 2. Priority 2: Environment Variable
    else if (const char* env_dev = std::getenv("DEVICE_ID")) {
        device_id = env_dev;
        std::cout << "[CONFIG] Device ID resolved from Environment Variable: " << device_id << "\n";
    }

    // 3. Priority 3: Dynamic Discovery via Backend API Endpoint
    if (device_id.empty()) {
        std::cout << "[INFO] No Device ID supplied. Querying backend for active streams...\n";
        device_id = discoverActiveDeviceID(curl, backend_base_url);
        
        if (!device_id.empty()) {
            std::cout << "[SUCCESS] Auto-discovered active ESP32 device: " << device_id << "\n";
        }
    }

    // 4. Priority 4: Dynamic Status Flag (Jika tidak ada stream yang aktif)
    if (device_id.empty()) {
        std::cout << "[WARN] Auto-discovery failed (no active ESP32 broadcast). Entering wildcard listener mode.\n";
        device_id = "UNKNOWN_DEVICE";
    }

    std::string stream_url = backend_base_url + "/api/v1/camera/latest/" + device_id + "/raw";
    std::cout << "[INFO] Stream Ingestion URL: " << stream_url << "\n\n";

    ReceiverConfig config;
    config.auto_convert_bgr = true;
    config.prealloc_buffer_size = 256 * 1024;
    CameraReceiver receiver(config);

    if (!headless) {
        cv::namedWindow("BawalVision ESP32 Logitech Camera Stream", cv::WINDOW_AUTOSIZE);

        std::vector<uint8_t> raw_bytes;
        raw_bytes.reserve(256 * 1024);
        uint64_t frame_seq = 0;

        while (true) {
            bool frame_ok = false;
            cv::Mat frame;

            // Jika device_id belum didapat, coba re-discover secara periodik setiap loop
            if (device_id == "UNKNOWN_DEVICE") {
                std::string discovered = discoverActiveDeviceID(curl, backend_base_url);
                if (!discovered.empty()) {
                    device_id = discovered;
                    stream_url = backend_base_url + "/api/v1/camera/latest/" + device_id + "/raw";
                    std::cout << "[SUCCESS] Connected to newly active device: " << device_id << "\n";
                }
            }

            if (device_id != "UNKNOWN_DEVICE" && fetchLatestFrame(curl, stream_url, raw_bytes)) {
                frame = cv::imdecode(raw_bytes, cv::IMREAD_COLOR);
                if (!frame.empty()) {
                    frame_ok = true;
                    frame_seq++;
                }
            }

            if (!frame_ok) {
                // Generate standby waiting HUD overlay
                frame = cv::Mat::zeros(360, 640, CV_8UC3);
                cv::putText(frame, "BAWAL VISION EDGE AI", cv::Point(160, 140),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);
                cv::putText(frame, "Waiting for ESP32 Logitech stream...", cv::Point(130, 190),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(200, 200, 200), 1);
                cv::putText(frame, "Target Device: " + device_id, cv::Point(130, 220),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(100, 255, 100), 1);
                cv::putText(frame, "URL: " + stream_url, cv::Point(130, 250),
                            cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(180, 180, 180), 1);
            } else {
                std::string hud = "ESP32-S3 Logitech (" + device_id + ") | Seq: " + std::to_string(frame_seq);
                cv::putText(frame, hud, cv::Point(15, 30),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 2);
            }

            cv::imshow("BawalVision ESP32 Logitech Camera Stream", frame);

            int key = cv::waitKey(30);
            if (key == 'q' || key == 'Q' || key == 27) {
                std::cout << "[INFO] User requested stop. Closing stream window...\n";
                break;
            }
        }

        cv::destroyAllWindows();
    }

    if (curl) curl_easy_cleanup(curl);
    curl_global_cleanup();

    std::cout << "\n[SUCCESS] Vision service exited cleanly.\n";
    return 0;
}