#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bawal::backend {

/**
 * @brief Structure-of-Arrays detection storage shared between the vision
 * worker and the HTTP handlers.
 *
 * The web layer is the only OOP boundary in this process. Detection data is
 * kept as flat, cache-aligned arrays so NMS, JSON conversion, and any future
 * SIMD pass can walk one field at a time without touching unused bytes.
 */
struct alignas(64) DetectionSoA {
    static constexpr size_t kMaxDetections = 64;
    static constexpr size_t kClassNameBytes = 32;

    float x[kMaxDetections]{};
    float y[kMaxDetections]{};
    float width[kMaxDetections]{};
    float height[kMaxDetections]{};
    float confidence[kMaxDetections]{};
    float spoilage_score[kMaxDetections]{};
    int32_t class_id[kMaxDetections]{};
    uint8_t status[kMaxDetections]{};       // FoodStatus
    char class_name[kMaxDetections][kClassNameBytes]{};

    uint32_t count{0};
    uint8_t reserved[60]{};                 // explicit pad to 64-byte multiple
};

static_assert(alignof(DetectionSoA) == 64, "DetectionSoA must be cache-line aligned");
static_assert(sizeof(DetectionSoA) == 3968, "DetectionSoA layout/padding mismatch");

enum class FoodStatus : uint8_t {
    Unknown = 0,
    Pass = 1,
    Review = 2,
    Reject = 3,
};

struct alignas(64) SceneOpticalSoA {
    float avg_r{0.0f};
    float avg_g{0.0f};
    float avg_b{0.0f};
    float lux_luminance{0.0f};
    int32_t color_temperature{6500};
    char white_balance_status[32]{"BALANCED"};
    uint8_t reserved[12]{};                 // explicit pad to 64 bytes
};

static_assert(alignof(SceneOpticalSoA) == 64, "SceneOpticalSoA must be cache-line aligned");
static_assert(sizeof(SceneOpticalSoA) == 64, "SceneOpticalSoA must be exactly 64 bytes");

struct CameraView {
    bool active{false};
    std::string device_id;
    double fps{0.0};
    uint64_t frames_total{0};
    uint64_t last_frame_age_ms{0};
    uint32_t frame_id{0};
    uint16_t width{0};
    uint16_t height{0};
};

struct InspectionView {
    bool valid{false};
    std::string device_id;
    std::string engine_pipeline;
    uint64_t sequence{0};
    uint64_t age_ms{0};
    uint32_t frame_id{0};
    DetectionSoA detections;
    SceneOpticalSoA optical;
};

struct InspectionHistoryDetection {
    std::string class_name;
    float confidence{0.0F};
    std::array<int, 4> bbox{};
    float freshness_score{0.0F};
    float portion_ratio{0.0F};
};

struct InspectionHistoryEntry {
    uint64_t frame_id{0};
    std::string timestamp;
    std::string verdict;
    double surface_temp{0.0};
    std::string image_path;
    std::vector<InspectionHistoryDetection> detections;
    double t_safe_hours{0.0};
    std::string checksum_sha256;
};

struct TelemetryView {
    bool valid{false};
    std::string device_id;
    std::string firmware_version;
    std::string camera_model;
    std::string connection_mode;
    std::string ip_address;
    uint64_t timestamp{0};
    uint64_t age_ms{0};
    float temperature{0.0f};
    float cpu_temperature{0.0f};
    uint8_t cpu_load{0};
    int8_t cpu_core0_load{-1};
    int8_t cpu_core1_load{-1};
    uint64_t heap_free{0};
    uint64_t free_heap_internal{0};
    uint64_t free_heap_psram{0};
    int16_t rssi{0};
    bool has_temperature{false};
    bool has_cpu_temperature{false};
    bool has_cpu_load{false};
    bool has_heap_free{false};
    bool has_free_heap_internal{false};
    bool has_free_heap_psram{false};
    bool has_rssi{false};
};

/**
 * @brief Partial telemetry update. Only fields with their `has_*` flag set (or
 * non-empty strings) overwrite the cached values, so a frame-header sample and
 * a full JSON heartbeat can merge into one view.
 */
struct TelemetrySample {
    std::string device_id;
    std::string firmware_version;
    std::string camera_model;
    std::string connection_mode;
    std::string ip_address;
    uint64_t timestamp{0};
    float temperature{0.0f};
    float cpu_temperature{0.0f};
    int cpu_load{-1};
    int cpu_core0_load{-1};
    int cpu_core1_load{-1};
    uint64_t heap_free{0};
    uint64_t free_heap_internal{0};
    uint64_t free_heap_psram{0};
    int rssi{0};
    bool has_temperature{false};
    bool has_cpu_temperature{false};
    bool has_cpu_load{false};
    bool has_heap_free{false};
    bool has_free_heap_internal{false};
    bool has_free_heap_psram{false};
    bool has_rssi{false};
};

