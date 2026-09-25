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

#include <bawal/vision.hpp>
#include <bawal/server/http_utils.hpp>

using namespace bawal::vision;
using namespace bawal::server;

// Robust MJPEG Frame Decoder with SOI locator and missing Huffman (DHT) table recovery
static cv::Mat decodeMJPEGFrame(const std::vector<uint8_t>& raw_bytes) {
    if (raw_bytes.empty()) return cv::Mat();

    // 1. Locate SOI marker (0xFF, 0xD8)
    size_t soi = 0;
    while (soi + 1 < raw_bytes.size()) {
        if (raw_bytes[soi] == 0xFF && raw_bytes[soi + 1] == 0xD8) {
            break;
        }
        soi++;
    }

    if (soi >= raw_bytes.size()) {
        return cv::Mat();
    }

    const uint8_t* data = raw_bytes.data() + soi;
    const size_t len = raw_bytes.size() - soi;

    // Direct OpenCV libjpeg decode
    cv::Mat buf(1, static_cast<int>(len), CV_8UC1, const_cast<uint8_t*>(data));
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (!img.empty()) {
        return img;
    }

    // 2. Check if DHT (0xFF, 0xC4) is missing and inject standard MJPEG DHT table
    bool has_dht = false;
    for (size_t i = 0; i + 1 < len; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xC4) {
            has_dht = true;
            break;
        }
        if (data[i] == 0xFF && data[i + 1] == 0xDA) { // SOS marker
            break;
        }
    }

    if (!has_dht && len > 2) {
        static const uint8_t dht_table[] = {
            0xFF, 0xC4, 0x01, 0xA2,
            0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
            0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
            0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D,
            0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22,
            0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33,
            0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55,
            0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76,
            0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96,
            0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5,
            0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4,
            0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1,
            0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
            0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77,
            0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
            0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62,
            0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29,
            0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54,
            0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
            0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94,
            0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3,
            0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2,
            0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
            0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA
        };

        std::vector<uint8_t> reconstructed;
        reconstructed.reserve(len + sizeof(dht_table));
        reconstructed.insert(reconstructed.end(), data, data + 2);
        reconstructed.insert(reconstructed.end(), std::begin(dht_table), std::end(dht_table));
        reconstructed.insert(reconstructed.end(), data + 2, data + len);

        cv::Mat rec_buf(1, static_cast<int>(reconstructed.size()), CV_8UC1, reconstructed.data());
        return cv::imdecode(rec_buf, cv::IMREAD_COLOR);
    }

    return cv::Mat();
}

int main(int argc, char** argv) {
    std::cout << "====================================================\n";
    std::cout << "  BawalVision C++ OpenCV Vision Processing Service  \n";
    std::cout << "  Source: ESP32-S3 Logitech USB Stream (via Network)\n";
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

    // 4. Priority 4: Dynamic Fallback Flag (Jika belum ada stream yang aktif)
    if (device_id.empty()) {
        std::cout << "[WARN] Auto-discovery pending (no active broadcast yet). Entering wildcard listener mode.\n";
        device_id = "UNKNOWN_DEVICE";
    }

    std::string stream_url = backend_base_url + "/api/v1/camera/latest/" + device_id + "/raw";
    std::cout << "[INFO] Stream Ingestion URL: " << stream_url << "\n\n";

    ReceiverConfig config;
    config.auto_convert_bgr = true;
    config.prealloc_buffer_size = 256 * 1024;
    CameraReceiver receiver(config);

    if (!headless) {
        const std::string window_name = "BawalVision ESP32 Logitech Camera Stream";
        cv::namedWindow(window_name, cv::WINDOW_AUTOSIZE);

        std::vector<uint8_t> raw_bytes;
        raw_bytes.reserve(256 * 1024);
        uint64_t frame_seq = 0;

        while (true) {
            bool frame_ok = false;
            cv::Mat frame;

            // Jika device_id belum didapat, coba re-discover secara periodik setiap iterasi
            if (device_id == "UNKNOWN_DEVICE") {
                const std::string discovered = discoverActiveDeviceID(curl, backend_base_url);
                if (!discovered.empty()) {
                    device_id = discovered;
                    stream_url = backend_base_url + "/api/v1/camera/latest/" + device_id + "/raw";
                    std::cout << "[SUCCESS] Connected to newly active device: " << device_id << "\n";
                }
            }

            if (device_id != "UNKNOWN_DEVICE" && fetchLatestFrame(curl, stream_url, raw_bytes)) {
                frame = decodeMJPEGFrame(raw_bytes);
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
                const std::string hud = "ESP32-S3 Logitech (" + device_id + ") | Seq: " + std::to_string(frame_seq);
                cv::putText(frame, hud, cv::Point(15, 30),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 2);
            }

            cv::imshow(window_name, frame);

            const int key = cv::waitKey(30);
            if (key == 'q' || key == 'Q' || key == 27) {
                std::cout << "[INFO] User requested stop. Closing stream window...\n";
                break;
            }
        }

        cv::destroyAllWindows();
    }

    if (curl) {
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();

    std::cout << "\n[SUCCESS] Vision service exited cleanly.\n";
    return 0;
}