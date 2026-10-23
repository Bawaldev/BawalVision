#include "bawal/backend/vision_worker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

#include <opencv2/core.hpp>

#include "bawal/ipc/shm_consumer.hpp"
#include "bawal/server/rolling_image_store.hpp"
#include "bawal/server/sha256.hpp"
#include "bawal/vision/classifier.hpp"
#include "bawal/vision/pipeline.hpp"
#include "bawal/vision/types.hpp"

namespace bawal::backend {
namespace {

constexpr std::chrono::milliseconds kIdlePoll{2};
constexpr std::chrono::milliseconds kInitRetry{500};

std::string boundedName(std::string_view name) {
    std::string out(name);
    if (out.size() >= DetectionSoA::kClassNameBytes) {
        out.resize(DetectionSoA::kClassNameBytes - 1);
    }
    return out;
}

FoodStatus classifyStatus(const bawal::vision::DetectionResult& detection) {
    const float spoilage_pct = std::clamp(
        detection.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);

    if (detection.spoilage.is_visually_spoiled || spoilage_pct >= 45.0f) {
        return FoodStatus::Reject;
    }
    if (spoilage_pct >= 25.0f || detection.confidence < 0.60f) {
        return FoodStatus::Review;
    }
    return FoodStatus::Pass;
}

void fillDetectionSoA(const std::vector<bawal::vision::DetectionResult>& detections,
                      DetectionSoA& out) {
    out = DetectionSoA{};
    const size_t count = std::min(detections.size(), DetectionSoA::kMaxDetections);
    out.count = static_cast<uint32_t>(count);

    for (size_t i = 0; i < count; ++i) {
        const auto& d = detections[i];
        out.x[i] = static_cast<float>(d.bbox.x);
        out.y[i] = static_cast<float>(d.bbox.y);
        out.width[i] = static_cast<float>(d.bbox.width);
        out.height[i] = static_cast<float>(d.bbox.height);
        out.confidence[i] = d.confidence;
        out.spoilage_score[i] = std::clamp(
            d.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);
        out.class_id[i] = d.class_id;
        out.status[i] = static_cast<uint8_t>(classifyStatus(d));
        const std::string name = boundedName(d.class_name);
        std::memcpy(out.class_name[i], name.data(), name.size());
        out.class_name[i][name.size()] = '\0';
    }
}

SceneOpticalSoA toOpticalSoA(const bawal::vision::SceneOpticalMetrics& optical) {
    SceneOpticalSoA out;
    out.avg_r = optical.avg_r;
    out.avg_g = optical.avg_g;
    out.avg_b = optical.avg_b;
    out.lux_luminance = optical.lux_luminance;
    out.color_temperature = optical.color_temperature;
    const std::string status = boundedName(optical.white_balance_status);
    std::memcpy(out.white_balance_status, status.data(), status.size());
    out.white_balance_status[status.size()] = '\0';
    return out;
}

bool fileExists(const std::string& path) {
    std::ifstream file(path);
    return file.good();
}

std::string isoUtcTimestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_r(&now, &utc);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

double estimateSafeHours(float surface_temp) {
    if (surface_temp <= 0.0F) {
        return 0.0;
    }
    if (surface_temp >= 60.0F) {
        return 4.5;
    }
    if (surface_temp >= 50.0F) {
        return 3.0;
    }
    if (surface_temp >= 40.0F) {
        return 1.5;
    }
    return 0.5;
}

std::string canonicalAuditPayload(const InspectionHistoryEntry& entry) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4);
    out << "frame_id=" << entry.frame_id
        << "|timestamp=" << entry.timestamp
        << "|verdict=" << entry.verdict
        << "|surface_temp=" << entry.surface_temp
        << "|image_path=" << entry.image_path
        << "|t_safe_hours=" << entry.t_safe_hours;
    for (const auto& detection : entry.detections) {
        out << "|det=" << detection.class_name
            << ',' << detection.confidence
            << ',' << detection.bbox[0]
            << ',' << detection.bbox[1]
            << ',' << detection.bbox[2]
            << ',' << detection.bbox[3]
            << ',' << detection.freshness_score
            << ',' << detection.portion_ratio;
    }
    return out.str();
}

} // namespace

std::string resolveModelPath() {
    if (const char* env_model = std::getenv("MODEL_PATH")) {
        if (*env_model != '\0') {
            return env_model;
        }
    }

    const std::vector<std::string> candidates = {
        "ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
        "ml/AI1/model/ai1_vision_int8.onnx",
        "../ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
        "../../ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
        "services/build/weights/ai1_vision_int8.onnx",
        "weights/ai1_vision_int8.onnx",
        "/app/ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx",
        "/app/weights/ai1_vision_int8.onnx",
        "ml/AI1/model/yolov8s/ai1_vision.onnx",
        "ml/AI1/model/yolov8n_int8/ai1_vision_int8.onnx",
    };

    for (const auto& path : candidates) {
        if (fileExists(path)) {
            return path;
        }
    }
    return candidates.front();
}

VisionWorker::VisionWorker(BackendState& state, std::string model_path, Database* database,
                           server::RollingImageStore* image_store)
    : m_state(state),
      m_database(database),
      m_image_store(image_store),
      m_model_path(std::move(model_path)) {}

VisionWorker::~VisionWorker() {
    stop();
}

bool VisionWorker::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    m_failed.store(false, std::memory_order_release);
    m_consumer_ready.store(false, std::memory_order_release);
    m_thread = std::thread(&VisionWorker::run, this);
    return true;
}

void VisionWorker::stop() {
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_state.setVisionReady(false);
}

