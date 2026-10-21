#pragma once

#include "bawal/vision/video_source.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

namespace bawal::vision {

/**
 * @brief One frame handed from the capture thread to the consumer.
 *
 * The cv::Mat owns a reference to the decoded pixel buffer. Moving this struct
 * transfers ownership without copying the image.
 */
struct CapturedFrame {
    cv::Mat image;
    uint64_t sequence{0};
    bool rewound{false};
    bool duplicate{false};
};

/**
 * @brief Asynchronous producer for VideoSource I/O.
 *
 * A dedicated thread performs the blocking HTTP/decoder work and publishes only
 * the newest frame into a single-slot mailbox. If the consumer is slower than
 * the camera, older frames are overwritten and dropped instead of accumulating
 * an unbounded queue. The mutex is held only for pointer/handle swaps, never
 * across network I/O, JPEG decoding, or colour conversion.
 */
class LatestFrameGrabber {
public:
    explicit LatestFrameGrabber(VideoSource& source);
    ~LatestFrameGrabber();

    LatestFrameGrabber(const LatestFrameGrabber&) = delete;
    LatestFrameGrabber& operator=(const LatestFrameGrabber&) = delete;

    void start();
    void stop();

    /**
     * @brief Update ingestion URLs without blocking the caller on active I/O.
     * The capture thread applies the change before its next grab.
     */
    void setStreamUrl(std::string primary_url, std::string fallback_url = {});

    /**
     * @brief Non-blocking take of the most recent captured frame.
     * @return true when a frame was moved into out.
     */
    [[nodiscard]] bool tryTake(CapturedFrame& out);

    /**
     * @brief Wait up to timeout_ms for a frame. Intended for the main consumer
     *        loop; returns immediately when a frame is already pending.
     */
    [[nodiscard]] bool waitTake(CapturedFrame& out, std::chrono::milliseconds timeout_ms);

private:
    void run();

    VideoSource& m_source;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    CapturedFrame m_pending;
    bool m_has_pending{false};
    uint64_t m_sequence{0};

    std::mutex m_config_mutex;
    std::string m_primary_url;
    std::string m_fallback_url;
    bool m_url_dirty{false};
};

/**
 * @brief Single-slot latest-frame mailbox with a small recycled cv::Mat pool.
 *
 * Producer side copies into a recycled destination and swaps it in under a
 * short lock; a pending frame is moved back to the recycle pool when it is
 * superseded. Consumer side takes the newest frame by move. No image clone is
 * performed on the take path.
 */
class LatestMatMailbox {
public:
    explicit LatestMatMailbox(size_t recycle_capacity = 3);

    void publish(const cv::Mat& frame);
    [[nodiscard]] bool tryTake(cv::Mat& out);
    [[nodiscard]] bool waitTake(cv::Mat& out, std::chrono::milliseconds timeout_ms);
    void recycle(cv::Mat&& frame);
    void clear();

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    cv::Mat m_pending;
    bool m_has_pending{false};
    std::vector<cv::Mat> m_recycle;
    size_t m_recycle_capacity;
};

} // namespace bawal::vision
