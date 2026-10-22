#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

namespace bawal::server {

/**
 * @brief Non-blocking, drop-oldest uploader for annotated MJPEG frames.
 *
 * The video/stream thread only hands over a deep copy of the annotated image;
 * the worker performs JPEG encoding and the blocking HTTP POST. If the network
 * cannot keep up with the camera, the producer overwrites the pending frame,
 * so latency never grows without bound and a slow backend cannot stall frame
 * capture or inference.
 *
 * publish() recycles cv::Mat buffers with the worker, so steady-state operation
 * does not allocate a new frame buffer for every encode.
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
     * @brief Queue the latest annotated frame. Never blocks on network I/O.
     *
     * Copies @p image into an internal buffer before returning, so the caller
     * may immediately reuse its overlay buffer on the next frame.
     *
     * @return true when the frame was accepted into the single-slot queue.
     */
    bool publish(std::string device_id, uint32_t frame_id, const cv::Mat& image);

private:
    void run();

    std::string m_backend_base_url;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    cv::Mat m_pending_image;
    cv::Mat m_spare_image;
    std::string m_pending_device;
    uint32_t m_pending_frame_id{0};
    bool m_has_pending{false};
};

} // namespace bawal::server
