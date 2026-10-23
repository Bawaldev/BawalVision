#ifdef BUILD_AWS_LAMBDA

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <aws/lambda-runtime/runtime.h>
#include <fmt/core.h>
#include <glaze/glaze.hpp>
#include <openssl/evp.h>

#include "bawal/server/inspection_dto.hpp"
#include "bawal/vision/pipeline.hpp"

namespace {

static bawal::vision::InspectionPipeline g_cloud_pipeline;

std::vector<uint8_t> decodeBase64(std::string_view encoded) {
    if (encoded.empty() || encoded.size() % 4 != 0 ||
        encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    std::vector<uint8_t> decoded((encoded.size() / 4) * 3);
    const int written = EVP_DecodeBlock(
        decoded.data(), reinterpret_cast<const unsigned char*>(encoded.data()),
        static_cast<int>(encoded.size()));
    if (written < 0) {
        return {};
    }

    size_t padding = 0;
    if (!encoded.empty() && encoded.back() == '=') ++padding;
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') ++padding;
    decoded.resize(static_cast<size_t>(written) - padding);
    return decoded;
}

aws::lambda_runtime::invocation_response lambda_handler(
    aws::lambda_runtime::invocation_request const& request) {
    try {
        bawal::server::LambdaEvent event;
        if (const auto error = glz::read_json(event, request.payload); error) {
            return aws::lambda_runtime::invocation_response::failure(
                "Malformed JSON event", "InvalidPayload");
        }

        std::vector<uint8_t> image_bytes;
        if (!event.image_base64.empty()) {
            image_bytes = decodeBase64(event.image_base64);
        } else if (event.is_base64_encoded) {
            image_bytes = decodeBase64(event.body);
        } else {
            image_bytes.assign(event.body.begin(), event.body.end());
        }
        if (image_bytes.empty()) {
            return aws::lambda_runtime::invocation_response::failure(
                "Event did not contain a usable image payload", "InvalidPayload");
        }

        bawal::vision::InspectionOutput output;
        if (!g_cloud_pipeline.execute(image_bytes, output)) {
            return aws::lambda_runtime::invocation_response::failure(
                "Vision pipeline could not process the image", "ExecutionError");
        }

        bawal::server::InspectionResponse dto;
        dto.frame_id = event.frame_id;
        dto.status = "PROCESSED";
        dto.image_path.clear();
        // The current model does not estimate food surface temperature.
        dto.surface_temp = 0.0;
        dto.detections.reserve(output.detections.size());
        for (const auto& detection : output.detections) {
            dto.detections.push_back({
                std::string(detection.class_name), detection.confidence,
                {detection.bbox.x, detection.bbox.y,
                 detection.bbox.width, detection.bbox.height}});
        }

        std::string body;
        if (const auto error = glz::write_json(dto, body); error) {
            return aws::lambda_runtime::invocation_response::failure(
                "Could not serialize inspection result", "SerializationError");
        }
        std::string response_payload;
        const bawal::server::LambdaProxyResponse proxy_response{200, {}, std::move(body)};
        if (const auto error = glz::write_json(proxy_response, response_payload); error) {
            return aws::lambda_runtime::invocation_response::failure(
                "Could not serialize Lambda response", "SerializationError");
        }
        return aws::lambda_runtime::invocation_response::success(
            response_payload, "application/json");
    } catch (const std::exception& error) {
        return aws::lambda_runtime::invocation_response::failure(
            error.what(), "ExecutionError");
    }
}

} // namespace

int main() {
    const char* model_path = std::getenv("MODEL_PATH");
    if (model_path == nullptr || *model_path == '\0') {
        model_path = "/app/models/ai1_vision_int8.onnx";
    }
    if (!g_cloud_pipeline.initialize(model_path, 640, 480)) {
        fmt::print(stderr, "[FATAL] Failed to initialize Lambda vision pipeline\n");
        return 1;
    }
    return aws::lambda_runtime::run_handler(lambda_handler);
}

#else

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
#include "bawal/vision/frame_grabber.hpp"
#include "bawal/server.hpp"
#include "bawal/server/async_frame_publisher.hpp"
#include "bawal/server/inspection_dto.hpp"
#include "bawal/ipc.hpp"
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
    bool device_id_pinned = false;

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
            device_id_pinned = true;
            std::cout << "[CONFIG] Device ID resolved from CLI Argument: " << device_id << "\n";
        }
    } 
    // 2. Priority 2: Environment Variable
    if (device_id.empty()) {
        if (const char* env_dev = std::getenv("DEVICE_ID")) {
            device_id = sanitizeID(env_dev);
            if (!device_id.empty()) {
                device_id_pinned = true;
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

    // 4. Inisialisasi Model AI Inspection Pipeline
    std::string model_path = "/app/ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx";
    if (const char* env_model = std::getenv("MODEL_PATH")) {
        if (std::string(env_model).length() > 0) {
            model_path = env_model;
        }
    } else {
        const std::vector<std::string> candidate_model_paths = {
            "/app/ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
            "ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
            "../ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
            "../../ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
            "/app/weights/yolov8s_int8.onnx",
            "weights/yolov8s_int8.onnx",
            "/app/ml/AI1/model/yolov8s/ai1_vision.onnx",
            "ml/AI1/model/yolov8s/ai1_vision.onnx",
            "/app/ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx",
            "ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx"
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

    const std::string window_name = "BawalVision AI1 Inspection Pipeline";
    if (!headless) {
        cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name, 800, 600);
    }

    // 5. Inisialisasi Ingestion (Mock MP4 vs Zero-Copy POSIX SHM IPC)
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

    bool use_shm = false;
    bawal::ipc::ShmFrameConsumer shm_consumer;
    LatestMatMailbox shm_mailbox(2);
    std::atomic<bool> shm_worker_running{false};
    std::atomic<uint32_t> shm_latest_frame_id{0};
    std::thread shm_worker;

    // Fallback HTTP FrameGrabber (hanya aktif jika mode video lokal atau jika SHM gagal)
    LatestFrameGrabber frame_grabber(pipeline.getVideoSource());

    if (use_video_source) {
        frame_grabber.start();
    } else {
        // Coba inisialisasi Consumer SHM modular
        if (shm_consumer.init()) {
            use_shm = true;
            shm_worker_running.store(true, std::memory_order_release);

            // Ingestion worker asinkron: Menunggu sinyal frame SHM tanpa menyandera GUI main loop
            shm_worker = std::thread([&]() {
                cv::Mat decoded;
                uint32_t fid = 0;
                while (shm_worker_running.load(std::memory_order_acquire)) {
                    try {
                        if (shm_consumer.waitForNextFrame(decoded, fid)) {
                            if (!decoded.empty()) {
                                shm_latest_frame_id.store(fid, std::memory_order_relaxed);
                                shm_mailbox.publish(decoded);
                            }
                        } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                    } catch (const std::exception& e) {
                        // A corrupt/torn SHM frame (or allocation failure) must
                        // not take down the whole vision service.
                        fmt::print(stderr, "[SHM] frame decode failed: {}\n", e.what());
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    } catch (...) {
                        fmt::print(stderr, "[SHM] frame decode failed with unknown error\n");
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            });
            fmt::print("[IPC] POSIX Shared Memory zero-copy consumer aktif!\n");
        } else {
            fmt::print("[WARN] Gagal mengaktifkan SHM. Fallback otomatis ke HTTP frame grabber.\n");
            (void)pipeline.openVideoSource(SourceMode::ESP32_STREAM, stream_url);
            pipeline.getVideoSource().setStreamUrl(stream_url, backend_base_url + "/api/v1/camera/latest/raw");
            frame_grabber.setStreamUrl(stream_url, backend_base_url + "/api/v1/camera/latest/raw");
            frame_grabber.start();
        }
    }

    uint64_t frame_seq = 0;
    auto last_ai_dispatch = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    // AI Worker Asinkron (Single-Slot Mailbox Decoupled)
    std::mutex ai_mutex;
    LatestMatMailbox ai_mailbox(3);
    std::atomic<bool> ai_worker_running{true};
    std::atomic<bool> ai_detections_updated{false};
    std::atomic<bool> video_has_rewound{false};
    std::vector<DetectionResult> shared_detections;
    std::string current_device_id = device_id;

    std::mutex discovery_mutex;
    std::string discovered_device_id;
    bool discovery_has_update = false;
    std::atomic<bool> discovery_running{true};

    auto last_valid_detection_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    std::thread discovery_worker([&]() {
        if (use_video_source) return;
        while (discovery_running.load(std::memory_order_acquire)) {
            const std::string id = bawal::server::discoverActiveDeviceID(nullptr, backend_base_url);
            {
                std::lock_guard<std::mutex> lock(discovery_mutex);
                if (id != discovered_device_id) {
                    discovered_device_id = id;
                    discovery_has_update = true;
                }
            }
            for (int i = 0; i < 40 && discovery_running.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });

    std::thread ai_worker([&]() {
        CURL* curl_db = curl_easy_init();
        auto last_dispatch_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        std::string last_state_signature;
        std::vector<DetectionResult> detected;
        std::vector<DetectionResult> valid_detections;
        auto last_thresh_sync = std::chrono::steady_clock::now();

        while (ai_worker_running.load(std::memory_order_acquire)) {
            cv::Mat frame_to_process;
            std::string dev_id;
            {
                std::lock_guard<std::mutex> lock(ai_mutex);
                dev_id = current_device_id;
            }

            if (!ai_mailbox.waitTake(frame_to_process, std::chrono::milliseconds(100))) {
                continue;
            }

            if (frame_to_process.empty()) continue;

            const int64_t t0 = cv::getTickCount();
            detected.clear();
            if (pipeline.runInference(frame_to_process, detected)) {
                const double infer_ms = ((cv::getTickCount() - t0) * 1000.0) / cv::getTickFrequency();
                
                // Sinkronisasi Confidence Threshold dinamis dari backend tiap 3 detik
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
                valid_detections.clear();
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

                auto serializeDto = [](const auto& value) {
                    std::string json;
                    if (const auto error = glz::write_json(value, json); error) {
                        fmt::print(stderr, "[JSON] Glaze serialization failed\n");
                        return std::string{};
                    }
                    return json;
                };

                auto buildSceneOpticalDto = [](const SceneOpticalMetrics& optical) {
                    return bawal::server::SceneOpticalDto{
                        optical.avg_r, optical.avg_g, optical.avg_b,
                        optical.lux_luminance, optical.color_temperature,
                        optical.white_balance_status};
                };

                auto buildDetectionDtos = [](const std::vector<DetectionResult>& dets) {
                    std::vector<bawal::server::InspectionDetectionDto> result;
                    result.reserve(dets.size());
                    for (const auto& detection : dets) {
                        const float spoilage_pct = std::clamp(
                            detection.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);
                        const char* status = (detection.spoilage.is_visually_spoiled ||
                                              spoilage_pct >= 45.0f)
                            ? "REJECT"
                            : ((spoilage_pct >= 25.0f || detection.confidence < 0.60f)
                                   ? "REVIEW" : "PASS");
                        result.push_back({
                            std::string(detection.class_name), detection.confidence,
                            spoilage_pct, status,
                            {detection.bbox.x, detection.bbox.y,
                             detection.bbox.width, detection.bbox.height}});
                    }
                    return result;
                };

                auto serializeVisionDetectionsJson = [&](const std::vector<DetectionResult>& dets) {
                    const auto optical = pipeline.getClassifier().getLastOpticalMetrics();
                    return optical;
                };

                if (!valid_detections.empty() && (state_changed || heartbeat_sync) && debounce_ok && !dev_id.empty()) {
                    const bool is_rewound = video_has_rewound.exchange(false, std::memory_order_relaxed);
                    const std::string engine_pipeline = fmt::format("{} + OpenCV HSV", pipeline.getModelArchitecture());
                    const auto optical = serializeVisionDetectionsJson(valid_detections);
                    const auto scene_optical = buildSceneOpticalDto(optical);
                    const auto detections = buildDetectionDtos(valid_detections);
                    std::string payload_json = is_mock_source
                        ? serializeDto(bawal::server::MockInspectionPayload{
                              dev_id, dev_id, "mock_video", "MockInspectionRequest",
                              engine_pipeline, is_rewound, scene_optical, detections})
                        : serializeDto(bawal::server::LiveInspectionPayload{
                              dev_id, "live_esp32", is_rewound, scene_optical, detections});

                    if (is_mock_source) {
                        const std::string mock_telemetry_json = serializeDto(
                            bawal::server::MockTelemetryPayload{
                                dev_id, dev_id, "mock_video", "MockTelemetryPayload",
                                "mock_simulation", engine_pipeline, is_rewound, scene_optical});
                        (void)bawal::server::postMockTelemetryPayload(backend_base_url, mock_telemetry_json);
                    }

                    const bool dispatched = !payload_json.empty() && (is_mock_source
                        ? bawal::server::postMockInspectionPayload(curl_db, backend_base_url, payload_json)
                        : bawal::server::postInspectionPayload(curl_db, backend_base_url, payload_json));
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
                    const std::vector<DetectionResult> empty_dets;
                    const auto optical = serializeVisionDetectionsJson(empty_dets);
                    const auto scene_optical = buildSceneOpticalDto(optical);
                    std::string clear_json = is_mock_source
                        ? serializeDto(bawal::server::MockInspectionPayload{
                              dev_id, dev_id, "mock_video", "MockInspectionRequest",
                              fmt::format("{} + OpenCV HSV", pipeline.getModelArchitecture()),
                              false, scene_optical, {}})
                        : serializeDto(bawal::server::LiveInspectionPayload{
                              dev_id, "live_esp32", false, scene_optical, {}});
                    const bool cleared = !clear_json.empty() && (is_mock_source
                        ? bawal::server::postMockInspectionPayload(curl_db, backend_base_url, clear_json)
                        : bawal::server::postInspectionPayload(curl_db, backend_base_url, clear_json));
                    if (cleared) {
                        last_dispatch_time = current_time;
                        last_state_signature.clear();
                    }
                }
            }

            ai_mailbox.recycle(std::move(frame_to_process));
        }
        if (curl_db) curl_easy_cleanup(curl_db);
    });

    AsyncAnnotatedFramePublisher frame_publisher(backend_base_url);
    frame_publisher.start();

    auto postDeviceClearAsync = [backend_base_url](std::string id) {
        if (id.empty()) return;
        std::thread([backend_base_url, id = std::move(id)]() {
            const bool is_mock = (id == "MOCK_VIDEO_CAM" || id.rfind("MOCK_", 0) == 0);
            const std::string json = is_mock
                ? fmt::format(R"({{"mock_id":"{}","device_id":"{}","detections":[]}})", id, id)
                : fmt::format(R"({{"device_id":"{}","detections":[]}})", id);
            if (is_mock) {
                (void)bawal::server::postMockInspectionPayload(nullptr, backend_base_url, json);
            } else {
                (void)bawal::server::postInspectionPayload(nullptr, backend_base_url, json);
            }
        }).detach();
    };

    std::vector<DetectionResult> current_detections;
    current_detections.reserve(32);
    InspectionOutput inspection;

    while (true) {
        bool frame_ok = false;
        inspection.is_valid = false;
        inspection.detections.clear();
        inspection.dark_status = DarkDetectionResult{};

        cv::Mat frame_to_stream;
        uint32_t active_fid = 0;

        // INGESTION: POSIX SHM IPC vs HTTP Grabber (Mock Video)
        if (use_shm) {
            if (!shm_mailbox.waitTake(frame_to_stream, std::chrono::milliseconds(10)) || frame_to_stream.empty()) {
                if (!headless) (void)cv::waitKey(1);
                continue;
            }
            active_fid = shm_latest_frame_id.load(std::memory_order_relaxed);
        } else {
            CapturedFrame captured;
            const auto wait_budget = use_video_source ? std::chrono::milliseconds(50)
                                                      : std::chrono::milliseconds(10);
            if (!frame_grabber.waitTake(captured, wait_budget) || captured.image.empty()) {
                if (!headless) (void)cv::waitKey(1);
                continue;
            }

            frame_to_stream = std::move(captured.image);
            active_fid = static_cast<uint32_t>(frame_seq + 1);

            if (captured.rewound) {
                pipeline.resetTracker();
                video_has_rewound.store(true, std::memory_order_relaxed);
                fmt::print("[INFO] Video rewind detected: duplicate inspection writes suppressed.\n");
            }

            const bool detections_changed = ai_detections_updated.exchange(false, std::memory_order_relaxed);
            if (captured.duplicate && !detections_changed) {
                continue;
            }
        }

        if (!use_video_source) {
            std::string discovered;
            bool has_discovery_update = false;
            {
                std::lock_guard<std::mutex> lock(discovery_mutex);
                if (discovery_has_update) {
                    discovered = discovered_device_id;
                    discovery_has_update = false;
                    has_discovery_update = true;
                }
            }

            if (device_id_pinned) {
                has_discovery_update = false;
            }

            if (has_discovery_update && discovered != device_id) {
                const std::string prev_device_id = device_id;
                device_id = discovered;

                {
                    std::lock_guard<std::mutex> lock(ai_mutex);
                    current_device_id = device_id;
                    shared_detections.clear();
                    last_valid_detection_time = std::chrono::steady_clock::now();
                }
                pipeline.resetTracker();
                postDeviceClearAsync(prev_device_id);
                postDeviceClearAsync(device_id);

                stream_url = device_id.empty()
                    ? fmt::format("{}/api/v1/camera/latest/raw", backend_base_url)
                    : fmt::format("{}/api/v1/camera/latest/{}/raw", backend_base_url, device_id);
                if (!use_shm) {
                    frame_grabber.setStreamUrl(stream_url, fmt::format("{}/api/v1/camera/latest/raw", backend_base_url));
                }
                fmt::print("[INFO] Active camera stream: {}\n", device_id.empty() ? "<none>" : device_id);
                fmt::print("[INFO] Ingestion URL: {}\n", stream_url);
            }
        }

        inspection.frame_id = (active_fid > 0) ? active_fid : static_cast<uint32_t>(frame_seq + 1);

        {
            std::lock_guard<std::mutex> lock(ai_mutex);
            current_detections.assign(shared_detections.begin(), shared_detections.end());
        }

        // Real-Time Video Streaming Pass (Dewarp + Fast ISP + Overlay, < 6 ms)
        const bool is_mock_video = use_video_source || (device_id.find("MOCK") != std::string::npos);
        if (pipeline.processStream(frame_to_stream, current_detections, inspection, is_mock_video)) {
            frame_ok = true;
            frame_seq++;

            const auto now_ai = std::chrono::steady_clock::now();
            const auto ms_since_ai = std::chrono::duration_cast<std::chrono::milliseconds>(now_ai - last_ai_dispatch).count();
            if (ms_since_ai >= 90) {
                ai_mailbox.publish(inspection.dewarped_image);
                last_ai_dispatch = now_ai;
            }

            if (frame_seq % 60 == 1) {
                std::cout << "[STREAM] Fluid 30+ FPS OK | Frame #" << frame_seq 
                          << " (" << std::fixed << std::setprecision(1) << inspection.processing_time_ms << " ms) | Live Boxes: "
                          << current_detections.size() << "\n";
            }

            if (!inspection.annotated_image.empty() && !device_id.empty()) {
                // Encoding + HTTP upload are owned by the publisher worker;
                // publish() deep-copies so the overlay buffer can be reused.
                (void)frame_publisher.publish(device_id, inspection.frame_id, inspection.annotated_image);
            }
        }

        if (!headless) {
            cv::Mat display_frame;
            if (!frame_ok || inspection.annotated_image.empty()) {
                display_frame = cv::Mat::zeros(480, 640, CV_8UC3);
                cv::putText(display_frame, "BAWAL VISION AI1 PIPELINE", cv::Point(145, 180),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
                cv::putText(display_frame, use_shm ? "Waiting for SHM Frame..." : "Waiting for ESP32-S3 Stream...",
                            cv::Point(145, 230), cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
                cv::putText(display_frame, "Target Device: " + device_id, cv::Point(145, 265),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(100, 255, 100), 1, cv::LINE_AA);
                cv::putText(display_frame, "URL: " + stream_url, cv::Point(145, 295),
                            cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(180, 180, 180), 1, cv::LINE_AA);
            } else {
                display_frame = inspection.annotated_image;
            }

            cv::imshow(window_name, display_frame);

            const int key = cv::waitKey(1);
            if (key == 'q' || key == 'Q' || key == 27) {
                std::cout << "[INFO] User requested stop. Closing vision window...\n";
                break;
            }
        }
    }

    // Prosedur Shutdown Urut & Bersih
    if (use_shm) {
        shm_worker_running.store(false, std::memory_order_release);
        if (shm_worker.joinable()) {
            shm_worker.join();
        }
        // shm_consumer otomatis dieksekusi destruktornya (RAII)
    } else {
        frame_grabber.stop();
    }

    discovery_running.store(false, std::memory_order_release);
    ai_worker_running.store(false, std::memory_order_release);
    if (discovery_worker.joinable()) {
        discovery_worker.join();
    }
    if (ai_worker.joinable()) {
        ai_worker.join();
    }
    frame_publisher.stop();

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

#endif // BUILD_AWS_LAMBDA
