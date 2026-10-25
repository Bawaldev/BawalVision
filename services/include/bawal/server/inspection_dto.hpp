#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

namespace bawal::server {

struct DetectionItem {
    std::string class_name;
    float confidence{0.0F};
    std::array<int, 4> bbox{}; // x, y, width, height
    float spoilage_score{0.0F};
    float freshness{100.0F};
};

struct InspectionResponse {
    uint64_t frame_id{0};
    std::string status;
    std::string image_path;
    double surface_temp{0.0};
    std::vector<DetectionItem> detections;
    std::string device_id;
    std::string engine_pipeline;
    double processing_time_ms{0.0};
    std::string annotated_image_base64;
    std::string message;
};

struct SceneOpticalDto {
    float avg_r{0.0F};
    float avg_g{0.0F};
    float avg_b{0.0F};
    float lux_luminance{0.0F};
    int color_temperature{6500};
    std::string white_balance_status{"BALANCED"};
};

struct InspectionDetectionDto {
    std::string food_name;
    float similarity_rate{0.0F};
    float spoilage_score{0.0F};
    std::string status;
    std::array<int, 4> box{};
};

struct LiveInspectionPayload {
    std::string device_id;
    std::string source_type{"live_esp32"};
    bool is_rewind{false};
    SceneOpticalDto scene_optical;
    std::vector<InspectionDetectionDto> detections;
};

struct MockInspectionPayload {
    std::string mock_id;
    std::string device_id;
    std::string source_type{"mock_video"};
    std::string inspection_struct{"MockInspectionRequest"};
    std::string engine_pipeline;
    bool is_rewind{false};
    SceneOpticalDto scene_optical;
    std::vector<InspectionDetectionDto> detections;
};

struct MockTelemetryPayload {
    std::string mock_id;
    std::string device_id;
    std::string source_type{"mock_video"};
    std::string telemetry_struct{"MockTelemetryPayload"};
    std::string connection_mode{"mock_simulation"};
    std::string engine_pipeline;
    bool is_rewind{false};
    SceneOpticalDto scene_optical;
};

struct LambdaEvent {
    uint64_t frame_id{0};
    std::string body;
    std::string image_base64;
    bool is_base64_encoded{false};
    std::string device_id;
    double confidence_threshold{0.35};
    bool return_annotated{false};
    int processing_width{0};
    int processing_height{0};
};

struct LambdaProxyHeaders {
    std::string content_type{"application/json"};
    std::string content_disposition;
    std::string x_report_format;
    std::string access_control_allow_origin{"*"};
    std::string access_control_allow_methods{"GET, POST, OPTIONS, HEAD"};
    std::string access_control_allow_headers{"*"};
};

struct LambdaProxyResponse {
    int status_code{200};
    LambdaProxyHeaders headers;
    bool is_base64_encoded{false};
    std::string body;
};

} // namespace bawal::server

template <>
struct glz::meta<bawal::server::DetectionItem> {
    using T = bawal::server::DetectionItem;
    static constexpr auto value = glz::object(
        "class_name", &T::class_name,
        "confidence", &T::confidence,
        "bbox", &T::bbox,
        "spoilage_score", &T::spoilage_score,
        "freshness", &T::freshness);
};

template <>
struct glz::meta<bawal::server::InspectionResponse> {
    using T = bawal::server::InspectionResponse;
    static constexpr auto value = glz::object(
        "frame_id", &T::frame_id,
        "status", &T::status,
        "image_path", &T::image_path,
        "surface_temp", &T::surface_temp,
        "detections", &T::detections,
        "device_id", &T::device_id,
        "engine_pipeline", &T::engine_pipeline,
        "processing_time_ms", &T::processing_time_ms,
        "annotated_image_base64", &T::annotated_image_base64,
        "message", &T::message);
};

template <>
struct glz::meta<bawal::server::SceneOpticalDto> {
    using T = bawal::server::SceneOpticalDto;
    static constexpr auto value = glz::object(
        "avg_r", &T::avg_r,
        "avg_g", &T::avg_g,
        "avg_b", &T::avg_b,
        "lux_luminance", &T::lux_luminance,
        "color_temperature", &T::color_temperature,
        "white_balance_status", &T::white_balance_status);
};

template <>
struct glz::meta<bawal::server::InspectionDetectionDto> {
    using T = bawal::server::InspectionDetectionDto;
    static constexpr auto value = glz::object(
        "food_name", &T::food_name,
        "similarity_rate", &T::similarity_rate,
        "spoilage_score", &T::spoilage_score,
        "status", &T::status,
        "box", &T::box);
};

template <>
struct glz::meta<bawal::server::LiveInspectionPayload> {
    using T = bawal::server::LiveInspectionPayload;
    static constexpr auto value = glz::object(
        "device_id", &T::device_id,
        "source_type", &T::source_type,
        "is_rewind", &T::is_rewind,
        "scene_optical", &T::scene_optical,
        "detections", &T::detections);
};

template <>
struct glz::meta<bawal::server::MockInspectionPayload> {
    using T = bawal::server::MockInspectionPayload;
    static constexpr auto value = glz::object(
        "mock_id", &T::mock_id,
        "device_id", &T::device_id,
        "source_type", &T::source_type,
        "inspection_struct", &T::inspection_struct,
        "engine_pipeline", &T::engine_pipeline,
        "is_rewind", &T::is_rewind,
        "scene_optical", &T::scene_optical,
        "detections", &T::detections);
};

template <>
struct glz::meta<bawal::server::MockTelemetryPayload> {
    using T = bawal::server::MockTelemetryPayload;
    static constexpr auto value = glz::object(
        "mock_id", &T::mock_id,
        "device_id", &T::device_id,
        "source_type", &T::source_type,
        "telemetry_struct", &T::telemetry_struct,
        "connection_mode", &T::connection_mode,
        "engine_pipeline", &T::engine_pipeline,
        "is_rewind", &T::is_rewind,
        "scene_optical", &T::scene_optical);
};

template <>
struct glz::meta<bawal::server::LambdaEvent> {
    using T = bawal::server::LambdaEvent;
    static constexpr auto value = glz::object(
        "frame_id", &T::frame_id,
        "body", &T::body,
        "image_base64", &T::image_base64,
        "isBase64Encoded", &T::is_base64_encoded,
        "device_id", &T::device_id,
        "confidence_threshold", &T::confidence_threshold,
        "return_annotated", &T::return_annotated,
        "processing_width", &T::processing_width,
        "processing_height", &T::processing_height);
};

template <>
struct glz::meta<bawal::server::LambdaProxyHeaders> {
    using T = bawal::server::LambdaProxyHeaders;
    static constexpr auto value = glz::object(
        "Content-Type", &T::content_type,
        "Content-Disposition", &T::content_disposition,
        "X-Report-Format", &T::x_report_format,
        "Access-Control-Allow-Origin", &T::access_control_allow_origin,
        "Access-Control-Allow-Methods", &T::access_control_allow_methods,
        "Access-Control-Allow-Headers", &T::access_control_allow_headers);
};

template <>
struct glz::meta<bawal::server::LambdaProxyResponse> {
    using T = bawal::server::LambdaProxyResponse;
    static constexpr auto value = glz::object(
        "statusCode", &T::status_code,
        "headers", &T::headers,
        "isBase64Encoded", &T::is_base64_encoded,
        "body", &T::body);
};
