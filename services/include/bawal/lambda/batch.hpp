#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "bawal/lambda/types.hpp"

namespace bawal::lambda {

/** A single telemetry/log record accepted by the batch summary endpoint. */
struct BatchTelemetryRecord {
    std::string device_id;
    std::string timestamp;
    std::string verdict;
    double temperature{0.0};
    double cpu_load{0.0};
    double rssi{0.0};
    double heap_free{0.0};
    double lux_luminance{0.0};
    double spoilage_score{0.0};
    uint64_t frame_id{0};
    bool has_temperature{false};
    bool has_cpu_load{false};
    bool has_rssi{false};
    bool has_heap_free{false};
    bool has_lux_luminance{false};
    bool has_spoilage_score{false};
    bool has_frame_id{false};
};

/** Parsed batch event (50-100 records recommended, 100 maximum). */
struct LambdaBatchRequest {
    std::string batch_id;
    std::string device_id;
    std::vector<BatchTelemetryRecord> records;
};

struct BatchDeviceSummary {
    std::string device_id;
    uint32_t record_count{0};
    double avg_temperature{0.0};
    double max_cpu_load{0.0};
    double min_heap_free{0.0};
    double max_spoilage_score{0.0};
    uint32_t anomaly_count{0};
};

/** Aggregated, compact response returned to API Gateway / direct invoke. */
struct BatchSummary {
    std::string batch_id;
    std::string status{"OK"};
    uint32_t record_count{0};
    uint32_t device_count{0};
    uint32_t anomaly_count{0};
    double processing_time_ms{0.0};
    double avg_temperature{0.0};
    double min_temperature{0.0};
    double max_temperature{0.0};
    double avg_cpu_load{0.0};
    double avg_rssi{0.0};
    double min_heap_free{0.0};
    double avg_lux_luminance{0.0};
    double max_spoilage_score{0.0};
    uint32_t pass_count{0};
    uint32_t review_count{0};
    uint32_t reject_count{0};
    uint32_t unknown_verdict_count{0};
    std::vector<BatchDeviceSummary> devices;
};

/**
 * @brief Pure-CPU batch telemetry/log aggregator.
 *
 * This path never loads an ONNX model, so a batch-only Lambda cold start is
 * bounded by JSON parsing and a few linear passes over at most 100 records.
 */
class BatchSummaryProcessor {
public:
    static constexpr uint32_t kMaxRecords = 100;

    /** Fast payload sniffing before attempting a full JSON parse. */
    [[nodiscard]] static bool looksLikeBatch(std::string_view event_payload);

    [[nodiscard]] bool parse(std::string_view event_payload,
                             LambdaBatchRequest& out_request,
                             std::string& out_error) const;

    [[nodiscard]] LambdaInspectionResponse process(
        const LambdaBatchRequest& request) const;
};

} // namespace bawal::lambda

template <>
struct glz::meta<bawal::lambda::BatchDeviceSummary> {
    using T = bawal::lambda::BatchDeviceSummary;
    static constexpr auto value = glz::object(
        "device_id", &T::device_id,
        "record_count", &T::record_count,
        "avg_temperature", &T::avg_temperature,
        "max_cpu_load", &T::max_cpu_load,
        "min_heap_free", &T::min_heap_free,
        "max_spoilage_score", &T::max_spoilage_score,
        "anomaly_count", &T::anomaly_count);
};

template <>
struct glz::meta<bawal::lambda::BatchSummary> {
    using T = bawal::lambda::BatchSummary;
    static constexpr auto value = glz::object(
        "batch_id", &T::batch_id,
        "status", &T::status,
        "record_count", &T::record_count,
        "device_count", &T::device_count,
        "anomaly_count", &T::anomaly_count,
        "processing_time_ms", &T::processing_time_ms,
        "avg_temperature", &T::avg_temperature,
        "min_temperature", &T::min_temperature,
        "max_temperature", &T::max_temperature,
        "avg_cpu_load", &T::avg_cpu_load,
        "avg_rssi", &T::avg_rssi,
        "min_heap_free", &T::min_heap_free,
        "avg_lux_luminance", &T::avg_lux_luminance,
        "max_spoilage_score", &T::max_spoilage_score,
        "pass_count", &T::pass_count,
        "review_count", &T::review_count,
        "reject_count", &T::reject_count,
        "unknown_verdict_count", &T::unknown_verdict_count,
        "devices", &T::devices);
};
