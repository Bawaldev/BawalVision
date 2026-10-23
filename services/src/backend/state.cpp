#include "bawal/backend/state.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace bawal::backend {
namespace {

constexpr uint64_t kActiveWindowNs = 5'000'000'000ULL;

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
    const uint64_t now = nowNs();

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!device_id.empty()) {
        m_device_id.assign(device_id);
    }

    m_frame_times_ns.push_back(now);
    constexpr uint64_t kFpsWindowNs = 1'000'000'000ULL;
    while (m_frame_times_ns.size() > 1 && now - m_frame_times_ns.front() > kFpsWindowNs) {
        m_frame_times_ns.pop_front();
    }
    if (m_frame_times_ns.size() > 1) {
        const uint64_t span_ns = now - m_frame_times_ns.front();
        m_fps = span_ns > 0
            ? static_cast<double>(m_frame_times_ns.size() - 1) * 1.0e9 / static_cast<double>(span_ns)
            : 0.0;
        m_fps = std::clamp(m_fps, 0.0, 240.0);
    } else {
        m_fps = 0.0;
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
        m_last_frame_ns = 0;
        m_prev_frame_ns = 0;
        m_fps = 0.0;
        m_device_id.clear();
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

TelemetryView BackendState::telemetry() const {
    const uint64_t now = nowNs();

    std::lock_guard<std::mutex> lock(m_mutex);
    TelemetryView view;
    view.valid = m_telemetry_received_ns != 0;
    view.device_id = m_telemetry_device_id;
    view.firmware_version = m_telemetry_firmware;
    view.camera_model = m_telemetry_camera_model;
    view.connection_mode = m_telemetry_connection_mode;
    view.ip_address = m_telemetry_ip.empty() ? m_peer_ip : m_telemetry_ip;
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
