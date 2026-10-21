#include "bawal/server/async_frame_publisher.hpp"

#include "bawal/server/http_utils.hpp"

#include <utility>

namespace bawal::server {

AsyncAnnotatedFramePublisher::AsyncAnnotatedFramePublisher(std::string backend_base_url)
    : m_backend_base_url(std::move(backend_base_url)) {}

AsyncAnnotatedFramePublisher::~AsyncAnnotatedFramePublisher() {
    stop();
}

void AsyncAnnotatedFramePublisher::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    m_thread = std::thread([this]() { run(); });
}

void AsyncAnnotatedFramePublisher::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool AsyncAnnotatedFramePublisher::publish(std::string device_id,
                                           uint32_t frame_id,
                                           std::vector<uint8_t>& jpeg_bytes) {
    if (jpeg_bytes.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_pending) {
            // Drop the superseded frame and retain its allocation for reuse.
            std::swap(m_spare_bytes, m_pending_bytes);
        }
        // Swap with the caller buffer: the caller receives the previous spare
        // allocation back, keeping both sides allocation-free in steady state.
        std::swap(m_pending_bytes, jpeg_bytes);
        m_pending_device = std::move(device_id);
        m_pending_frame_id = frame_id;
        m_has_pending = true;
    }
    m_cv.notify_one();
    return true;
}

void AsyncAnnotatedFramePublisher::run() {
    std::vector<uint8_t> work_bytes;
    std::string device_id;

    while (true) {
        uint32_t frame_id = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return m_has_pending || !m_running.load(std::memory_order_acquire);
            });

            if (!m_running.load(std::memory_order_acquire) && !m_has_pending) {
                break;
            }
            if (!m_has_pending) {
                continue;
            }

            std::swap(work_bytes, m_pending_bytes);
            device_id = std::move(m_pending_device);
            frame_id = m_pending_frame_id;
            m_has_pending = false;
        }

        // Blocking network call happens outside every short-lived mutex.
        (void)postAnnotatedFrame(nullptr, m_backend_base_url, device_id, frame_id, work_bytes);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_spare_bytes.empty()) {
                std::swap(m_spare_bytes, work_bytes);
            }
        }
    }
}

} // namespace bawal::server
