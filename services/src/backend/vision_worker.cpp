#include "bawal/backend/vision_worker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

#include <opencv2/core.hpp>

#include "bawal/ipc/shm_consumer.hpp"
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

VisionWorker::VisionWorker(BackendState& state, std::string model_path)
    : m_state(state), m_model_path(std::move(model_path)) {}

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
