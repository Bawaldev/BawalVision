#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>

namespace bawal::lambda {

/**
 * @brief Configuration parameters for AWS Lambda serverless runtime.
 */
struct LambdaConfig {
    std::string runtime_api_endpoint;   ///< AWS_LAMBDA_RUNTIME_API (e.g. 127.0.0.1:9001)
    std::string model_path;             ///< Path to final quantized INT8 YOLOv8 model
    float confidence_threshold{0.35f};  ///< Detection confidence threshold (0.25 - 0.98)
    int processing_width{640};          ///< Target width for frame normalization
    int processing_height{480};         ///< Target height for frame normalization
    int num_threads{2};                 ///< OpenMP & ONNX thread allocation (optimal for 2048-3072MB Lambda)
    bool return_annotated{false};       ///< Whether to include base64 annotated image in response
    bool enable_prewarm{true};          ///< Whether to run single-pass pre-warm on cold boot
};

/**
 * @brief Invocation source classification.
 */
enum class InvocationSource : uint8_t {
    DIRECT_INVOKE = 0,       ///< Direct AWS SDK / CLI invocation
    API_GATEWAY_HTTP_V2 = 1, ///< API Gateway HTTP API v2.0 payload format
    FUNCTION_URL = 2,        ///< AWS Lambda Function URL
    STANDALONE_HTTP = 3      ///< Local testing / AWS Lambda Web Adapter
};

/**
 * @brief Serverless Inspection Request representation for event-driven invocation.
 */
struct LambdaInspectionRequest {
    std::vector<uint8_t> raw_image_bytes;
    std::string device_id{"ESP32S3_INSPECT"};
    std::string request_id;
    InvocationSource source{InvocationSource::DIRECT_INVOKE};
    float confidence_threshold{0.35f};
    bool return_annotated{false};
};

/**
 * @brief Serverless Inspection Response container.
 */
struct LambdaInspectionResponse {
    std::string json_payload;
    int status_code{200};
    double execution_time_ms{0.0};
    bool is_success{false};
};

} // namespace bawal::lambda

