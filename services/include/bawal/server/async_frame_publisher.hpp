#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

<<<<<<< HEAD
=======
#include <opencv2/core.hpp>

>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
namespace bawal::server {

/**
 * @brief Non-blocking, drop-oldest uploader for annotated MJPEG frames.
 *
<<<<<<< HEAD
 * The video/stream thread only hands over the already encoded JPEG bytes; the
 * worker performs the blocking HTTP POST. If the network cannot keep up with
 * the camera, the producer overwrites the pending frame, so latency never grows
 * without bound and a slow backend cannot stall frame capture or inference.
 *
 * publish() recycles buffers with the worker, so steady-state operation does
 * not allocate a new JPEG buffer for every frame.
=======
 * The video/stream thread only hands over a deep copy of the annotated image;
 * the worker performs JPEG encoding and the blocking HTTP POST. If the network
 * cannot keep up with the camera, the producer overwrites the pending frame,
 * so latency never grows without bound and a slow backend cannot stall frame
 * capture or inference.
 *
 * publish() recycles cv::Mat buffers with the worker, so steady-state operation
 * does not allocate a new frame buffer for every encode.
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
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
<<<<<<< HEAD
     * @brief Queue the latest frame. Never blocks on network I/O.
     *
     * @param jpeg_bytes Buffer to swap with an internal spare. On return it may
     *                   hold a recycled buffer for the next encode.
     * @return true when the frame was accepted into the single-slot queue.
     */
    bool publish(std::string device_id, uint32_t frame_id, std::vector<uint8_t>& jpeg_bytes);
=======
     * @brief Queue the latest annotated frame. Never blocks on network I/O.
     *
     * Copies @p image into an internal buffer before returning, so the caller
     * may immediately reuse its overlay buffer on the next frame.
     *
     * @return true when the frame was accepted into the single-slot queue.
     */
    bool publish(std::string device_id, uint32_t frame_id, const cv::Mat& image);
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444

private:
    void run();

    std::string m_backend_base_url;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    std::mutex m_mutex;
    std::condition_variable m_cv;
<<<<<<< HEAD
    std::vector<uint8_t> m_pending_bytes;
    std::vector<uint8_t> m_spare_bytes;
=======
    cv::Mat m_pending_image;
    cv::Mat m_spare_image;
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
    std::string m_pending_device;
    uint32_t m_pending_frame_id{0};
    bool m_has_pending{false};
};

} // namespace bawal::server
