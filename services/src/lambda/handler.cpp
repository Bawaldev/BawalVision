#include "bawal/lambda/handler.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <opencv2/imgcodecs.hpp>

#include "bawal/lambda/base64.hpp"
#include "bawal/lambda/runtime.hpp"
#include "bawal/server/inspection_dto.hpp"
#include "bawal/vision/pipeline.hpp"

namespace bawal::lambda {
namespace {

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

LambdaInspectionResponse errorResponse(int status_code, std::string message,
                                       Clock::time_point start) {
    LambdaInspectionResponse response;
    response.status_code = status_code;
    response.is_success = false;
    response.execution_time_ms = elapsedMs(start);

    std::map<std::string, std::string> payload{{"error", std::move(message)}};
    if (const auto error = glz::write_json(payload, response.json_payload); error) {
        response.json_payload = R"({"error":"internal error"})";
    }
    return response;
}

int clampDimension(int value, int fallback) {
    if (value < 64 || value > 4096) {
        return fallback;
    }
    return value;
}

} // namespace

VisionLambdaHandler::VisionLambdaHandler() = default;
VisionLambdaHandler::~VisionLambdaHandler() = default;

bool VisionLambdaHandler::initialize(const LambdaConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    m_configured.store(true, std::memory_order_release);

    // Batch-only deployments must not pay the ONNX model cold-start cost.
    if (!config.enable_prewarm) {
        return true;
    }
    return initializeLocked(m_config);
}

bool VisionLambdaHandler::initializeLocked(const LambdaConfig& config) {
    if (m_initialized.load(std::memory_order_acquire) && m_pipeline) {
        return true;
    }

    m_config = config;
    m_configured.store(true, std::memory_order_release);
    m_config.processing_width = clampDimension(m_config.processing_width, 640);
    m_config.processing_height = clampDimension(m_config.processing_height, 480);
    m_config.confidence_threshold =
        std::clamp(m_config.confidence_threshold, 0.25f, 0.98f);
    m_config.num_threads = std::max(1, m_config.num_threads);

    m_pipeline = std::make_unique<bawal::vision::InspectionPipeline>();
    if (!m_pipeline->initialize(m_config.model_path, m_config.processing_width,
                                m_config.processing_height)) {
        m_pipeline.reset();
        return false;
    }
    m_pipeline->setConfidenceThreshold(m_config.confidence_threshold);

    if (m_config.enable_prewarm) {
        // Touch the ONNX graph and OpenCV buffers once so the first real
        // invocation does not pay the full cold-start allocation cost.
        bawal::vision::InspectionOutput warmed;
        const cv::Mat warm_frame(64, 64, CV_8UC3, cv::Scalar(40, 120, 40));
        (void)m_pipeline->execute(warm_frame, warmed);
    }

    m_initialized.store(true, std::memory_order_release);
    return true;
}

LambdaInspectionResponse VisionLambdaHandler::handleEvent(
    std::string_view event_payload) {
    if (BatchSummaryProcessor::looksLikeBatch(event_payload)) {
        LambdaBatchRequest batch;
        std::string error;
        if (!m_batch_processor.parse(event_payload, batch, error)) {
            const Clock::time_point start = Clock::now();
            return errorResponse(400, std::move(error), start);
        }
        return m_batch_processor.process(batch);
    }

    LambdaEventParser parser;
    LambdaInspectionRequest request;
    if (!parser.parseEvent(event_payload, request)) {
        const Clock::time_point start = Clock::now();
        return errorResponse(
            400,
            "Invalid event: expected image_base64/body, a base64 image, or a "
            "telemetry/log batch",
            start);
    }
    return handleInspectionEvent(request);
}

LambdaInspectionResponse VisionLambdaHandler::handleInspectionEvent(
    const LambdaInspectionRequest& request) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return handleLocked(request);
}

LambdaInspectionResponse VisionLambdaHandler::handleLocked(
    const LambdaInspectionRequest& request) {
    const Clock::time_point start = Clock::now();

    if (request.raw_image_bytes.empty()) {
        return errorResponse(400, "Event did not contain a usable image payload", start);
    }

    if (!m_initialized.load(std::memory_order_acquire) || !m_pipeline) {
        LambdaConfig config = m_config;
        if (config.model_path.empty()) {
            return errorResponse(503, "Lambda vision pipeline is not configured", start);
        }
        config.confidence_threshold = std::clamp(
            request.confidence_threshold > 0.0f ? request.confidence_threshold
                                                : config.confidence_threshold,
            0.25f, 0.98f);
        if (request.processing_width > 0) {
            config.processing_width =
                clampDimension(request.processing_width, config.processing_width);
        }
        if (request.processing_height > 0) {
            config.processing_height =
                clampDimension(request.processing_height, config.processing_height);
        }
        if (!initializeLocked(config)) {
            return errorResponse(500, "Could not initialize the Lambda vision pipeline", start);
        }
    } else if (request.confidence_threshold > 0.0f) {
        m_pipeline->setConfidenceThreshold(request.confidence_threshold);
    }

    bawal::vision::InspectionOutput output;
    if (!m_pipeline->execute(request.raw_image_bytes, output) || !output.is_valid) {
        return errorResponse(422, "Vision pipeline could not process the image", start);
    }

    bawal::server::InspectionResponse dto;
    dto.frame_id = request.frame_id != 0 ? request.frame_id : output.frame_id;
    dto.status = "PROCESSED";
    dto.image_path.clear();
    // The current model does not estimate food surface temperature.
    dto.surface_temp = 0.0;
    dto.device_id = request.device_id;
    const std::string_view architecture = m_pipeline->getModelArchitecture();
    dto.engine_pipeline = architecture.empty() ? "YOLOv8-INT8 + OpenCV HSV"
                                               : std::string(architecture);
    dto.processing_time_ms = output.processing_time_ms;
    dto.message = "Serverless BawalVision inspection completed";

    dto.detections.reserve(output.detections.size());
    for (const auto& detection : output.detections) {
        if (detection.class_name == "no_food" || detection.class_name == "no_model") {
            continue;
        }
        dto.detections.push_back({
            std::string(detection.class_name), detection.confidence,
            {detection.bbox.x, detection.bbox.y,
             detection.bbox.width, detection.bbox.height}});
    }

    if (request.return_annotated && !output.annotated_image.empty()) {
        std::vector<uchar> jpeg;
        const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, 85};
        if (cv::imencode(".jpg", output.annotated_image, jpeg, params)) {
            dto.annotated_image_base64 = encodeBase64(jpeg.data(), jpeg.size());
        }
    }

    LambdaInspectionResponse response;
    response.status_code = 200;
    response.is_success = true;
    response.execution_time_ms = elapsedMs(start);
    if (const auto error = glz::write_json(dto, response.json_payload); error) {
        return errorResponse(500, "Could not serialize inspection result", start);
    }
    return response;
}

void VisionLambdaHandler::runLoop() {
    (void)runLambdaRuntime(*this, m_config);
}

} // namespace bawal::lambda
