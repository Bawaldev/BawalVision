#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bawal::server {

/**
 * @brief Non-blocking, drop-oldest uploader for annotated MJPEG frames.
 *
 * The video/stream thread only hands over the already encoded JPEG bytes; the
 * worker performs the blocking HTTP POST. If the network cannot keep up with
 * the camera, the producer overwrites the pending frame, so latency never grows
 * without bound and a slow backend cannot stall frame capture or inference.
 *
 * publish() recycles buffers with the worker, so steady-state operation does
 * not allocate a new JPEG buffer for every frame.
 */
class AsyncAnnotatedFramePublisher {
public:
    explicit AsyncAnnotatedFramePublisher(std::string backend_base_url);
    ~AsyncAnnotatedFramePublisher();

    AsyncAnnotatedFramePublisher(const AsyncAnnotatedFramePublisher&) = delete;
    AsyncAnnotatedFramePublisher& operator=(const AsyncAnnotatedFramePublisher&) = delete;

    void start();
    void stop();

    /**
     * @brief Queue the latest frame. Never blocks on network I/O.
     *
     * @param jpeg_bytes Buffer to swap with an internal spare. On return it may
     *                   hold a recycled buffer for the next encode.
     * @return true when the frame was accepted into the single-slot queue.
     */
    bool publish(std::string device_id, uint32_t frame_id, std::vector<uint8_t>& jpeg_bytes);

private:
    void run();

    std::string m_backend_base_url;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<uint8_t> m_pending_bytes;
    std::vector<uint8_t> m_spare_bytes;
    std::string m_pending_device;
    uint32_t m_pending_frame_id{0};
    bool m_has_pending{false};
};

} // namespace bawal::server
