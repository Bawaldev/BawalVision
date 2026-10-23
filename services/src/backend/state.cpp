#include "bawal/backend/state.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace bawal::backend {
namespace {

constexpr auto kCameraFrameStaleAfter = std::chrono::milliseconds(1500);

bool isValidJpeg(const std::vector<uint8_t>& jpeg) {
    return jpeg.size() >= 4 && jpeg[0] == 0xFF && jpeg[1] == 0xD8 &&
           jpeg[jpeg.size() - 2] == 0xFF && jpeg.back() == 0xD9;
}

bool isMockDeviceId(std::string_view device_id) {
    return device_id == "MOCK_VIDEO_CAM" || device_id.rfind("MOCK_", 0) == 0;
}

// Docker bridge / Desktop VM addresses never identify a LAN edge node.
bool isDockerOrLoopbackAddress(std::string_view ip) {
    if (ip.empty() || ip == "0.0.0.0") {
        return true;
    }
    if (ip.rfind("127.", 0) == 0 || ip.rfind("192.168.65.", 0) == 0) {
        return true;
    }
    if (ip.rfind("172.", 0) == 0) {
        const std::size_t dot = ip.find('.', 4);
        if (dot != std::string_view::npos) {
            const std::string octet(ip.substr(4, dot - 4));
            char* end = nullptr;
            const long value = std::strtol(octet.c_str(), &end, 10);
            if (end != octet.c_str() && value >= 16 && value <= 31) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

uint64_t BackendState::nowNs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void BackendState::updateFrame(std::string_view device_id, uint32_t frame_id,
                               const std::vector<uint8_t>& jpeg,
                               uint16_t width, uint16_t height) {
    if (!isValidJpeg(jpeg)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!device_id.empty()) {
        m_device_id.assign(device_id);
    }

    // Average over a 1 s window: instantaneous 1/delta spikes when the pull
    // delivers a burst of buffered frames after a stall.
    ++m_fps_window_frames;
    if (m_fps_window_start.time_since_epoch().count() == 0) {
        m_fps_window_start = now;
        m_fps_window_frames = 1;
        m_fps = 0.0;
    } else if (const double window_sec =
                   std::chrono::duration<double>(now - m_fps_window_start).count();
               window_sec >= 1.0) {
        m_fps = static_cast<double>(m_fps_window_frames) / window_sec;
        m_fps_window_start = now;
        m_fps_window_frames = 0;
    }
    m_last_frame_time = now;
    m_last_frame_id = frame_id;
    m_frames_total++;

    if (width > 0 && height > 0) {
        m_width = width;
        m_height = height;
    }

    m_latest_jpeg = jpeg;

    if (isMockDeviceId(device_id)) {
        return;
    }

    m_live_device_id.assign(device_id);
    ++m_live_fps_window_frames;
    if (m_live_fps_window_start.time_since_epoch().count() == 0) {
        m_live_fps_window_start = now;
        m_live_fps_window_frames = 1;
        m_live_fps = 0.0;
    } else if (const double window_sec =
                   std::chrono::duration<double>(now - m_live_fps_window_start).count();
               window_sec >= 1.0) {
        m_live_fps = static_cast<double>(m_live_fps_window_frames) / window_sec;
        m_live_fps_window_start = now;
        m_live_fps_window_frames = 0;
    }
    m_live_last_frame_time = now;
    m_live_last_frame_id = frame_id;
    m_live_frames_total++;
    if (width > 0 && height > 0) {
        m_live_width = width;
        m_live_height = height;
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

void BackendState::pushInspectionHistory(InspectionHistoryEntry entry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_inspection_history.size() >= kMaxInspectionHistory) {
        m_inspection_history.pop_front();
    }
    m_inspection_history.push_back(std::move(entry));
}

std::vector<InspectionHistoryEntry> BackendState::inspectionHistory(
    std::size_t limit) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    limit = std::clamp<std::size_t>(limit, 1, kMaxInspectionHistory);

    std::vector<InspectionHistoryEntry> entries;
    const std::size_t start = m_inspection_history.size() > limit
        ? m_inspection_history.size() - limit
        : 0;
    entries.reserve(m_inspection_history.size() - start);
    for (std::size_t i = start; i < m_inspection_history.size(); ++i) {
        entries.push_back(m_inspection_history[i]);
    }
    return entries;
}

void BackendState::updateTelemetry(const TelemetrySample& sample) {
    const uint64_t now = nowNs();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!sample.device_id.empty()) {
        m_telemetry_device_id = sample.device_id;
    }
    if (!sample.firmware_version.empty()) {
        m_telemetry_firmware = sample.firmware_version;
    }
    if (!sample.camera_model.empty()) {
        m_telemetry_camera_model = sample.camera_model;
    }
    if (!sample.connection_mode.empty()) {
        m_telemetry_connection_mode = sample.connection_mode;
    }
    if (!sample.ip_address.empty()) {
        m_telemetry_ip = sample.ip_address;
    }
    if (sample.timestamp > 0) {
        m_telemetry_timestamp = sample.timestamp;
    }
    if (sample.has_temperature) {
        m_telemetry_temperature = sample.temperature;
        m_telemetry_has_temperature = true;
    }
    if (sample.has_cpu_temperature) {
        m_telemetry_cpu_temperature = sample.cpu_temperature;
        m_telemetry_has_cpu_temperature = true;
    }
    if (sample.has_cpu_load) {
        m_telemetry_cpu_load = static_cast<uint8_t>(std::clamp(sample.cpu_load, 0, 100));
        m_telemetry_has_cpu_load = true;
    }
    if (sample.cpu_core0_load >= 0) {
        m_telemetry_core0_load = static_cast<int8_t>(std::clamp(sample.cpu_core0_load, 0, 100));
    }
    if (sample.cpu_core1_load >= 0) {
        m_telemetry_core1_load = static_cast<int8_t>(std::clamp(sample.cpu_core1_load, 0, 100));
    }
    if (sample.has_heap_free) {
        m_telemetry_heap_free = sample.heap_free;
        m_telemetry_has_heap_free = true;
    }
    if (sample.has_free_heap_internal) {
        m_telemetry_free_heap_internal = sample.free_heap_internal;
        m_telemetry_has_free_heap_internal = true;
    }
    if (sample.has_free_heap_psram) {
        m_telemetry_free_heap_psram = sample.free_heap_psram;
        m_telemetry_has_free_heap_psram = true;
    }
    if (sample.has_rssi) {
        m_telemetry_rssi = static_cast<int16_t>(sample.rssi);
        m_telemetry_has_rssi = true;
    }
    m_telemetry_received_ns = now;
}

void BackendState::updatePeerIp(std::string ip) {
    if (isDockerOrLoopbackAddress(ip)) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_peer_ip = std::move(ip);
}

void BackendState::disconnectDevice(std::string_view device_id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    const bool all = device_id.empty();
    if (all || device_id == m_device_id) {
        m_last_frame_time = {};
        m_fps = 0.0;
        m_fps_window_start = {};
        m_fps_window_frames = 0;
        m_device_id.clear();
    }
    if (all || device_id == m_live_device_id) {
        m_live_last_frame_time = {};
        m_live_device_id.clear();
        m_live_fps = 0.0;
        m_live_fps_window_start = {};
        m_live_fps_window_frames = 0;
    }
    if (all || device_id == m_telemetry_device_id) {
        m_telemetry_received_ns = 0;
        m_telemetry_device_id.clear();
        m_telemetry_firmware.clear();
        m_telemetry_camera_model.clear();
        m_telemetry_connection_mode.clear();
        m_telemetry_ip.clear();
        m_telemetry_timestamp = 0;
        m_telemetry_has_temperature = false;
        m_telemetry_has_cpu_temperature = false;
        m_telemetry_has_cpu_load = false;
        m_telemetry_has_heap_free = false;
        m_telemetry_has_free_heap_internal = false;
        m_telemetry_has_free_heap_psram = false;
        m_telemetry_has_rssi = false;
    }
}

void BackendState::clearDetectionState() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_latest_jpeg.clear();
    m_latest_annotated_jpeg.clear();
    m_inspection_sequence = 0;
    m_inspection_timestamp_ns = 0;
    m_inspection_frame_id = 0;
    m_inspection_device_id.clear();
    m_detections = DetectionSoA{};
    m_optical = SceneOpticalSoA{};
}

void BackendState::requestSourceClear() noexcept {
    m_tracker_reset_requested.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    m_source_clear_deadline_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch())
            .count(),
        std::memory_order_release);
}