void VisionWorker::run() {
    bawal::ipc::ShmFrameConsumer consumer;

    // The producer owns shm_open(O_CREAT); main() initialises it before this
    // thread starts. Retry briefly to tolerate container startup ordering.
    while (m_running.load(std::memory_order_acquire) && !consumer.init()) {
        std::cerr << "[VISION] SHM consumer init failed; retrying in 500 ms\n";
        std::this_thread::sleep_for(kInitRetry);
    }

    if (!m_running.load(std::memory_order_acquire)) {
        return;
    }

    m_consumer_ready.store(true, std::memory_order_release);
    std::cout << "[VISION] POSIX SHM consumer attached (bawal_frame_shm)\n";

    bawal::vision::InspectionPipeline pipeline;
    if (!pipeline.initialize(m_model_path, 640, 480)) {
        std::cerr << "[VISION] Pipeline fallback active (model: " << m_model_path << ")\n";
    }
    m_state.setVisionReady(true);

    std::cout << "[VISION] YOLO/ONNX pipeline ready at 640x480"
              << " [" << pipeline.getModelArchitecture() << "]\n";

    const std::string engine_pipeline =
        std::string(pipeline.getModelArchitecture()) + " + OpenCV HSV";

    constexpr std::chrono::seconds kPersistInterval{2};

    uint64_t processed = 0;
    uint64_t persist_failures = 0;
    std::chrono::steady_clock::time_point last_persist{};
    while (m_running.load(std::memory_order_acquire)) {
        cv::Mat frame;
        uint32_t frame_id = 0;
        if (!consumer.waitForNextFrame(frame, frame_id) || frame.empty()) {
            std::this_thread::sleep_for(kIdlePoll);
            continue;
        }

        const std::string device_id = m_state.camera().device_id;

        const int64_t t0 = cv::getTickCount();
        std::vector<bawal::vision::DetectionResult> detected;
        const bool inferred = pipeline.runInference(frame, detected);
        const double infer_ms =
            ((cv::getTickCount() - t0) * 1000.0) / cv::getTickFrequency();

        if (!inferred) {
            std::cerr << "[VISION] inference failed for frame #" << frame_id << "\n";
            continue;
        }

        const float threshold = pipeline.getConfidenceThreshold();
        std::vector<bawal::vision::DetectionResult> valid;
        valid.reserve(detected.size());
        for (const auto& d : detected) {
            if (d.class_id >= 0 &&
                d.confidence >= threshold &&
                d.class_name != "no_food" &&
                d.class_name != "no_model") {
                valid.push_back(d);
            }
        }

        DetectionSoA soa;
        fillDetectionSoA(valid, soa);
        const SceneOpticalSoA optical =
            toOpticalSoA(pipeline.getClassifier().getLastOpticalMetrics());
        m_state.updateInspection(device_id, frame_id, soa, optical, engine_pipeline);

        InspectionHistoryEntry history;
        history.frame_id = frame_id;
        history.timestamp = isoUtcTimestamp();
        const float frame_area = static_cast<float>(
            std::max(1, frame.cols) * std::max(1, frame.rows));
        bool rejected = false;
        const uint32_t detection_count = std::min<uint32_t>(
            soa.count, DetectionSoA::kMaxDetections);
        history.detections.reserve(detection_count);
        for (uint32_t i = 0; i < detection_count; ++i) {
            const std::string class_name = boundedName(soa.class_name[i]);
            InspectionHistoryDetection detection;
            detection.class_name = class_name.empty() ? "unknown" : class_name;
            detection.confidence = soa.confidence[i];
            detection.bbox = {
                static_cast<int>(soa.x[i]),
                static_cast<int>(soa.y[i]),
                static_cast<int>(soa.width[i]),
                static_cast<int>(soa.height[i]),
            };
            detection.freshness_score = std::clamp(
                1.0F - soa.spoilage_score[i] / 100.0F, 0.0F, 1.0F);
            detection.portion_ratio = std::clamp(
                (soa.width[i] * soa.height[i]) / frame_area, 0.0F, 1.0F);
            if (soa.status[i] == static_cast<uint8_t>(FoodStatus::Reject)) {
                rejected = true;
            }
            history.detections.push_back(std::move(detection));
        }

        history.verdict = rejected ? "REJECTED" : "ACCEPTED";
        const TelemetryView telemetry = m_state.telemetry();
        history.surface_temp = telemetry.has_temperature
            ? static_cast<double>(telemetry.temperature)
            : 0.0;
        if (m_image_store != nullptr) {
            history.image_path = m_image_store->save_frame(frame_id, frame);
        }
        history.t_safe_hours = estimateSafeHours(
            static_cast<float>(history.surface_temp));
        history.checksum_sha256 = server::sha256Hex(canonicalAuditPayload(history));
        m_state.pushInspectionHistory(std::move(history));

        processed++;

        if (m_database != nullptr) {
            const auto now = std::chrono::steady_clock::now();
            if (last_persist.time_since_epoch().count() == 0 ||
                now - last_persist >= kPersistInterval) {
                last_persist = now;
                if (m_database->saveInspection(device_id, frame_id, soa, optical,
                                               engine_pipeline)) {
                    persist_failures = 0;
                } else {
                    persist_failures++;
                    if (persist_failures == 1 || persist_failures % 30 == 0) {
                        std::cerr << "[DB] Inspection persist skipped/failed ("
                                  << persist_failures << "); continuing in-memory\n";
                    }
                }
            }
        }

        if (processed % 30 == 1 || !valid.empty()) {
            std::cout << "[VISION] frame #" << frame_id
                      << " | " << infer_ms << " ms | detections: " << valid.size()
                      << " | device: " << (device_id.empty() ? "<none>" : device_id)
                      << "\n";
        }
    }

    m_state.setVisionReady(false);
    std::cout << "[VISION] worker stopped\n";
}

} // namespace bawal::backend
