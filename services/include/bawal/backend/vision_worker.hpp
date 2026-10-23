#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "bawal/backend/database.hpp"
#include "bawal/backend/state.hpp"

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
    void run();

    BackendState& m_state;
    Database* m_database{nullptr};
    server::RollingImageStore* m_image_store{nullptr};
    std::string m_model_path;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_consumer_ready{false};
    std::atomic<bool> m_failed{false};
};

} // namespace bawal::backend
