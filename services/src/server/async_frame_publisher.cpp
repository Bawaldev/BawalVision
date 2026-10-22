#include "bawal/server/async_frame_publisher.hpp"

#include "bawal/server/http_utils.hpp"

#include <opencv2/imgcodecs.hpp>

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
                                           const cv::Mat& image) {
    if (image.empty()) {
        return false;
    }

    // Take a recycled buffer (if one is available), then copy outside the lock.
    // The copy is required because the caller reuses its overlay buffer on the
    // next frame; a shallow cv::Mat reference would race with that write.
    cv::Mat owned;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_spare_image.empty() &&
            m_spare_image.size() == image.size() &&
            m_spare_image.type() == image.type()) {
            owned = std::move(m_spare_image);
        }
    }

    try {
        if (owned.empty()) {
            owned.create(image.size(), image.type());
        }
        image.copyTo(owned);
    } catch (const cv::Exception&) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_has_pending) {
            // Drop the superseded frame and retain its allocation for reuse.
            m_spare_image = std::move(m_pending_image);
        }
        m_pending_image = std::move(owned);
        m_pending_device = std::move(device_id);
        m_pending_frame_id = frame_id;
        m_has_pending = true;
    }
    m_cv.notify_one();
    return true;
}

void AsyncAnnotatedFramePublisher::run() {
    cv::Mat work_image;
    std::string device_id;
    std::vector<uint8_t> jpeg_bytes;
    jpeg_bytes.reserve(128 * 1024);
    static const std::vector<int> kEncodeParams = {
        cv::IMWRITE_JPEG_QUALITY, 94,
        cv::IMWRITE_JPEG_OPTIMIZE, 1
    };

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

            std::swap(work_image, m_pending_image);
            device_id = std::move(m_pending_device);
            frame_id = m_pending_frame_id;
            m_has_pending = false;
        }

        // JPEG encoding and the blocking network call both happen off the
        // stream thread, outside every short-lived mutex.
        bool encoded = false;
        try {
            encoded = cv::imencode(".jpg", work_image, jpeg_bytes, kEncodeParams);
        } catch (const cv::Exception&) {
            encoded = false;
        }
        if (encoded) {
            (void)postAnnotatedFrame(nullptr, m_backend_base_url, device_id, frame_id, jpeg_bytes);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_spare_image.empty()) {
                m_spare_image = std::move(work_image);
            }
        }
    }
}

} // namespace bawal::server
