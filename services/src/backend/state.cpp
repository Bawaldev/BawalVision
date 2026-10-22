#include "bawal/backend/state.hpp"

#include <algorithm>
#include <chrono>

namespace bawal::backend {
namespace {

constexpr uint64_t kActiveWindowNs = 5'000'000'000ULL;

} // namespace

uint64_t BackendState::nowNs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void BackendState::updateFrame(std::string_view device_id, uint32_t frame_id,
                               const std::vector<uint8_t>& jpeg,
                               uint16_t width, uint16_t height) {
    const uint64_t now = nowNs();

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!device_id.empty()) {
        m_device_id.assign(device_id);
    }

    if (m_prev_frame_ns != 0 && now > m_prev_frame_ns) {
        const uint64_t delta_ns = now - m_prev_frame_ns;
        if (delta_ns >= 1'000'000ULL && delta_ns <= 2'000'000'000ULL) {
            const double instant = 1.0e9 / static_cast<double>(delta_ns);
            m_fps = (m_fps <= 0.0) ? instant : (m_fps * 0.9 + instant * 0.1);
            m_fps = std::clamp(m_fps, 0.0, 240.0);
        }
    }

    m_prev_frame_ns = now;
    m_last_frame_ns = now;
    m_last_frame_id = frame_id;
    m_frames_total++;

    if (width > 0 && height > 0) {
        m_width = width;
        m_height = height;
    }

    if (!jpeg.empty()) {
        m_latest_jpeg = jpeg;
    }
}

void BackendState::updateInspection(std::string_view device_id, uint32_t frame_id,
                                    const DetectionSoA& detections,
                                    const SceneOpticalSoA& optical,
                                    std::string_view engine_pipeline) {
    const uint64_t now = nowNs();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_inspection_sequence++;
    m_inspection_timestamp_ns = now;
    m_inspection_frame_id = frame_id;
    m_inspection_device_id.assign(device_id);
    m_engine_pipeline.assign(engine_pipeline);
    m_detections = detections;
    m_optical = optical;
}

CameraView BackendState::camera() const {
    const uint64_t now = nowNs();

    std::lock_guard<std::mutex> lock(m_mutex);
    CameraView view;
    view.device_id = m_device_id;
    view.frames_total = m_frames_total;
    view.frame_id = m_last_frame_id;
    view.width = m_width;
    view.height = m_height;

    if (m_last_frame_ns != 0 && now >= m_last_frame_ns) {
        const uint64_t age_ns = now - m_last_frame_ns;
        view.last_frame_age_ms = age_ns / 1'000'000ULL;
        view.active = age_ns <= kActiveWindowNs;
    }

    view.fps = view.active ? m_fps : 0.0;
    return view;
}

InspectionView BackendState::inspection() const {
    const uint64_t now = nowNs();

    std::lock_guard<std::mutex> lock(m_mutex);
    InspectionView view;
    view.valid = m_inspection_sequence > 0;
    view.device_id = m_inspection_device_id;
    view.engine_pipeline = m_engine_pipeline;
    view.sequence = m_inspection_sequence;
    view.frame_id = m_inspection_frame_id;
    view.detections = m_detections;
    view.optical = m_optical;
    if (view.valid && m_inspection_timestamp_ns != 0 && now >= m_inspection_timestamp_ns) {
        view.age_ms = (now - m_inspection_timestamp_ns) / 1'000'000ULL;
    }
    return view;
}

std::vector<uint8_t> BackendState::latestJpeg(std::string_view device_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!device_id.empty() && device_id != "*" && device_id != m_device_id) {
        return {};
    }
    return m_latest_jpeg;
}

} // namespace bawal::backend
