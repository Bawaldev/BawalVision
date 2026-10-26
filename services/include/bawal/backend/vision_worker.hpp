#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "bawal/backend/database.hpp"
#include "bawal/backend/state.hpp"
#include "bawal/vision/classifier.hpp"

namespace bawal::server {
class RollingImageStore;
}

namespace bawal::backend {

/// Resolves MODEL_PATH or the first existing repository/container candidate.
[[nodiscard]] std::string resolveModelPath();

/**
 * @brief Vision pipeline thread inside the Crow process.
 *
 * Crow -> ShmProducer -> POSIX SHM -> ShmFrameConsumer -> YOLO/ONNX pipeline
 * -> DetectionSoA snapshot. No local HTTP hop and no cgo.
 */
class VisionWorker {
public:
    VisionWorker(BackendState& state, std::string model_path,
                 Database* database = nullptr,
                 server::RollingImageStore* image_store = nullptr);
    ~VisionWorker();

    VisionWorker(const VisionWorker&) = delete;
    VisionWorker& operator=(const VisionWorker&) = delete;

    bool start();
    void stop();

    [[nodiscard]] bool consumerReady() const noexcept {
        return m_consumer_ready.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool failed() const noexcept {
        return m_failed.load(std::memory_order_acquire);
    }

private:
    struct TraySignature {
        bool valid{false};
        std::string class_name;
        int x{0};
        int y{0};
        int width{0};
        int height{0};
    };

    void run();

    static float intersectionOverUnion(const TraySignature& lhs,
                                       const TraySignature& rhs);
    static TraySignature primarySignature(
        const std::vector<bawal::vision::DetectionResult>& detections);
    static bool sameSignature(const TraySignature& lhs,
                              const TraySignature& rhs);

    /// Debounces the tray: returns true exactly once when a new, stable food
    /// object should be written to the audit log and rolling image buffer.
    bool updateTrayState(
        const std::vector<bawal::vision::DetectionResult>& detections);
    void resetTrayState() noexcept;

    bool m_current_tray_locked{false};
    int m_stable_frames_count{0};
    int m_empty_frames_count{0};
    TraySignature m_last_tray_signature;
    TraySignature m_candidate_signature;
    std::chrono::steady_clock::time_point m_last_detection_time{};
    std::chrono::steady_clock::time_point m_last_annotated_encode{};

    BackendState& m_state;
    Database* m_database{nullptr};
    server::RollingImageStore* m_image_store{nullptr};
    std::string m_model_path;
    std::string m_lambda_endpoint;
    std::string m_lambda_auth_token;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_consumer_ready{false};
    std::atomic<bool> m_failed{false};
};

} // namespace bawal::backend
