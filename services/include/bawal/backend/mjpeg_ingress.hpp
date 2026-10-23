#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bawal::backend {

class BackendState;
}
namespace bawal::ipc {
class ShmProducer;
}

namespace bawal::backend {

/** Pulls the ESP32's multipart MJPEG stream and publishes only complete latest frames. */
class MjpegIngress {
public:
    MjpegIngress(BackendState& state, ipc::ShmProducer& producer);
    ~MjpegIngress();
    MjpegIngress(const MjpegIngress&) = delete;
    MjpegIngress& operator=(const MjpegIngress&) = delete;

    void start();
    void stop();
    void setPeerIp(std::string ip);

    // Callback hooks used by libcurl's C callbacks.
    bool shouldStop(uint64_t generation) const;
    void publishFrame(const uint8_t* data, size_t len);

    /** Time of the last complete frame, in steady-clock nanoseconds (0 if none yet). */
    [[nodiscard]] int64_t lastFrameNs() const noexcept {
        return m_last_frame_ns.load(std::memory_order_relaxed);
    }

    /** True while mock video owns the pipeline and the live pull must pause. */
    [[nodiscard]] bool sourcePaused() const noexcept;

private:
    void run();

    BackendState& m_state;
    ipc::ShmProducer& m_producer;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_peer_ip;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_generation{0};
    std::atomic<uint32_t> m_frame_id{0};
    std::atomic<int64_t> m_last_frame_ns{0};
};

} // namespace bawal::backend
