#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>

#include "bawal/vision/pipeline.hpp"

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
    bool return_annotated{false};       ///< Whether to include base64 annotated image in response
};

/**
 * @brief Serverless Inspection Request representation for event-driven invocation.
 */
struct LambdaInspectionRequest {
    std::vector<uint8_t> raw_image_bytes;
    std::string device_id{"ESP32S3_INSPECT"};
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

/**
 * @brief Forward initialization interface for future AWS Lambda deployment.
 * Concrete implementation will be added during final serverless container deployment.
 */
class LambdaRuntimeHandler {
public:
    LambdaRuntimeHandler() = default;
    virtual ~LambdaRuntimeHandler() = default;

    /**
     * @brief Initialize AWS Lambda environment, thread pools, and vision pipeline.
     */
    [[nodiscard]] virtual bool initialize(const LambdaConfig& config) = 0;

    /**
     * @brief Process a single tray inspection snapshot event (Dewarp -> Enhance -> INT8 YOLOv8 -> SIMD Spoilage).
     */
    [[nodiscard]] virtual LambdaInspectionResponse handleInspectionEvent(const LambdaInspectionRequest& request) = 0;
};

} // namespace bawal::lambda

namespace bawal {
    // Alias to support bawal::lamda syntax
    namespace lamda = lambda;
}

