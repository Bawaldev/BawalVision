#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <iomanip>

#include <curl/curl.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "bawal/vision.hpp"
#include "bawal/server.hpp"

using namespace bawal::vision;
using namespace bawal::server;

int main(int argc, char** argv) {
    std::cout << "====================================================\n";
    std::cout << "  BawalVision C++ End-to-End Vision Pipeline Engine \n";
    std::cout << "  [Receiver] -> [Dewarp] -> [Segmenter] -> [AI1]    \n";
    std::cout << "====================================================\n\n";

    curl_global_init(CURL_GLOBAL_ALL);
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[ERROR] Failed to initialize libcurl.\n";
        return 1;
    }

    bool headless = false;
    if (const char* env_headless = std::getenv("HEADLESS")) {
        if (std::string(env_headless) == "1") {
            headless = true;
        }
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

    const std::string backend_base_url = "http://" + backend_host + ":" + backend_port;
    std::string device_id = "";

    auto sanitizeID = [](std::string id) -> std::string {
        while (!id.empty() && (id.front() == ' ' || id.front() == '"' || id.front() == '\'' || id.front() == '*' || id.front() == '/')) {
            id.erase(id.begin());
        }
        while (!id.empty() && (id.back() == ' ' || id.back() == '"' || id.back() == '\'' || id.back() == '*' || id.back() == '/')) {
            id.pop_back();
        }
        if (id.find('/') != std::string::npos || id.find('\\') != std::string::npos || id.find('*') != std::string::npos || id == "UNKNOWN_DEVICE") {
            return "";
        }
        return id;
    };

    // 1. Priority 1: CLI Argument
    if (argc > 1 && std::string(argv[1]).length() > 0) {
        device_id = sanitizeID(argv[1]);
        if (!device_id.empty()) {
            std::cout << "[CONFIG] Device ID resolved from CLI Argument: " << device_id << "\n";
        }
    } 
    // 2. Priority 2: Environment Variable
    if (device_id.empty()) {
        if (const char* env_dev = std::getenv("DEVICE_ID")) {
            device_id = sanitizeID(env_dev);
            if (!device_id.empty()) {
                std::cout << "[CONFIG] Device ID resolved from Environment Variable: " << device_id << "\n";
            }
        }
    }

    // 3. Priority 3: Dynamic Discovery via Backend API Endpoint
    if (device_id.empty()) {
        std::cout << "[INFO] No Device ID supplied. Querying backend for active streams...\n";
        device_id = discoverActiveDeviceID(curl, backend_base_url);
        if (!device_id.empty()) {
            std::cout << "[SUCCESS] Auto-discovered active ESP32 device: " << device_id << "\n";
        }
    }

    std::string stream_url = !device_id.empty() 
        ? (backend_base_url + "/api/v1/camera/latest/" + device_id + "/raw")
        : (backend_base_url + "/api/v1/camera/latest/raw");
    std::cout << "[INFO] Stream Ingestion URL: " << stream_url << "\n\n";

    // 4. Initialize Inspection Pipeline Orchestrator
    std::string model_path = "ml/AI1/model/ai1_vision.onnx";
    if (const char* env_model = std::getenv("MODEL_PATH")) {
        model_path = env_model;
    }

    InspectionPipeline pipeline;
    if (!pipeline.initialize(model_path, 640, 480)) {
        std::cerr << "[WARN] Pipeline initialized with fallback configuration.\n";
    }

    const std::string window_name = "BawalVision AI1 Inspection Pipeline";
    if (!headless) {
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name, 800, 600);
    }

    std::vector<uint8_t> raw_bytes;
    raw_bytes.reserve(256 * 1024);
    uint64_t frame_seq = 0;
    int discovery_tick = 0;

    while (true) {
        bool frame_ok = false;
        InspectionOutput inspection;

        // Periodically refresh active device ID and update stream URL
        if (device_id.empty() || (discovery_tick++ % 30 == 0)) {
            const std::string discovered = discoverActiveDeviceID(curl, backend_base_url);
            if (!discovered.empty() && (discovered != device_id || stream_url.find(discovered) == std::string::npos)) {
                device_id = discovered;
                stream_url = backend_base_url + "/api/v1/camera/latest/" + device_id + "/raw";
                std::cout << "[SUCCESS] Active camera stream connected: " << device_id << "\n";
                std::cout << "[INFO] Ingestion URL: " << stream_url << "\n";
            }
        }

        // Attempt 1: Fetch from active stream URL
        if (!fetchLatestFrame(curl, stream_url, raw_bytes) || raw_bytes.empty()) {
            // Attempt 2: Check if device ID updated on backend
            const std::string discovered = discoverActiveDeviceID(curl, backend_base_url);
            if (!discovered.empty() && discovered != device_id) {
                device_id = discovered;
                stream_url = backend_base_url + "/api/v1/camera/latest/" + device_id + "/raw";
                fetchLatestFrame(curl, stream_url, raw_bytes);
            }
            // Attempt 3: Generic latest endpoint
            if (raw_bytes.empty()) {
                std::string fallback_url = backend_base_url + "/api/v1/camera/latest/raw";
                fetchLatestFrame(curl, fallback_url, raw_bytes);
            }
        }

        if (!raw_bytes.empty()) {
            inspection.frame_id = static_cast<uint32_t>(frame_seq + 1);
            
            // Execute End-to-End Pipeline
            // [CameraReceiver] -> [Dewarp] -> [Segmenter] -> [AI1 Classifier]
            if (pipeline.execute(raw_bytes, inspection)) {
                frame_ok = true;
                frame_seq++;
                
                if (frame_seq % 30 == 1) {
                    std::cout << "[PIPELINE] Frame #" << frame_seq 
                              << " (" << std::fixed << std::setprecision(1) << inspection.processing_time_ms << " ms) | Compartments: ";
                    for (const auto& c : inspection.classifications) {
                        std::cout << "[" << c.class_name << ": " << int(c.confidence * 100) << "%] ";
                    }
                    std::cout << "\n";
                }
            }
        }

        if (!headless) {
            cv::Mat display_frame;
            if (!frame_ok || inspection.annotated_image.empty()) {
                // Standby Waiting HUD
                display_frame = cv::Mat::zeros(480, 640, CV_8UC3);
                cv::putText(display_frame, "BAWAL VISION AI1 PIPELINE", cv::Point(145, 180),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
                cv::putText(display_frame, "Waiting for ESP32-S3 Stream...", cv::Point(145, 230),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
                cv::putText(display_frame, "Target Device: " + device_id, cv::Point(145, 265),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(100, 255, 100), 1, cv::LINE_AA);
                cv::putText(display_frame, "URL: " + stream_url, cv::Point(145, 295),
                            cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(180, 180, 180), 1, cv::LINE_AA);
            } else {
                display_frame = inspection.annotated_image;
            }

            cv::imshow(window_name, display_frame);

            const int key = cv::waitKey(30);
            if (key == 'q' || key == 'Q' || key == 27) {
                std::cout << "[INFO] User requested stop. Closing vision window...\n";
                break;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    }

    if (!headless) {
        cv::destroyAllWindows();
    }

    if (curl) {
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();

    std::cout << "\n[SUCCESS] Vision pipeline service exited cleanly.\n";
    return 0;
}