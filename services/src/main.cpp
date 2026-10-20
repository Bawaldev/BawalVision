#include <iostream>
#include <fstream>
#include <array>
#include <vector>
#include <string>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <iomanip>
#include <algorithm>

#include <httplib.h>
#include <fmt/core.h>
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

    bool headless = false;
    if (const char* env_headless = std::getenv("HEADLESS")) {
        if (std::string(env_headless) == "1") {
            headless = true;
        }
    }

    if (headless) {
        fmt::print("[INFO] Running in HEADLESS mode.\n");
    } else {
        fmt::print("[INFO] Running in LOCAL GUI mode.\n");
    }

    std::string backend_host = "localhost";
    std::string backend_port = "8080";

    if (const char* env_host = std::getenv("BACKEND_HOST")) backend_host = env_host;
    if (const char* env_port = std::getenv("BACKEND_PORT")) backend_port = env_port;

    const std::string backend_base_url = fmt::format("http://{}:{}", backend_host, backend_port);
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

    // 4. Initialize Inspection Pipeline Orchestrator (Prioritizing YOLOv8s over YOLOv8n)
    std::string model_path = "/app/ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx";
    if (const char* env_model = std::getenv("MODEL_PATH")) {
        if (std::string(env_model).length() > 0) {
            model_path = env_model;
        }
    } else {
        const std::vector<std::string> candidate_model_paths = {
            // Priority 1: YOLOv8s INT8
            "/app/ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
            "ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
            "../ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
            "../../ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
            "../../../ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
            "/app/weights/yolov8s_int8.onnx",
            "/app/weights/yolo8s_int8.onnx",
            "weights/yolov8s_int8.onnx",
            "weights/yolo8s_int8.onnx",
            "../weights/yolov8s_int8.onnx",
            "../../weights/yolov8s_int8.onnx",

            // Priority 2: YOLOv8s FP32
            "/app/ml/AI1/model/yolov8s/ai1_vision.onnx",
            "/app/ml/AI1/model/yolov8s/yolov8s.onnx",
            "ml/AI1/model/yolov8s/ai1_vision.onnx",
            "ml/AI1/model/yolov8s/yolov8s.onnx",
            "../ml/AI1/model/yolov8s/ai1_vision.onnx",
            "../../ml/AI1/model/yolov8s/ai1_vision.onnx",
            "../../../ml/AI1/model/yolov8s/ai1_vision.onnx",
            "/app/weights/yolov8s.onnx",
            "weights/yolov8s.onnx",
            "../weights/yolov8s.onnx",
            "../../weights/yolov8s.onnx",

            // Priority 3: Fallback to YOLOv8n INT8 / Generic
            "/app/ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx",
            "ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx",
            "../ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx",
            "../../ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx",
            "../../../ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx",
            "/app/weights/ai1_vision_int8.onnx",
            "weights/ai1_vision_int8.onnx",
            "../weights/ai1_vision_int8.onnx",
            "../../weights/ai1_vision_int8.onnx",
            "../../../weights/ai1_vision_int8.onnx",

            // Priority 4: Fallback to YOLOv8n FP32
            "/app/ml/AI1/model/yolov8n/ai1_vision.onnx",
            "ml/AI1/model/yolov8n/ai1_vision.onnx",
            "../ml/AI1/model/yolov8n/ai1_vision.onnx",
            "../../ml/AI1/model/yolov8n/ai1_vision.onnx"
        };
        for (const auto& p : candidate_model_paths) {
            std::ifstream check(p.c_str());
            if (check.good()) {
                model_path = p;
                break;
            }
        }
    }

    InspectionPipeline pipeline;
    if (!pipeline.initialize(model_path, 640, 480)) {
        std::cerr << "[WARN] Pipeline initialized with fallback configuration.\n";
    }

    std::cout << "[INFO] Super-Resolution Enhancer: " 
              << (pipeline.getEnhancer().isReady() ? "Active (NCNN Vulkan / Multi-Arch SIMD)" : "Standby") 
              << " | Scale: " << pipeline.getEnhancer().params.scale << "x\n";

    CURL* curl_fetch = curl_easy_init();
    CURL* curl_post = curl_easy_init();

    const std::string window_name = "BawalVision AI1 Inspection Pipeline";
    if (!headless) {
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name, 800, 600);
    }

    bool use_video_source = false;
    if (const char* env_video = std::getenv("VIDEO_PATH")) {
        if (std::string(env_video).length() > 0) {
            if (pipeline.openVideoSource(SourceMode::LOCAL_MP4, env_video)) {
                use_video_source = true;
                if (device_id.empty()) {
                    device_id = "MOCK_VIDEO_CAM";
                    std::cout << "[CONFIG] Assigned default Device ID for local MP4 VideoSource: " << device_id << "\n";
                }
                std::cout << "[INFO] VideoSource initialized with local MP4: " << env_video << "\n";
            }
        }
    }
    if (!use_video_source) {
        (void)pipeline.openVideoSource(SourceMode::ESP32_STREAM, stream_url);
        pipeline.getVideoSource().setStreamUrl(stream_url, backend_base_url + "/api/v1/camera/latest/raw");
    }

    uint64_t frame_seq = 0;
    int discovery_tick = 0;
    auto last_ai_dispatch = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    // Asynchronous Background AI Worker Thread
    std::mutex ai_mutex;
    std::condition_variable ai_cv;
    cv::Mat ai_input_frame;
    bool new_ai_frame_available = false;
    std::atomic<bool> ai_worker_running{true};
    std::atomic<bool> ai_detections_updated{false};
    std::atomic<bool> video_has_rewound{false};
    std::vector<DetectionResult> shared_detections;
    std::string current_device_id = device_id;

    auto last_valid_detection_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    std::thread ai_worker([&]() {
        CURL* curl_db = curl_easy_init();
        auto last_dispatch_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        std::string last_state_signature;

        while (ai_worker_running.load()) {
            cv::Mat frame_to_process;
            std::string dev_id;
            {
                std::unique_lock<std::mutex> lock(ai_mutex);
                ai_cv.wait(lock, [&]() { return new_ai_frame_available || !ai_worker_running.load(); });
                if (!ai_worker_running.load()) break;
                frame_to_process = ai_input_frame;
                dev_id = current_device_id;
                new_ai_frame_available = false;
            }

            if (frame_to_process.empty()) continue;

            const int64_t t0 = cv::getTickCount();
            std::vector<DetectionResult> detected;
            if (pipeline.runInference(frame_to_process, detected)) {
                const double infer_ms = ((cv::getTickCount() - t0) * 1000.0) / cv::getTickFrequency();
                
                // Periodically synchronize dynamic confidence threshold from backend (every ~3 seconds)
                static auto last_thresh_sync = std::chrono::steady_clock::now();
                const auto now_thresh = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now_thresh - last_thresh_sync).count() >= 3) {
                    last_thresh_sync = now_thresh;
                    const float backend_thresh = bawal::server::fetchDynamicThreshold(curl_db, backend_base_url, pipeline.getConfidenceThreshold());
                    if (std::abs(backend_thresh - pipeline.getConfidenceThreshold()) > 0.005f) {
                        std::cout << "[CONFIG] Dynamically updated confidence threshold from backend: " 
                                  << int(backend_thresh * 100) << "%\n";
                        pipeline.setConfidenceThreshold(backend_thresh);
                    }
                }

                const float min_conf_threshold = pipeline.getConfidenceThreshold();
                std::vector<DetectionResult> valid_detections;
                for (const auto& d : detected) {
                    if (d.class_id >= 0 && d.confidence >= min_conf_threshold && d.class_name != "no_food" && d.class_name != "no_model") {
                        valid_detections.push_back(d);
                    }
                }

                bool should_clear_backend = false;
                {
                    std::lock_guard<std::mutex> lock(ai_mutex);
                    if (!valid_detections.empty()) {
                        shared_detections = valid_detections;
                        last_valid_detection_time = std::chrono::steady_clock::now();
                        ai_detections_updated.store(true);
                    } else {
                        // Only clear after 1.5s of no detections to avoid HUD flicker
                        const auto now = std::chrono::steady_clock::now();
                        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_valid_detection_time).count();
                        if (elapsed_ms > 1500 && !shared_detections.empty()) {
                            shared_detections.clear();
                            ai_detections_updated.store(true);
                            should_clear_backend = true;
                        }
                    }
                }

                if (!valid_detections.empty()) {
                    fmt::print("[AI INFERENCE] ({:.1f} ms) | Detections: ", infer_ms);
                    for (const auto& d : valid_detections) {
                        const float freshness = std::clamp(1.0f - d.spoilage.discoloration_ratio, 0.0f, 1.0f);
                        const char* status_str = d.spoilage.is_visually_spoiled ? "SPOILED" : "FRESH";
                        fmt::print("[{}: {}% ({} {}%)] ", d.class_name, int(d.confidence * 100), status_str, int(freshness * 100));
                    }
                    fmt::print("\n");
                }

                // State-Change Hysteresis & Selective Sync Gatekeeper:
                // Only dispatch if the food detection state has changed (new item or status change)
                // or if 15 seconds have passed (periodic heartbeat). Enforces a minimum debounce.
                const auto current_time = std::chrono::steady_clock::now();
                const auto elapsed_since_dispatch = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_dispatch_time).count();

                std::string current_state_sig;
                for (const auto& vd : valid_detections) {
                    const float spoilage_pct = std::clamp(vd.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);
                    const std::string status = (vd.spoilage.is_visually_spoiled || spoilage_pct >= 45.0f)
                                                   ? "REJECT"
                                                   : ((spoilage_pct >= 25.0f || vd.confidence < 0.60f) ? "REVIEW" : "PASS");
                    current_state_sig += fmt::format("{}:{};", vd.class_name, status);
                }

                const bool is_mock_source = (dev_id == "MOCK_VIDEO_CAM" || dev_id.rfind("MOCK_", 0) == 0 || use_video_source);
                const bool state_changed = (current_state_sig != last_state_signature);
                const bool heartbeat_sync = is_mock_source ? (elapsed_since_dispatch >= 2500) : (elapsed_since_dispatch >= 15000);
                const bool debounce_ok = (elapsed_since_dispatch >= 800);

                // Shared YOLO object detection + OpenCV optical/spoilage JSON serializer formatted with fmt::format
                auto serializeVisionDetectionsJson = [&](std::string& out_body, const std::vector<DetectionResult>& dets) {
                    const auto optical = pipeline.getClassifier().getLastOpticalMetrics();
                    out_body += fmt::format(
                        R"("scene_optical":{{"avg_r":{:.1f},"avg_g":{:.1f},"avg_b":{:.1f},"lux_luminance":{:.1f},"color_temperature":{},"white_balance_status":"{}"}},"detections":[)",
                        optical.avg_r,
                        optical.avg_g,
                        optical.avg_b,
                        optical.lux_luminance,
                        optical.color_temperature,
                        optical.white_balance_status
                    );
                    for (size_t i = 0; i < dets.size(); ++i) {
                        const auto& vd = dets[i];
                        const float spoilage_pct = std::clamp(vd.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);
                        const std::string status = (vd.spoilage.is_visually_spoiled || spoilage_pct >= 45.0f)
                                                       ? "REJECT"
                                                       : ((spoilage_pct >= 25.0f || vd.confidence < 0.60f) ? "REVIEW" : "PASS");
                        if (i > 0) {
                            out_body.push_back(',');
                        }
                        out_body += fmt::format(
                            R"({{"food_name":"{}","similarity_rate":{:.4f},"spoilage_score":{:.2f},"status":"{}","box":[{},{},{},{}]}})",
                            vd.class_name,
                            vd.confidence,
                            spoilage_pct,
                            status,
                            vd.bbox.x,
                            vd.bbox.y,
                            vd.bbox.width,
                            vd.bbox.height
                        );
                    }
                    out_body.push_back(']');
                    return optical;
                };

                if (!valid_detections.empty() && (state_changed || heartbeat_sync) && debounce_ok && !dev_id.empty()) {
                    const bool is_rewound = video_has_rewound.load();
                    const std::string engine_pipeline = fmt::format("{} + OpenCV HSV", pipeline.getModelArchitecture());
                    std::string payload_json = is_mock_source
                        ? fmt::format(
                              R"({{"mock_id":"{}","device_id":"{}","source_type":"mock_video","inspection_struct":"MockInspectionRequest","engine_pipeline":"{}","is_rewind":{},)",
                              dev_id,
                              dev_id,
                              engine_pipeline,
                              is_rewound ? "true" : "false"
                          )
                        : fmt::format(
                              R"({{"device_id":"{}","source_type":"live_esp32","is_rewind":{},)",
                              dev_id,
                              is_rewound ? "true" : "false"
                          );

                    const auto optical = serializeVisionDetectionsJson(payload_json, valid_detections);
                    payload_json.push_back('}');

                    if (is_mock_source) {
                        const std::string mock_telemetry_json = fmt::format(
                            R"({{"mock_id":"{}","device_id":"{}","source_type":"mock_video","telemetry_struct":"MockTelemetryPayload","connection_mode":"mock_simulation","engine_pipeline":"{}","is_rewind":{},"scene_optical":{{"avg_r":{:.1f},"avg_g":{:.1f},"avg_b":{:.1f},"lux_luminance":{:.1f},"color_temperature":{},"white_balance_status":"{}"}}}})",
                            dev_id,
                            dev_id,
                            engine_pipeline,
                            is_rewound ? "true" : "false",
                            optical.avg_r,
                            optical.avg_g,
                            optical.avg_b,
                            optical.lux_luminance,
                            optical.color_temperature,
                            optical.white_balance_status
                        );
                        (void)bawal::server::postMockTelemetryPayload(backend_base_url, mock_telemetry_json);
                    }

                    const bool dispatched = is_mock_source
                        ? bawal::server::postMockInspectionPayload(curl_db, backend_base_url, payload_json)
                        : bawal::server::postInspectionPayload(curl_db, backend_base_url, payload_json);
                    if (dispatched) {
                        last_dispatch_time = current_time;
                        last_state_signature = current_state_sig;
                        fmt::print(
                            "[DISPATCH] {} saved to PostgreSQL ({} detected foods | Optical: {} lx, {} | {})\n",
                            is_mock_source ? "MockInspectionRequest" : "Live IngressInspectionRequest",
                            valid_detections.size(),
                            int(optical.lux_luminance),
                            optical.white_balance_status,
                            state_changed ? "STATE CHANGE" : "HEARTBEAT"
                        );
                    }
                } else if (should_clear_backend && !dev_id.empty()) {
                    std::string clear_json = is_mock_source
                        ? fmt::format(
                              R"({{"mock_id":"{}","device_id":"{}","source_type":"mock_video","inspection_struct":"MockInspectionRequest",)",
                              dev_id,
                              dev_id
                          )
                        : fmt::format(
                              R"({{"device_id":"{}","source_type":"live_esp32",)",
                              dev_id
                          );
                    const std::vector<DetectionResult> empty_dets;
                    (void)serializeVisionDetectionsJson(clear_json, empty_dets);
                    clear_json.push_back('}');
                    const bool cleared = is_mock_source
                        ? bawal::server::postMockInspectionPayload(curl_db, backend_base_url, clear_json)
                        : bawal::server::postInspectionPayload(curl_db, backend_base_url, clear_json);
                    if (cleared) {
                        last_dispatch_time = current_time;
                        last_state_signature.clear();
                    }
                }
            }
        }
        if (curl_db) curl_easy_cleanup(curl_db);
    });

    constexpr int kIdleBackoffMinMs = 50;
    constexpr int kIdleBackoffMaxMs = 1000;
    int idle_backoff_ms = 0;  

    while (true) {
        const auto loop_start_time = std::chrono::steady_clock::now();
        bool frame_ok = false;
        InspectionOutput inspection;

        cv::Mat frame_to_stream;
        auto& video_source = pipeline.getVideoSource();

        if (use_video_source) {
            if (!video_source.grabNextFrame(frame_to_stream) || frame_to_stream.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            if (video_source.didRewind()) {
                pipeline.resetTracker();
                video_has_rewound.store(true);
                fmt::print("[INFO] Video rewind detected: duplicate inspection writes suppressed.\n");
            }
            inspection.frame_id = static_cast<uint32_t>(frame_seq + 1);
        } else {
            if (device_id.empty() || idle_backoff_ms > 0 || (discovery_tick++ % 15 == 0)) {
                const std::string discovered = discoverActiveDeviceID(curl_fetch, backend_base_url);
                if (!discovered.empty() && discovered != "\\" && discovered != "/" && (discovered != device_id || stream_url.find(discovered) == std::string::npos)) {
                    const bool device_changed = (discovered != device_id);
                    const std::string prev_device_id = device_id;
                    device_id = discovered;
                    {
                        std::lock_guard<std::mutex> lock(ai_mutex);
                        current_device_id = device_id;
                        if (device_changed) {
                            shared_detections.clear();
                        }
                    }
                    if (device_changed) {
                        pipeline.resetTracker();
                        if (!prev_device_id.empty()) {
                            const bool prev_is_mock = (prev_device_id == "MOCK_VIDEO_CAM" || prev_device_id.rfind("MOCK_", 0) == 0);
                            const std::string clear_prev = prev_is_mock
                                ? fmt::format(R"({{"mock_id":"{}","device_id":"{}","detections":[]}})", prev_device_id, prev_device_id)
                                : fmt::format(R"({{"device_id":"{}","detections":[]}})", prev_device_id);
                            if (prev_is_mock) {
                                (void)bawal::server::postMockInspectionPayload(curl_post, backend_base_url, clear_prev);
                            } else {
                                (void)bawal::server::postInspectionPayload(curl_post, backend_base_url, clear_prev);
                            }
                        }
                        const bool new_is_mock = (device_id == "MOCK_VIDEO_CAM" || device_id.rfind("MOCK_", 0) == 0);
                        const std::string clear_new = new_is_mock
                            ? fmt::format(R"({{"mock_id":"{}","device_id":"{}","detections":[]}})", device_id, device_id)
                            : fmt::format(R"({{"device_id":"{}","detections":[]}})", device_id);
                        if (new_is_mock) {
                            (void)bawal::server::postMockInspectionPayload(curl_post, backend_base_url, clear_new);
                        } else {
                            (void)bawal::server::postInspectionPayload(curl_post, backend_base_url, clear_new);
                        }
                    }
                    stream_url = fmt::format("{}/api/v1/camera/latest/{}/raw", backend_base_url, device_id);
                    video_source.setStreamUrl(stream_url, fmt::format("{}/api/v1/camera/latest/raw", backend_base_url));
                    fmt::print("[SUCCESS] Active camera stream connected: {}\n", device_id);
                    fmt::print("[INFO] Ingestion URL: {}\n", stream_url);
                } else if (discovered.empty() && idle_backoff_ms > 200 && !device_id.empty()) {
                    device_id = "";
                    stream_url = fmt::format("{}/api/v1/camera/latest/raw", backend_base_url);
                    video_source.setStreamUrl(stream_url, stream_url);
                }
            }

            if (video_source.grabNextFrame(frame_to_stream) && !frame_to_stream.empty()) {
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
                continue;
            }

            // Avoid re-processing and flood-broadcasting duplicate frames unless async AI detections updated
            const bool detections_changed = ai_detections_updated.exchange(false);
            if (video_source.is_duplicate_frame && !detections_changed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            inspection.frame_id = static_cast<uint32_t>(frame_seq + 1);
        }

        // Get currently active detections
        std::vector<DetectionResult> current_detections;
        {
            std::lock_guard<std::mutex> lock(ai_mutex);
            current_detections = shared_detections;
        }

        // High-Speed Real-Time Video Streaming Pass (Dewarp + Fast ISP + Overlay, < 6 ms)
        const bool is_mock_video = use_video_source || (device_id.find("MOCK") != std::string::npos);
        if (pipeline.processStream(frame_to_stream, current_detections, inspection, is_mock_video)) {
            frame_ok = true;
            frame_seq++;

            // Dispatch frame to background AI worker with throttling (~6 inferences/sec)
            const auto now_ai = std::chrono::steady_clock::now();
            const auto ms_since_ai = std::chrono::duration_cast<std::chrono::milliseconds>(now_ai - last_ai_dispatch).count();
            if (ms_since_ai >= 160) {
                std::unique_lock<std::mutex> lock(ai_mutex, std::try_to_lock);
                if (lock.owns_lock() && !new_ai_frame_available) {
                    ai_input_frame = inspection.dewarped_image.clone();
                    new_ai_frame_available = true;
                    last_ai_dispatch = now_ai;
                    ai_cv.notify_one();
                }
            }

            if (frame_seq % 60 == 1) {
                std::cout << "[STREAM] 30 FPS OK | Frame #" << frame_seq 
                          << " (" << std::fixed << std::setprecision(1) << inspection.processing_time_ms << " ms) | Live Boxes: "
                          << current_detections.size() << "\n";
            }

            // Push OpenCV Annotated Frame to backend camera stream (Crisp High Quality 92)
            if (!inspection.annotated_image.empty() && !device_id.empty()) {
                std::vector<uint8_t> annotated_jpeg;
                std::vector<int> encode_params = {cv::IMWRITE_JPEG_QUALITY, 92};
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
            } else {
                // Rate pacing in headless mode: target ~30 FPS to prevent busy-spinning and saturating backend network I/O
                const auto loop_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - loop_start_time).count();
                if (loop_elapsed_ms < 30) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30 - loop_elapsed_ms));
                }
            }
        }
    }

    ai_worker_running.store(false);
    ai_cv.notify_all();
    if (ai_worker.joinable()) {
        ai_worker.join();
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