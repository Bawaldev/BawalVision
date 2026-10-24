#include "bawal/lambda/batch.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "bawal/lambda/base64.hpp"

namespace bawal::lambda {
namespace {

using Clock = std::chrono::steady_clock;
using JsonObject = glz::json_t::object_t;

constexpr double kTemperatureWarnC = 60.0;
constexpr double kCpuLoadWarnPercent = 90.0;
constexpr double kRssiWarnDbm = -80.0;
constexpr double kHeapWarnBytes = 32.0 * 1024.0;
constexpr double kSpoilageWarnScore = 0.5;

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string errorJson(std::string_view message) {
    std::map<std::string, std::string> payload{{"error", std::string(message)}};
    std::string encoded;
    if (const auto error = glz::write_json(payload, encoded); error) {
        return R"({"error":"internal error"})";
    }
    return encoded;
}

const glz::json_t* findMember(const glz::json_t& object,
                              std::initializer_list<std::string_view> keys) {
    const auto* map = object.get_if<JsonObject>();
    if (map == nullptr) {
        return nullptr;
    }
    for (const std::string_view key : keys) {
        const auto it = map->find(key);
        if (it != map->end()) {
            return &it->second;
        }
    }
    return nullptr;
}

std::optional<double> numberValue(const glz::json_t* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (const auto* number = value->get_if<double>()) {
        return *number;
    }
    if (const auto* flag = value->get_if<bool>()) {
        return *flag ? 1.0 : 0.0;
    }
    if (const auto* text = value->get_if<std::string>()) {
        char* end = nullptr;
        const double parsed = std::strtod(text->c_str(), &end);
        if (end != text->c_str()) {
            return parsed;
        }
    }
    return std::nullopt;
}

std::string stringValue(const glz::json_t* value) {
    if (value == nullptr) {
        return {};
    }
    if (const auto* text = value->get_if<std::string>()) {
        return *text;
    }
    if (const auto* number = value->get_if<double>()) {
        return std::to_string(static_cast<long long>(*number));
    }
    return {};
}

void fillNumber(const glz::json_t& record,
                std::initializer_list<std::string_view> keys,
                double& destination, bool& has_destination) {
    if (const auto value = numberValue(findMember(record, keys)); value.has_value()) {
        destination = *value;
        has_destination = true;
    }
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

/**
 * API Gateway HTTP API/Function URL wrap the real payload in a `body` string.
 * Unwrap that envelope so batch callers can use either direct invoke or a
 * proxy integration without changing the record schema.
 */
std::optional<std::string> normalizedPayload(std::string_view payload) {
    if (payload.empty()) {
        return std::nullopt;
    }
    if (payload.front() != '{') {
        return std::string(payload);
    }

    glz::json_t root;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(root, payload)) {
        return std::string(payload);
    }

    const glz::json_t* body = findMember(root, {"body"});
    if (body == nullptr) {
        return std::string(payload);
    }
    const bool is_gateway =
        findMember(root, {"requestContext", "rawPath", "isBase64Encoded", "version"}) !=
        nullptr;
    if (!is_gateway) {
        return std::string(payload);
    }

    std::string body_text = stringValue(body);
    if (const auto encoded = numberValue(findMember(root, {"isBase64Encoded"}));
        encoded.has_value() && *encoded != 0.0) {
        const std::vector<uint8_t> decoded = decodeBase64(body_text);
        return std::string(decoded.begin(), decoded.end());
    }
    return body_text;
}

const glz::json_t* findRecordArray(const glz::json_t& root) {
    if (root.holds<glz::json_t::array_t>()) {
        return &root;
    }
    return findMember(root, {"records", "telemetry", "logs", "batch_records",
                             "summary_records"});
}

struct DeviceAccumulator {
    uint32_t records{0};
    uint32_t anomalies{0};
    double temperature_sum{0.0};
    uint32_t temperature_count{0};
    double max_cpu_load{0.0};
    double min_heap_free{std::numeric_limits<double>::max()};
    bool has_heap{false};
    double max_spoilage_score{0.0};
};

} // namespace

bool BatchSummaryProcessor::looksLikeBatch(std::string_view event_payload) {
    if (event_payload.find("\"records\"") != std::string_view::npos ||
        event_payload.find("\"telemetry\"") != std::string_view::npos ||
        event_payload.find("\"batch_records\"") != std::string_view::npos ||
        event_payload.find("\"summary_records\"") != std::string_view::npos) {
        return true;
    }

    // Gateway envelope: only parse when the payload actually looks like one.
    if (event_payload.find("\"body\"") == std::string_view::npos ||
        (event_payload.find("\"requestContext\"") == std::string_view::npos &&
         event_payload.find("\"isBase64Encoded\"") == std::string_view::npos &&
         event_payload.find("\"rawPath\"") == std::string_view::npos)) {
        return false;
    }

    const std::optional<std::string> inner = normalizedPayload(event_payload);
    return inner.has_value() &&
           (inner->find("\"records\"") != std::string::npos ||
            inner->find("\"telemetry\"") != std::string::npos ||
            inner->find("\"batch_records\"") != std::string::npos ||
            inner->find("\"summary_records\"") != std::string::npos);
}

bool BatchSummaryProcessor::parse(std::string_view event_payload,
                                  LambdaBatchRequest& out_request,
                                  std::string& out_error) const {
    out_request = LambdaBatchRequest{};
    out_error.clear();

    const std::optional<std::string> normalized = normalizedPayload(event_payload);
    if (!normalized.has_value() || normalized->empty()) {
        out_error = "Empty batch payload";
        return false;
    }

    glz::json_t root;
    if (const auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(
            root, *normalized);
        error) {
        out_error = "Malformed batch JSON";
        return false;
    }

    out_request.batch_id = stringValue(findMember(root, {"batch_id", "batchId", "id"}));
    out_request.device_id = stringValue(findMember(root, {"device_id", "deviceId"}));

    const glz::json_t* array = findRecordArray(root);
    if (array == nullptr || !array->holds<glz::json_t::array_t>()) {
        out_error = "Batch payload must contain a records/telemetry/logs array";
        return false;
    }

    const auto& records = array->get<glz::json_t::array_t>();
    if (records.empty()) {
        out_error = "Batch payload contains zero records";
        return false;
    }
    if (records.size() > kMaxRecords) {
        out_error = "Batch payload exceeds the 100-record limit";
        return false;
    }

    out_request.records.reserve(records.size());
    for (const glz::json_t& item : records) {
        if (!item.holds<JsonObject>()) {
            out_error = "Every batch record must be a JSON object";
            return false;
        }

        BatchTelemetryRecord record;
        record.device_id = stringValue(findMember(item, {"device_id", "deviceId"}));
        if (record.device_id.empty()) {
            record.device_id = out_request.device_id;
        }
        record.timestamp = stringValue(findMember(item, {"timestamp", "ts", "created_at"}));
        record.verdict = uppercase(stringValue(findMember(item, {"verdict", "status"})));
        fillNumber(item, {"temperature", "cpu_temperature", "temp"},
                   record.temperature, record.has_temperature);
        fillNumber(item, {"cpu_load", "cpuLoad", "load"},
                   record.cpu_load, record.has_cpu_load);
        fillNumber(item, {"rssi", "wifi_rssi"}, record.rssi, record.has_rssi);
        fillNumber(item, {"heap_free", "free_heap_internal", "free_heap_psram", "heap"},
                   record.heap_free, record.has_heap_free);
        fillNumber(item, {"lux_luminance", "lux"}, record.lux_luminance,
                   record.has_lux_luminance);
        fillNumber(item, {"spoilage_score", "spoilage_risk"}, record.spoilage_score,
                   record.has_spoilage_score);
        if (const auto value = numberValue(findMember(item, {"frame_id", "frameId"}));
            value.has_value() && *value >= 0.0) {
            record.frame_id = static_cast<uint64_t>(*value);
            record.has_frame_id = true;
        }

        out_request.records.push_back(std::move(record));
    }
    return true;
}

LambdaInspectionResponse BatchSummaryProcessor::process(
    const LambdaBatchRequest& request) const {
    const Clock::time_point start = Clock::now();

    if (request.records.empty()) {
        LambdaInspectionResponse response;
        response.status_code = 400;
        response.is_success = false;
        response.execution_time_ms = elapsedMs(start);
        response.json_payload = errorJson("Batch payload contains zero records");
        return response;
    }
    if (request.records.size() > kMaxRecords) {
        LambdaInspectionResponse response;
        response.status_code = 413;
        response.is_success = false;
        response.execution_time_ms = elapsedMs(start);
        response.json_payload = errorJson("Batch payload exceeds the 100-record limit");
        return response;
    }

    BatchSummary summary;
    summary.batch_id = request.batch_id;
    summary.record_count = static_cast<uint32_t>(request.records.size());

    double temperature_sum = 0.0;
    uint32_t temperature_count = 0;
    double min_temperature = std::numeric_limits<double>::max();
    double max_temperature = std::numeric_limits<double>::lowest();
    double cpu_load_sum = 0.0;
    uint32_t cpu_load_count = 0;
    double rssi_sum = 0.0;
    uint32_t rssi_count = 0;
    double min_heap = std::numeric_limits<double>::max();
    uint32_t heap_count = 0;
    double lux_sum = 0.0;
    uint32_t lux_count = 0;
    double max_spoilage = 0.0;
    uint32_t spoilage_count = 0;

    std::map<std::string, DeviceAccumulator> devices;

    for (const BatchTelemetryRecord& record : request.records) {
        const std::string device_id =
            record.device_id.empty() ? request.device_id : record.device_id;
        DeviceAccumulator& device = devices[device_id];
        ++device.records;

        bool anomalous = false;
        if (record.has_temperature) {
            temperature_sum += record.temperature;
            ++temperature_count;
            min_temperature = std::min(min_temperature, record.temperature);
            max_temperature = std::max(max_temperature, record.temperature);
            device.temperature_sum += record.temperature;
            ++device.temperature_count;
            anomalous = anomalous || record.temperature > kTemperatureWarnC;
        }
        if (record.has_cpu_load) {
            cpu_load_sum += record.cpu_load;
            ++cpu_load_count;
            device.max_cpu_load = std::max(device.max_cpu_load, record.cpu_load);
            anomalous = anomalous || record.cpu_load > kCpuLoadWarnPercent;
        }
        if (record.has_rssi) {
            rssi_sum += record.rssi;
            ++rssi_count;
            anomalous = anomalous || record.rssi < kRssiWarnDbm;
        }
        if (record.has_heap_free) {
            min_heap = std::min(min_heap, record.heap_free);
            ++heap_count;
            device.min_heap_free = std::min(device.min_heap_free, record.heap_free);
            device.has_heap = true;
            anomalous = anomalous || record.heap_free < kHeapWarnBytes;
        }
        if (record.has_lux_luminance) {
            lux_sum += record.lux_luminance;
            ++lux_count;
        }
        if (record.has_spoilage_score) {
            max_spoilage = std::max(max_spoilage, record.spoilage_score);
            device.max_spoilage_score =
                std::max(device.max_spoilage_score, record.spoilage_score);
            ++spoilage_count;
            anomalous = anomalous || record.spoilage_score > kSpoilageWarnScore;
        }

        switch (record.verdict.empty() ? 0 : 1) {
            case 0:
                ++summary.unknown_verdict_count;
                break;
            default:
                if (record.verdict == "PASS") {
                    ++summary.pass_count;
                } else if (record.verdict == "REVIEW") {
                    ++summary.review_count;
                } else if (record.verdict == "REJECT") {
                    ++summary.reject_count;
                } else {
                    ++summary.unknown_verdict_count;
                }
                break;
        }

        if (anomalous) {
            ++summary.anomaly_count;
            ++device.anomalies;
        }
    }

    summary.device_count = static_cast<uint32_t>(devices.size());
    summary.avg_temperature =
        temperature_count > 0 ? temperature_sum / temperature_count : 0.0;
    summary.min_temperature =
        temperature_count > 0 ? min_temperature : 0.0;
    summary.max_temperature =
        temperature_count > 0 ? max_temperature : 0.0;
    summary.avg_cpu_load = cpu_load_count > 0 ? cpu_load_sum / cpu_load_count : 0.0;
    summary.avg_rssi = rssi_count > 0 ? rssi_sum / rssi_count : 0.0;
    summary.min_heap_free = heap_count > 0 ? min_heap : 0.0;
    summary.avg_lux_luminance = lux_count > 0 ? lux_sum / lux_count : 0.0;
    summary.max_spoilage_score = spoilage_count > 0 ? max_spoilage : 0.0;

    const uint32_t critical_threshold =
        std::max<uint32_t>(1, summary.record_count / 10);
    if (summary.anomaly_count == 0) {
        summary.status = "OK";
    } else if (summary.anomaly_count >= critical_threshold) {
        summary.status = "CRITICAL";
    } else {
        summary.status = "WARN";
    }

    summary.devices.reserve(devices.size());
    for (auto& [device_id, aggregate] : devices) {
        BatchDeviceSummary device;
        device.device_id = device_id;
        device.record_count = aggregate.records;
        device.avg_temperature = aggregate.temperature_count > 0
                                     ? aggregate.temperature_sum /
                                           aggregate.temperature_count
                                     : 0.0;
        device.max_cpu_load = aggregate.max_cpu_load;
        device.min_heap_free = aggregate.has_heap ? aggregate.min_heap_free : 0.0;
        device.max_spoilage_score = aggregate.max_spoilage_score;
        device.anomaly_count = aggregate.anomalies;
        summary.devices.push_back(std::move(device));
    }

    LambdaInspectionResponse response;
    response.status_code = 200;
    response.is_success = true;
    response.execution_time_ms = elapsedMs(start);
    summary.processing_time_ms = response.execution_time_ms;
    if (const auto error = glz::write_json(summary, response.json_payload); error) {
        response.status_code = 500;
        response.is_success = false;
        response.json_payload = errorJson("Could not serialize batch summary");
    }
    return response;
}

} // namespace bawal::lambda
