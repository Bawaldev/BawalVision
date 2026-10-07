#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <iomanip>
#include <algorithm>
#include <sstream>

#include <curl/curl.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "bawal/vision.hpp"
#include "bawal/server.hpp"

using namespace bawal::vision;
using namespace bawal::server;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

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

    CURL* curl_fetch = curl_easy_init();
    CURL* curl_post = curl_easy_init();

    const std::string window_name = "BawalVision AI1 Inspection Pipeline";
    if (!headless) {
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name, 800, 600);
    }

    std::vector<uint8_t> raw_bytes;
    std::vector<uint8_t> prev_raw_bytes;
    raw_bytes.reserve(256 * 1024);
    prev_raw_bytes.reserve(256 * 1024);
    uint64_t frame_seq = 0;
    int discovery_tick = 0;
    auto last_dispatch_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    constexpr int kIdleBackoffMinMs = 100;
    constexpr int kIdleBackoffMaxMs = 1000;
    int idle_backoff_ms = 0;  // 0 == currently receiving frames

    while (true) {
        bool frame_ok = false;
        InspectionOutput inspection;

        if (device_id.empty() || idle_backoff_ms > 0 || (discovery_tick++ % 30 == 0)) {
            const std::string discovered = discoverActiveDeviceID(curl_fetch, backend_base_url);
            if (!discovered.empty() && (discovered != device_id || stream_url.find(discovered) == std::string::npos)) {
                device_id = discovered;
                stream_url = backend_base_url + "/api/v1/camera/latest/" + device_id + "/raw";
                std::cout << "[SUCCESS] Active camera stream connected: " << device_id << "\n";
                std::cout << "[INFO] Ingestion URL: " << stream_url << "\n";
            }
        }

        bool got_frame = fetchLatestFrame(curl_fetch, stream_url, raw_bytes) && !raw_bytes.empty();
        if (!got_frame && !device_id.empty()) {
            const std::string fallback_url = backend_base_url + "/api/v1/camera/latest/raw";
            got_frame = fetchLatestFrame(curl_fetch, fallback_url, raw_bytes) && !raw_bytes.empty();
        }

        if (got_frame) {
            if (idle_backoff_ms > 0) {
                std::cout << "[INFO] Camera frames resumed.\n";
            }
            idle_backoff_ms = 0;
        } else {
            if (idle_backoff_ms == 0) {
                std::cout << "[INFO] No camera frame available. Backing off until an ESP32 stream appears...\n";
                idle_backoff_ms = kIdleBackoffMinMs;
            } else {
                idle_backoff_ms = std::min(idle_backoff_ms * 2, kIdleBackoffMaxMs);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(idle_backoff_ms));
            raw_bytes.clear();
            continue;
        }

        // Fast Header/Footer Duplicate Check (Avoids 250KB memcmp & eliminates lag)
        if (raw_bytes.size() == prev_raw_bytes.size() && raw_bytes.size() >= 32) {
            if (std::memcmp(raw_bytes.data(), prev_raw_bytes.data(), 16) == 0 &&
                std::memcmp(raw_bytes.data() + raw_bytes.size() - 16, prev_raw_bytes.data() + prev_raw_bytes.size() - 16, 16) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }
        prev_raw_bytes = raw_bytes;

        inspection.frame_id = static_cast<uint32_t>(frame_seq + 1);
        
        // Execute End-to-End Pipeline in parallel AVX2
        // [CameraReceiver] -> [Dewarp] -> [YOLOv8 + SIMD Evaluator]
        if (pipeline.execute(raw_bytes, inspection)) {
            frame_ok = true;
            frame_seq++;

            // Collect valid food detections
            std::vector<DetectionResult> valid_detections;
            for (const auto& d : inspection.detections) {
                if (d.class_id >= 0 && d.confidence >= 0.35f && d.class_name != "no_food" && d.class_name != "no_model") {
                    valid_detections.push_back(d);
                }
            }

            if (frame_seq % 30 == 1) {
                std::cout << "[PIPELINE] Frame #" << frame_seq 
                          << " (" << std::fixed << std::setprecision(1) << inspection.processing_time_ms << " ms) | Detections: ";
                for (const auto& d : inspection.detections) {
                    const float freshness = std::clamp(1.0f - d.spoilage.discoloration_ratio, 0.0f, 1.0f);
                    const char* status_str = d.spoilage.is_visually_spoiled ? "SPOILED" : "FRESH";
                    std::cout << "[" << d.class_name << ": " << int(d.confidence * 100) << "% (" 
                              << status_str << " " << int(freshness * 100) << "%)] ";
                }
                std::cout << "\n";
            }

            // Periodic Inspection Ingress Dispatch to Backend & Database
            const auto current_time = std::chrono::steady_clock::now();
            const auto elapsed_since_dispatch = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_dispatch_time).count();

            if (!valid_detections.empty() && elapsed_since_dispatch >= 800 && !device_id.empty()) {
                std::ostringstream json_ss;
                json_ss << "{"
                        << "\"device_id\":\"" << device_id << "\","
                        << "\"detections\":[";
                for (size_t i = 0; i < valid_detections.size(); ++i) {
                    const auto& vd = valid_detections[i];
                    const float spoilage_pct = std::clamp(vd.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);
                    const std::string status = vd.spoilage.is_visually_spoiled ? "REJECT" : (spoilage_pct > 35.0f ? "REVIEW" : "PASS");

                    if (i > 0) json_ss << ",";
                    json_ss << "{"
                            << "\"food_name\":\"" << vd.class_name << "\","
                            << "\"similarity_rate\":" << std::fixed << std::setprecision(4) << vd.confidence << ","
                            << "\"spoilage_score\":" << std::fixed << std::setprecision(2) << spoilage_pct << ","
                            << "\"status\":\"" << status << "\","
                            << "\"box\":[" << vd.bbox.x << "," << vd.bbox.y << "," << vd.bbox.width << "," << vd.bbox.height << "]"
                            << "}";
                }
                json_ss << "]}";

                const std::string payload_json = json_ss.str();
                if (bawal::server::postInspectionPayload(curl_post, backend_base_url, payload_json)) {
                    last_dispatch_time = current_time;
                    std::cout << "[DISPATCH] Inspection payload saved to PostgreSQL (" 
                              << valid_detections.size() << " detected foods)\n";
                }
            }

            // Push OpenCV Annotated Frame with detection boxes to backend camera stream (Fast Quality 68)
            if (!inspection.annotated_image.empty() && !device_id.empty()) {
                std::vector<uint8_t> annotated_jpeg;
                std::vector<int> encode_params = {cv::IMWRITE_JPEG_QUALITY, 68};
                if (cv::imencode(".jpg", inspection.annotated_image, annotated_jpeg, encode_params)) {
                    [[maybe_unused]] bool pushed = bawal::server::postAnnotatedFrame(curl_post, backend_base_url, device_id, inspection.frame_id, annotated_jpeg);
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

            const int key = cv::waitKey(idle_backoff_ms > 0 ? idle_backoff_ms : 10);
            if (key == 'q' || key == 'Q' || key == 27) {
                std::cout << "[INFO] User requested stop. Closing vision window...\n";
                break;
            }
        } else {
            if (idle_backoff_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(idle_backoff_ms));
            }
        }
    }

    if (!headless) {
        cv::destroyAllWindows();
    }

    if (curl_fetch) curl_easy_cleanup(curl_fetch);
    if (curl_post) curl_easy_cleanup(curl_post);
    if (curl) {
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();

    std::cout << "\n[SUCCESS] Vision pipeline service exited cleanly.\n";
    return 0;
}