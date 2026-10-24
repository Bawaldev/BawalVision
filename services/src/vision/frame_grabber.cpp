#include "bawal/vision/frame_grabber.hpp"

#include <algorithm>
#include <utility>

namespace bawal::vision {

LatestFrameGrabber::LatestFrameGrabber(VideoSource& source)
    : m_source(source) {}

LatestFrameGrabber::~LatestFrameGrabber() {
    stop();
}

void LatestFrameGrabber::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    m_thread = std::thread([this]() { run(); });
}

void LatestFrameGrabber::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void LatestFrameGrabber::setStreamUrl(std::string primary_url, std::string fallback_url) {
    std::lock_guard<std::mutex> lock(m_config_mutex);
    m_primary_url = std::move(primary_url);
    m_fallback_url = std::move(fallback_url);
    m_url_dirty = true;
}

void LatestFrameGrabber::run() {
    int backoff_ms = 2;

    while (m_running.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(m_config_mutex);
            if (m_url_dirty) {
                m_source.setStreamUrl(m_primary_url, m_fallback_url);
                m_url_dirty = false;
            }
        }

        CapturedFrame frame;
        const bool ok = m_source.grabNextFrame(frame.image);
        if (!ok || frame.image.empty()) {
            // Exponential idle backoff; never busy-spin on a missing camera.
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, 250);
            continue;
        }
        backoff_ms = 2;

        frame.sequence = ++m_sequence;
        frame.rewound = m_source.didRewind();
        frame.duplicate = m_source.is_duplicate_frame;
<<<<<<< HEAD
        const bool duplicate = frame.duplicate;

=======
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Single-slot mailbox: the previous unconsumed frame is dropped.
            m_pending = std::move(frame);
            m_has_pending = true;
        }
        m_cv.notify_one();

<<<<<<< HEAD
        // Yield briefly so a live HTTP poll cannot monopolise a CPU core or
        // hammer the backend when it keeps returning the same cached frame.
        std::this_thread::sleep_for(std::chrono::milliseconds(duplicate ? 8 : 1));
=======
        // Cap backend polling at ~20 FPS. The old 1 ms hot loop produced up to
        // 1000 GETs/s and starved the backend's raw frame ingest socket.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending = CapturedFrame{};
    m_has_pending = false;
}

bool LatestFrameGrabber::tryTake(CapturedFrame& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_has_pending) {
        return false;
    }
    out = std::move(m_pending);
    m_pending = CapturedFrame{};
    m_has_pending = false;
    return true;
}

bool LatestFrameGrabber::waitTake(CapturedFrame& out, std::chrono::milliseconds timeout_ms) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_cv.wait_for(lock, timeout_ms, [this]() { return m_has_pending; })) {
        return false;
    }
    out = std::move(m_pending);
    m_pending = CapturedFrame{};
    m_has_pending = false;
    return true;
}

LatestMatMailbox::LatestMatMailbox(size_t recycle_capacity)
    : m_recycle_capacity(std::max<size_t>(1, recycle_capacity)) {
    m_recycle.reserve(m_recycle_capacity);
}

void LatestMatMailbox::publish(const cv::Mat& frame) {
    if (frame.empty()) {
        return;
    }

    cv::Mat buffer;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_recycle.empty()) {
            buffer = std::move(m_recycle.back());
            m_recycle.pop_back();
        }
    }

    if (buffer.empty() || buffer.size() != frame.size() || buffer.type() != frame.type()) {
        buffer.create(frame.size(), frame.type());
    }
    frame.copyTo(buffer);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_pending && m_recycle.size() < m_recycle_capacity) {
            m_recycle.push_back(std::move(m_pending));
        }
        m_pending = std::move(buffer);
        m_has_pending = true;
    }
    m_cv.notify_one();
}

bool LatestMatMailbox::tryTake(cv::Mat& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_has_pending) {
        return false;
    }
    out = std::move(m_pending);
    m_pending = cv::Mat{};
    m_has_pending = false;
    return true;
}

bool LatestMatMailbox::waitTake(cv::Mat& out, std::chrono::milliseconds timeout_ms) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_cv.wait_for(lock, timeout_ms, [this]() { return m_has_pending; })) {
        return false;
    }
    out = std::move(m_pending);
    m_pending = cv::Mat{};
    m_has_pending = false;
    return true;
}

void LatestMatMailbox::recycle(cv::Mat&& frame) {
    if (frame.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_recycle.size() < m_recycle_capacity) {
        m_recycle.push_back(std::move(frame));
    }
}

void LatestMatMailbox::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending = cv::Mat{};
    m_has_pending = false;
    m_recycle.clear();
}

} // namespace bawal::vision
