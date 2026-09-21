#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include "bawal/vision.hpp"

using namespace bawal::vision;

int main(int argc, char** argv) {
    std::cout << "====================================================\n";
    std::cout << "  BawalVision C++ OpenCV Vision Processing Service  \n";
    std::cout << "  Source: ESP32-S3 Logitech USB Stream (via Network)\n";
    std::cout << "====================================================\n\n";

    bool headless = false;
    const char* env_headless = std::getenv("HEADLESS");
    if (env_headless && std::string(env_headless) == "1") {
        headless = true;
        std::cout << "[INFO] Running in HEADLESS mode (AWS Lambda / Cloud container).\n";
    } else {
        std::cout << "[INFO] Running in LOCAL GUI mode (OpenCV Window enabled).\n";
    }

    std::string backend_host = "localhost";
    std::string backend_port = "8080";
    std::string device_id = "ESP32-S3-BENCH-01";

    if (const char* env_port = std::getenv("BACKEND_PORT")) {
        backend_port = env_port;
    }
    if (argc > 1) {
        device_id = argv[1];
    }

    std::string stream_url = "http://" + backend_host + ":" + backend_port + "/api/v1/camera/latest/" + device_id + "/raw";
    std::cout << "[INFO] Stream Ingestion URL: " << stream_url << "\n";
    std::cout << "[INFO] Target Edge Device ID: " << device_id << "\n\n";

    ReceiverConfig config;
    config.auto_convert_bgr = true;
    config.prealloc_buffer_size = 256 * 1024;
    CameraReceiver receiver(config);

    if (!headless) {
        std::cout << "[INFO] Opening OpenCV Window: 'BawalVision ESP32 Logitech Camera Stream'...\n";
        std::cout << "[HINT] Press 'q' or 'ESC' in the window to exit.\n\n";
        cv::namedWindow("BawalVision ESP32 Logitech Camera Stream", cv::WINDOW_AUTOSIZE);

        cv::VideoCapture stream_cap;
        uint64_t frame_seq = 0;

        while (true) {
            // Check if stream is accessible or read direct frame
            if (!stream_cap.isOpened()) {
                stream_cap.open(stream_url);
            }

            cv::Mat frame;
            bool frame_ok = false;

            if (stream_cap.isOpened() && stream_cap.read(frame) && !frame.empty()) {
                frame_ok = true;
            } else {
                // Generate informative waiting HUD overlay
                frame = cv::Mat::zeros(360, 640, CV_8UC3);
                cv::putText(frame, "BAWAL VISION EDGE AI", cv::Point(160, 140),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);
                cv::putText(frame, "Waiting for ESP32 Logitech stream...", cv::Point(130, 190),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(200, 200, 200), 1);
                cv::putText(frame, "Device: " + device_id, cv::Point(130, 220),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(100, 255, 100), 1);
                cv::putText(frame, "URL: " + stream_url, cv::Point(130, 250),
                            cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(180, 180, 180), 1);

                stream_cap.release(); // Retry connection
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            frame_seq++;
            if (frame_ok) {
                std::string hud = "ESP32-S3 Logitech | Seq: " + std::to_string(frame_seq);
                cv::putText(frame, hud, cv::Point(15, 30),
                            cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
            }

            cv::imshow("BawalVision ESP32 Logitech Camera Stream", frame);

            int key = cv::waitKey(30);
            if (key == 'q' || key == 'Q' || key == 27) {
                std::cout << "[INFO] User requested stop. Closing stream window...\n";
                break;
            }
        }

        cv::destroyAllWindows();
    } else {
        std::cout << "[INFO] Headless mode: Ingesting frames in background...\n";
    }

    std::cout << "\n[SUCCESS] Vision service exited cleanly.\n";
    return 0;
}