#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bawal::lambda {

inline constexpr std::size_t kMaxEventPayloadBytes = 6U * 1024U * 1024U;
inline constexpr std::size_t kMaxImageBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxMarkdownBytes = 2U * 1024U * 1024U;
inline constexpr std::size_t kMaxTelemetryBytes = 64U * 1024U;
inline constexpr std::size_t kMaxJsonResponseBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxBinaryResponseBytes = 4U * 1024U * 1024U;
inline constexpr int kMaxImageDimension = 8192;
inline constexpr std::uint64_t kMaxImagePixels = 16'000'000ULL;

/**
 * @brief Configuration parameters for the AWS Lambda serverless runtime.
 */
struct LambdaConfig {
    std::string runtime_api_endpoint;   ///< AWS_LAMBDA_RUNTIME_API (e.g. 127.0.0.1:9001)
    std::string model_path;             ///< Path to the quantized INT8 YOLOv8 model
    std::string auth_token;             ///< Shared bearer token for HTTP invocations
    bool enforce_http_auth{true};       ///< Require token/IAM auth for HTTP events
    float confidence_threshold{0.35f};  ///< Detection confidence threshold (0.25 - 0.98)
    int processing_width{640};          ///< Target width for frame normalization
    int processing_height{480};         ///< Target height for frame normalization
    int num_threads{2};                 ///< OpenMP & ONNX thread allocation
    bool return_annotated{false};       ///< Include a base64 annotated JPEG in responses
    bool enable_prewarm{true};          ///< Run a single-pass pre-warm on cold boot
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
 * @brief Representation of an incoming HTTP request routed to the serverless function.
 */
struct LambdaHttpRoute {
    std::string path;
    std::string method{"POST"};
    bool is_http_event{false};
};

/**
 * @brief Serverless inspection request representation for event-driven invocation.
 */
struct LambdaInspectionRequest {
    std::vector<uint8_t> raw_image_bytes;
    std::string device_id{"ESP32S3_INSPECT"};
    std::string request_id;
    std::string image_content_type{"image/jpeg"};
    InvocationSource source{InvocationSource::DIRECT_INVOKE};
    uint64_t frame_id{0};
    float confidence_threshold{0.35f};
    bool return_annotated{false};
    int processing_width{0};
    int processing_height{0};
};

/**
 * @brief Serverless inspection response container.
 */
struct LambdaInspectionResponse {
    std::string json_payload;
    int status_code{200};
    double execution_time_ms{0.0};
    bool is_success{false};
    std::vector<uint8_t> binary_body;
    std::string content_type{"application/json"};
    std::string content_disposition;
    bool is_binary{false};
};

} // namespace bawal::lambda
