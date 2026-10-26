#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/core.h>

#include "bawal/lambda/handler.hpp"
#include "bawal/lambda/runtime.hpp"

namespace {

std::string envOr(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return value;
}

int envInt(const char* key, int fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

float envFloat(const char* key, float fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value) {
        return fallback;
    }
    return static_cast<float>(parsed);
}

bool envFlag(const char* key, bool fallback) {
    const std::string value = envOr(key, "");
    if (value.empty()) {
        return fallback;
    }
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "on";
}

} // namespace

int main() {
    bawal::lambda::LambdaConfig config;
    config.runtime_api_endpoint = envOr("AWS_LAMBDA_RUNTIME_API", "");
    config.model_path = envOr(
        "MODEL_PATH", "ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx");
    config.auth_token = envOr("BAWAL_LAMBDA_AUTH_TOKEN",
                        envOr("AUTH_TOKEN",
                        envOr("LAMBDA_AUTH_TOKEN",
                        envOr("BAWAL_AUTH_TOKEN", ""))));
    config.enforce_http_auth = envFlag("ENFORCE_HTTP_AUTH", !config.auth_token.empty());
    config.confidence_threshold = envFloat("CONF_THRESHOLD", 0.35f);
    config.processing_width = envInt("PROCESSING_WIDTH", 640);
    config.processing_height = envInt("PROCESSING_HEIGHT", 480);
    config.num_threads = envInt("LAMBDA_NUM_THREADS", 2);
    config.return_annotated = envFlag("RETURN_ANNOTATED", false);
    // Batch-summary invocations do not need the ONNX model. Keep the pipeline
    // lazy; set LAMBDA_PREWARM=1 for snapshot-only deployments.
    config.enable_prewarm = envFlag("LAMBDA_PREWARM", false);

    if (std::getenv("OMP_NUM_THREADS") == nullptr) {
        const std::string threads = std::to_string(config.num_threads);
        setenv("OMP_NUM_THREADS", threads.c_str(), 1);
    }

    fmt::print(stderr,
               "[INFO] BawalVision Lambda bootstrap starting (model={}, {}x{}, "
               "threads={}, prewarm={}, auth={})\n",
               config.model_path, config.processing_width,
               config.processing_height, config.num_threads,
               config.enable_prewarm ? "on" : "off",
               config.auth_token.empty() ? "missing" : "configured");

    bawal::lambda::VisionLambdaHandler handler;
    if (!handler.initialize(config)) {
        fmt::print(stderr, "[FATAL] Failed to initialize the Lambda vision pipeline\n");
        return 1;
    }

    return bawal::lambda::runLambdaRuntime(handler, config);
}