bool BackendState::sourceClearActive() const noexcept {
    const int64_t deadline = m_source_clear_deadline_ns.load(std::memory_order_acquire);
    if (deadline == 0) {
        return false;
    }
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    return now < deadline;
}

TelemetryView BackendState::telemetry() const {
    const uint64_t now = nowNs();

    std::lock_guard<std::mutex> lock(m_mutex);
    TelemetryView view;
    view.valid = m_telemetry_received_ns != 0;
    view.device_id = m_telemetry_device_id;
    view.firmware_version = m_telemetry_firmware;
    view.camera_model = m_telemetry_camera_model;
    view.connection_mode = m_telemetry_connection_mode;
    // The discovery responder observes the current UDP peer directly. Prefer
    // that DHCP-fresh address over an older IP cached in telemetry.
    view.ip_address = m_peer_ip.empty() ? m_telemetry_ip : m_peer_ip;
    view.timestamp = m_telemetry_timestamp;
    view.temperature = m_telemetry_temperature;
    view.cpu_temperature = m_telemetry_cpu_temperature;
    view.cpu_load = m_telemetry_cpu_load;
    view.cpu_core0_load = m_telemetry_core0_load;
    view.cpu_core1_load = m_telemetry_core1_load;
    view.heap_free = m_telemetry_heap_free;
    view.free_heap_internal = m_telemetry_free_heap_internal;
    view.free_heap_psram = m_telemetry_free_heap_psram;
    view.rssi = m_telemetry_rssi;
    view.has_temperature = m_telemetry_has_temperature;
    view.has_cpu_temperature = m_telemetry_has_cpu_temperature;
    view.has_cpu_load = m_telemetry_has_cpu_load;
    view.has_heap_free = m_telemetry_has_heap_free;
    view.has_free_heap_internal = m_telemetry_has_free_heap_internal;
    view.has_free_heap_psram = m_telemetry_has_free_heap_psram;
    view.has_rssi = m_telemetry_has_rssi;
    if (view.valid && now >= m_telemetry_received_ns) {
        view.age_ms = (now - m_telemetry_received_ns) / 1'000'000ULL;
    }
    return view;
}