/**
 * @brief Process-local state shared by Crow worker threads and the vision
 * thread. POSIX SHM carries the frame bytes; this object only carries small
 * metadata and the latest detection snapshot.
 */
class BackendState {
public:
    BackendState() = default;

    void updateFrame(std::string_view device_id,
                     uint32_t frame_id,
                     const std::vector<uint8_t>& jpeg,
                     uint16_t width = 0,
                     uint16_t height = 0);

    void updateInspection(std::string_view device_id,
                          uint32_t frame_id,
                          const DetectionSoA& detections,
                          const SceneOpticalSoA& optical,
                          std::string_view engine_pipeline);

    void updateTelemetry(const TelemetrySample& sample);
    void updatePeerIp(std::string ip);
    void disconnectDevice(std::string_view device_id);
    [[nodiscard]] CameraView liveCamera() const;

    void pushInspectionHistory(InspectionHistoryEntry entry);
    [[nodiscard]] std::vector<InspectionHistoryEntry> inspectionHistory(
        std::size_t limit = 100) const;

    // One-shot request consumed by the vision worker when a mock video loops
    // or the operator asks for a fresh re-inspection.
    void requestTrackerReset() noexcept {
        m_tracker_reset_requested.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool consumeTrackerReset() noexcept {
        return m_tracker_reset_requested.exchange(false, std::memory_order_acq_rel);
    }

    void updateAnnotatedJpeg(std::vector<uint8_t> jpeg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_latest_annotated_jpeg = std::move(jpeg);
    }

    [[nodiscard]] CameraView camera() const;
    [[nodiscard]] InspectionView inspection() const;
    [[nodiscard]] TelemetryView telemetry() const;
    [[nodiscard]] std::vector<uint8_t> latestJpeg(std::string_view device_id = {}) const;
    [[nodiscard]] std::vector<uint8_t> latestAnnotatedJpeg(
        std::string_view device_id = {}) const;

    void setVisionReady(bool ready) noexcept {
        m_vision_ready.store(ready, std::memory_order_release);
    }
    [[nodiscard]] bool visionReady() const noexcept {
        return m_vision_ready.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kMaxInspectionHistory = 100;

    static uint64_t nowNs() noexcept;

    mutable std::mutex m_mutex;

    std::string m_device_id;
    std::vector<uint8_t> m_latest_jpeg;
    std::vector<uint8_t> m_latest_annotated_jpeg;
    uint16_t m_width{0};
    uint16_t m_height{0};

    uint64_t m_frames_total{0};
    std::chrono::steady_clock::time_point m_last_frame_time{};
    uint32_t m_last_frame_id{0};
    double m_fps{0.0};

    // Live stream metrics exclude browser-supplied MOCK_* video frames.
    std::string m_live_device_id;
    uint64_t m_live_frames_total{0};
    std::chrono::steady_clock::time_point m_live_last_frame_time{};
    uint32_t m_live_last_frame_id{0};
    uint16_t m_live_width{0};
    uint16_t m_live_height{0};
    double m_live_fps{0.0};

    uint64_t m_inspection_sequence{0};
    uint64_t m_inspection_timestamp_ns{0};
    uint32_t m_inspection_frame_id{0};
    std::string m_inspection_device_id;
    std::string m_engine_pipeline;
    DetectionSoA m_detections;
    SceneOpticalSoA m_optical;
    std::deque<InspectionHistoryEntry> m_inspection_history;

    uint64_t m_telemetry_received_ns{0};
    std::string m_telemetry_device_id;
    std::string m_telemetry_firmware;
    std::string m_telemetry_camera_model;
    std::string m_telemetry_connection_mode;
    std::string m_telemetry_ip;
    uint64_t m_telemetry_timestamp{0};
    float m_telemetry_temperature{0.0f};
    float m_telemetry_cpu_temperature{0.0f};
    uint8_t m_telemetry_cpu_load{0};
    int8_t m_telemetry_core0_load{-1};
    int8_t m_telemetry_core1_load{-1};
    uint64_t m_telemetry_heap_free{0};
    uint64_t m_telemetry_free_heap_internal{0};
    uint64_t m_telemetry_free_heap_psram{0};
    int16_t m_telemetry_rssi{0};
    std::string m_peer_ip;
    bool m_telemetry_has_temperature{false};
    bool m_telemetry_has_cpu_temperature{false};
    bool m_telemetry_has_cpu_load{false};
    bool m_telemetry_has_heap_free{false};
    bool m_telemetry_has_free_heap_internal{false};
    bool m_telemetry_has_free_heap_psram{false};
    bool m_telemetry_has_rssi{false};

    std::atomic<bool> m_vision_ready{false};
    std::atomic<bool> m_tracker_reset_requested{false};
};

} // namespace bawal::backend
