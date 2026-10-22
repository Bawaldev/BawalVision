#pragma once

#include "shm_layout.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace bawal::ipc {

/**
 * @brief POSIX shared-memory frame producer used by the Crow HTTP ingress.
 *
 * The writer mirrors backend/internal/shm/producer.go byte-for-byte:
 *   1. mark the target slot as not-ready,
 *   2. copy the JPEG payload,
 *   3. publish the slot metadata,
 *   4. mark the slot ready,
 *   5. bump the global write sequence and notify the consumer over UDS.
 *
 * All slot metadata is cache-line aligned and padded explicitly in
 * shm_layout.hpp; this class only performs pointer casts into the mapping.
 */
class ShmProducer {
public:
    ShmProducer() = default;
    ~ShmProducer();

    ShmProducer(const ShmProducer&) = delete;
    ShmProducer& operator=(const ShmProducer&) = delete;

    [[nodiscard]] bool init();
    [[nodiscard]] bool writeFrame(uint32_t frame_id,
                                  const uint8_t* data,
                                  size_t len,
                                  uint16_t width = 0,
                                  uint16_t height = 0,
                                  uint8_t format = 0);
    [[nodiscard]] bool writeFrame(uint32_t frame_id,
                                  const std::vector<uint8_t>& data,
                                  uint16_t width = 0,
                                  uint16_t height = 0,
                                  uint8_t format = 0);

    /// Starts the lazy UDS notifier. Safe to call once after the consumer has
    /// bound /dev/shm/bawal_shm.sock (or before, thanks to retries).
    void startNotify();
    void stopNotify();

    [[nodiscard]] bool ready() const noexcept {
        return m_base_ptr != nullptr;
    }

    [[nodiscard]] uint64_t writeSequence() const noexcept {
        return m_write_seq.load(std::memory_order_acquire);
    }

private:
    void notifyLoop();
    void closeMappings();

    int m_shm_fd{-1};
    uint8_t* m_base_ptr{nullptr};
    ShmGlobalHeader* m_global_hdr{nullptr};

    std::mutex m_write_mutex;
    std::atomic<uint64_t> m_write_seq{0};

    std::atomic<int> m_notify_fd{-1};
    std::atomic<bool> m_notify_running{false};
    std::thread m_notify_thread;
};

} // namespace bawal::ipc