CameraView BackendState::camera() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    CameraView view;
    view.device_id = m_device_id;
    view.frames_total = m_frames_total;
    view.frame_id = m_last_frame_id;
    view.width = m_width;
    view.height = m_height;

    if (m_last_frame_time.time_since_epoch().count() != 0) {
        const auto age = std::chrono::steady_clock::now() - m_last_frame_time;
        view.last_frame_age_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
        view.active = age <= kCameraFrameStaleAfter;
    }

    view.fps = view.active ? m_fps : 0.0;
    return view;
}

CameraView BackendState::liveCamera() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    CameraView view;
    view.device_id = m_live_device_id;
    view.frames_total = m_live_frames_total;
    view.frame_id = m_live_last_frame_id;
    view.width = m_live_width;
    view.height = m_live_height;
    if (m_live_last_frame_time.time_since_epoch().count() != 0) {
        const auto age = std::chrono::steady_clock::now() - m_live_last_frame_time;
        view.last_frame_age_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
        view.active = age <= kCameraFrameStaleAfter;
    }
    view.fps = view.active ? m_live_fps : 0.0;
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

std::vector<uint8_t> BackendState::latestAnnotatedJpeg(std::string_view device_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!device_id.empty() && device_id != "*" && device_id != m_device_id) {
        return {};
    }
    return m_latest_annotated_jpeg;
}

} // namespace bawal::backend
