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
#include "bawal/server/jpeg_utils.hpp"
#include "bawal/server/rolling_image_store.hpp"
#include "bawal/server/sha256.hpp"
#include "bawal/vision/classifier.hpp"
#include "bawal/vision/pipeline.hpp"
#include "bawal/vision/types.hpp"

namespace bawal::backend {
namespace {

constexpr std::chrono::milliseconds kIdlePoll{2};
constexpr std::chrono::milliseconds kInitRetry{500};
constexpr std::chrono::milliseconds kAnnotatedCacheInterval{250};

// A tray must be seen consistently before it is written to the audit log, and
// must stay empty for a long grace window before the state machine unlocks.
// 18 frames (~1s) was too short: a transient missed detection could unlock the
// tray and log the same plate again. Require both 45 consecutive empty frames
// and 3 full seconds since the last detection.
constexpr int kStableTrayFrames = 4;
constexpr int kEmptyTrayFrames = 45;
constexpr std::chrono::milliseconds kEmptyTrayHold{3000};

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
        << "|device_id=" << entry.device_id
        << "|engine_pipeline=" << entry.engine_pipeline
        << "|verdict=" << entry.verdict
        << "|surface_temp=" << entry.surface_temp
        << "|lux_luminance=" << entry.lux_luminance
        << "|color_temperature=" << entry.color_temperature
        << "|white_balance_status=" << entry.white_balance_status
        << "|spoilage_risk=" << entry.spoilage_risk
        << "|tray_coverage=" << entry.tray_coverage
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

float VisionWorker::intersectionOverUnion(const TraySignature& lhs,
                                           const TraySignature& rhs) {
    if (lhs.width <= 0 || lhs.height <= 0 || rhs.width <= 0 || rhs.height <= 0) {
        return 0.0F;
    }

    const int x1 = std::max(lhs.x, rhs.x);
    const int y1 = std::max(lhs.y, rhs.y);
    const int x2 = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
    const int y2 = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
    const int intersection = std::max(0, x2 - x1) * std::max(0, y2 - y1);
    if (intersection <= 0) {
        return 0.0F;
    }

    const int lhs_area = lhs.width * lhs.height;
    const int rhs_area = rhs.width * rhs.height;
    const int union_area = lhs_area + rhs_area - intersection;
    if (union_area <= 0) {
        return 0.0F;
    }
    return static_cast<float>(intersection) / static_cast<float>(union_area);
}

VisionWorker::TraySignature VisionWorker::primarySignature(
    const std::vector<bawal::vision::DetectionResult>& detections) {
    TraySignature signature;
    const bawal::vision::DetectionResult* best = nullptr;
    int best_area = 0;

    for (const auto& detection : detections) {
        if (detection.class_id < 0 || detection.class_name == "no_food" ||
            detection.class_name == "no_model") {
            continue;
        }
        const int area = std::max(0, detection.bbox.width) *
                         std::max(0, detection.bbox.height);
        if (best == nullptr || area > best_area ||
            (area == best_area && detection.confidence > best->confidence)) {
            best = &detection;
            best_area = area;
        }
    }

    if (best != nullptr) {
        signature.valid = true;
        signature.class_name = best->class_name;
        signature.x = best->bbox.x;
        signature.y = best->bbox.y;
        signature.width = best->bbox.width;
        signature.height = best->bbox.height;
    }
    return signature;
}

bool VisionWorker::sameSignature(const TraySignature& lhs,
                                 const TraySignature& rhs) {
    return lhs.valid && rhs.valid && lhs.class_name == rhs.class_name &&
           intersectionOverUnion(lhs, rhs) > 0.40F;
}

void VisionWorker::resetTrayState() noexcept {
    m_current_tray_locked = false;
    m_stable_frames_count = 0;
    m_empty_frames_count = 0;
    m_last_tray_signature = TraySignature{};
    m_candidate_signature = TraySignature{};
    m_last_detection_time = std::chrono::steady_clock::time_point{};
}

bool VisionWorker::updateTrayState(
    const std::vector<bawal::vision::DetectionResult>& detections) {
    if (detections.empty()) {
        m_stable_frames_count = 0;
        m_candidate_signature = TraySignature{};
        ++m_empty_frames_count;

        const auto now = std::chrono::steady_clock::now();
        const bool hold_elapsed = m_last_detection_time.time_since_epoch().count() == 0 ||
                                  now - m_last_detection_time >= kEmptyTrayHold;
        if (m_empty_frames_count >= kEmptyTrayFrames && hold_elapsed) {
            m_current_tray_locked = false;
            m_last_tray_signature = TraySignature{};
            m_empty_frames_count = 0;
        }
        return false;
    }

    const TraySignature signature = primarySignature(detections);
    if (!signature.valid) {
        return false;
    }

    m_empty_frames_count = 0;
    m_last_detection_time = std::chrono::steady_clock::now();

    // A tray is locked from its first stable emit until the scene stays empty
    // long enough to be considered cleared. Bounding-box jitter or a second
    // object appearing on the same tray must never create a duplicate record.
    if (m_current_tray_locked) {
        return false;
    }

    if (sameSignature(m_candidate_signature, signature)) {
        ++m_stable_frames_count;
    } else {
        m_candidate_signature = signature;
        m_stable_frames_count = 1;
    }

    if (m_stable_frames_count < kStableTrayFrames) {
        return false;
    }

    // Single audit emit for this tray. Lock immediately so subsequent frames
    // of the same object are suppressed.
    m_current_tray_locked = true;
    m_last_tray_signature = signature;
    m_candidate_signature = signature;
    m_stable_frames_count = 0;
    return true;
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

    uint64_t processed = 0;
    uint64_t persist_failures = 0;
    while (m_running.load(std::memory_order_acquire)) {
        cv::Mat frame;
        uint32_t frame_id = 0;
        if (!consumer.waitForNextFrame(frame, frame_id) || frame.empty()) {
            std::this_thread::sleep_for(kIdlePoll);
            continue;
        }

        const std::string device_id = m_state.camera().device_id;
        const bool is_mock_video = device_id == "MOCK_VIDEO_CAM" ||
                                   device_id.rfind("MOCK_", 0) == 0;

        // A mock video loop/seek or a manual re-inspection resets the tray
        // state machine exactly once per request.
        if (m_state.consumeTrackerReset()) {
            pipeline.resetTracker();
            resetTrayState();
        }

        // Drop frames produced before a mock/live switch: draining them keeps
        // the previous source's overlay and tray state off the new stream.
        if (m_state.sourceClearActive()) {
            continue;
        }

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

        // Always render the overlay for the stream and keep an annotated JPEG
        // snapshot for manual audits; only the debounced tray emit persists it.
        bawal::vision::InspectionOutput stream_output;
        stream_output.frame_id = frame_id;
        if (!pipeline.processStream(frame, valid, stream_output, is_mock_video)) {
            std::cerr << "[VISION] stream overlay failed for frame #" << frame_id << "\n";
            continue;
        }
        const cv::Mat& annotated = !stream_output.annotated_image.empty()
            ? stream_output.annotated_image
            : frame;

        const auto now = std::chrono::steady_clock::now();
        if (m_last_annotated_encode.time_since_epoch().count() == 0 ||
            now - m_last_annotated_encode >= kAnnotatedCacheInterval) {
            m_last_annotated_encode = now;
            std::vector<uint8_t> annotated_jpeg;
            if (server::encodeJPEGFrame(annotated, annotated_jpeg, 85)) {
                m_state.updateAnnotatedJpeg(std::move(annotated_jpeg));
            }
        }

        const bool should_emit = updateTrayState(valid);
        if (should_emit) {
            InspectionHistoryEntry history;
            history.frame_id = frame_id;
            history.timestamp = isoUtcTimestamp();
            const float frame_area = static_cast<float>(
                std::max(1, frame.cols) * std::max(1, frame.rows));
            bool rejected = false;
            double max_spoilage = 0.0;
            double coverage_sum = 0.0;
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
                max_spoilage = std::max(
                    max_spoilage, static_cast<double>(soa.spoilage_score[i]));
                coverage_sum += detection.portion_ratio;
                history.detections.push_back(std::move(detection));
            }

            history.verdict = rejected ? "REJECTED" : "ACCEPTED";
            history.device_id = device_id;
            history.engine_pipeline = engine_pipeline;
            history.lux_luminance = static_cast<double>(optical.lux_luminance);
            history.color_temperature = optical.color_temperature;
            history.white_balance_status = optical.white_balance_status[0] != '\0'
                ? std::string(optical.white_balance_status)
                : "BALANCED";
            history.spoilage_risk = max_spoilage;
            history.tray_coverage = std::min(100.0, coverage_sum * 100.0);
            const TelemetryView telemetry = m_state.telemetry();
            history.surface_temp = telemetry.has_temperature
                ? static_cast<double>(telemetry.temperature)
                : 0.0;
            if (m_image_store != nullptr) {
                // Store the rendered overlay (bounding boxes + labels), never
                // the raw camera frame.
                history.image_path =
                    m_image_store->save_frame(frame_id, annotated);
            }
            history.t_safe_hours = estimateSafeHours(
                static_cast<float>(history.surface_temp));
            history.checksum_sha256 =
                server::sha256Hex(canonicalAuditPayload(history));
            m_state.pushInspectionHistory(std::move(history));

            // Force the manual-snapshot cache to this exact emitted frame.
            std::vector<uint8_t> annotated_jpeg;
            if (server::encodeJPEGFrame(annotated, annotated_jpeg, 85)) {
                m_state.updateAnnotatedJpeg(std::move(annotated_jpeg));
            }

            if (m_database != nullptr) {
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

            std::cout << "[VISION] single audit emit frame #" << frame_id
                      << " | detections: " << valid.size()
                      << " | device: " << (device_id.empty() ? "<none>" : device_id)
                      << "\n";
        }

        processed++;

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
