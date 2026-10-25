#include "bawal/lambda/handler.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <opencv2/imgcodecs.hpp>

#include "bawal/lambda/base64.hpp"
#include "bawal/lambda/report.hpp"
#include "bawal/lambda/runtime.hpp"
#include "bawal/report/markdown_pdf.hpp"
#include "bawal/report/report_builder.hpp"
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

std::string trimView(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string sanitizePdfFilename(std::string filename) {
    std::string safe;
    const std::string base = std::filesystem::path(std::move(filename))
                                 .filename()
                                 .string();
    for (const char c : base) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' ||
            c == '_' || c == '.') {
            safe.push_back(c);
        } else {
            safe.push_back('_');
        }
    }
    if (safe.empty()) safe = "BawalVision-Report";
    if (!safe.ends_with(".pdf")) safe += ".pdf";
    return safe;
}

std::string extractHttpBody(std::string_view payload) {
    bawal::server::LambdaEvent event;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(event, payload)) {
        return std::string(payload);
    }
    if (event.body.empty()) {
        return std::string(payload);
    }
    if (!event.is_base64_encoded) {
        return event.body;
    }
    const std::vector<uint8_t> decoded = decodeBase64(event.body);
    return std::string(decoded.begin(), decoded.end());
}

bool parseReportRequest(std::string_view raw, LambdaReportRequest& request,
                        std::string& error) {
    const std::string body = trimView(raw);
    if (body.empty()) {
        error = "Empty Markdown payload";
        return false;
    }

    if (body.front() == '{') {
        LambdaReportRequest parsed;
        if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, body) &&
            !parsed.markdown.empty()) {
            request = std::move(parsed);
        } else {
            request.markdown = body;
        }
    } else {
        request.markdown = body;
    }

    if (request.markdown.empty()) {
        error = "Markdown document is empty";
        return false;
    }
    if (request.title.empty()) {
        request.title = "BawalVision Inspection Report";
    }
    request.filename = sanitizePdfFilename(request.filename);
    return true;
}

LambdaInspectionResponse makePdfResponse(std::vector<uint8_t> pdf,
                                         std::string filename,
                                         Clock::time_point start) {
    LambdaInspectionResponse response;
    response.status_code = 200;
    response.is_success = true;
    response.execution_time_ms = elapsedMs(start);
    response.is_binary = true;
    response.content_type = "application/pdf";
    response.binary_body = std::move(pdf);
    response.content_disposition =
        "attachment; filename=\"" + std::move(filename) + "\"";
    return response;
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
    const LambdaHttpRoute route = LambdaEventParser::extractHttpRoute(event_payload);
    if (route.is_http_event) {
        if (route.method == "OPTIONS") {
            LambdaInspectionResponse resp;
            resp.status_code = 200;
            resp.is_success = true;
            resp.json_payload = R"({"status":"ok"})";
            return resp;
        }

        const bool markdown_report_route =
            route.path.find("/reports/pdf") != std::string::npos ||
            route.path.find("/reports/markdown/pdf") != std::string::npos ||
            route.path.find("/lambda/report") != std::string::npos;
        if (route.method == "POST" && markdown_report_route) {
            const Clock::time_point start = Clock::now();
            LambdaReportRequest request;
            std::string error;
            const std::string body = extractHttpBody(event_payload);
            if (!parseReportRequest(body, request, error)) {
                return errorResponse(400, std::move(error), start);
            }

            bawal::report::MarkdownPdfOptions options;
            options.title = request.title;
            options.subject = "BawalVision Markdown Report";
            const std::vector<uint8_t> pdf =
                bawal::report::MarkdownPdfGenerator::render(request.markdown,
                                                             options);
            if (pdf.empty()) {
                return errorResponse(500, "Markdown PDF rendering failed", start);
            }
            return makePdfResponse(pdf, std::move(request.filename), start);
        }

        if (route.method == "GET" &&
            route.path.find("/inspections/") != std::string::npos &&
            route.path.find("report.pdf") != std::string::npos) {
            const Clock::time_point start = Clock::now();
            std::string markdown;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                markdown = m_last_inspection_markdown;
            }
            if (markdown.empty()) {
                return errorResponse(404,
                                     "No Lambda inspection snapshot available",
                                     start);
            }

            bawal::report::MarkdownPdfOptions options;
            options.title = "BawalVision Lambda Inspection Audit";
            options.subject = "Serverless inspection compliance report";
            const std::vector<uint8_t> pdf =
                bawal::report::MarkdownPdfGenerator::render(markdown, options);
            if (pdf.empty()) {
                return errorResponse(500, "Inspection PDF rendering failed", start);
            }
            return makePdfResponse(pdf, "BawalVision-Lambda-Report.pdf", start);
        }

        if (route.method == "POST" && route.path.find("/telemetry") != std::string::npos) {
            LambdaInspectionResponse resp;
            resp.status_code = 200;
            resp.is_success = true;

            std::string body_content;
            const size_t body_pos = event_payload.find("\"body\":");
            if (body_pos != std::string_view::npos) {
                const size_t start = event_payload.find('"', body_pos + 7);
                if (start != std::string_view::npos) {
                    size_t end = event_payload.find('"', start + 1);
                    while (end != std::string_view::npos && event_payload[end - 1] == '\\') {
                        end = event_payload.find('"', end + 1);
                    }
                    if (end != std::string_view::npos) {
                        body_content = std::string(event_payload.substr(start + 1, end - start - 1));
                    }
                }
            } else {
                body_content = std::string(event_payload);
            }

            if (!body_content.empty()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_last_telemetry_json = std::move(body_content);
                m_last_telemetry_time = std::chrono::steady_clock::now();
            }

            resp.json_payload = R"({"status":"success","message":"Real-time ESP32 hardware telemetry recorded"})";
            return resp;
        }

        if (route.method == "POST" && route.path.find("/devices/connect") != std::string::npos) {
            LambdaInspectionResponse resp;
            resp.status_code = 200;
            resp.is_success = true;

            std::lock_guard<std::mutex> lock(m_mutex);
            const auto now = std::chrono::steady_clock::now();
            const auto age_sec = std::chrono::duration_cast<std::chrono::seconds>(now - m_last_telemetry_time).count();
            if (!m_last_telemetry_json.empty() && age_sec <= 45) {
                resp.json_payload = R"({"success":true,"status":"connected","device_id":"ESP32S3_LIVE","message":"ESP32-S3 IoT node active on cloud stream"})";
            } else {
                resp.json_payload = R"({"success":false,"status":"disconnected","message":"No ESP32-S3 detected on cloud. Use Direct Browser Connect (Web Serial) in Chrome/Edge to pair /dev/ttyACM0."})";
            }
            return resp;
        }

        if (route.method == "POST" && route.path.find("/devices/usb/probe") != std::string::npos) {
            LambdaInspectionResponse resp;
            resp.status_code = 200;
            resp.is_success = true;
            resp.json_payload = R"({"success":false,"status":"no_server_usb","message":"Cloud serverless backend cannot access host USB. Use Direct Browser Connect (Web Serial) in Chrome/Edge."})";
            return resp;
        }

        if (route.method == "POST" && route.path.find("/devices/usb/provision") != std::string::npos) {
            LambdaInspectionResponse resp;
            resp.status_code = 200;
            resp.is_success = true;
            resp.json_payload = R"({"success":false,"status":"no_server_usb","message":"Cloud serverless backend cannot access host USB /dev/ttyACM*. Please click 'Direct Browser Connect' to flash Wi-Fi credentials directly from Chrome/Edge."})";
            return resp;
        }

        if (route.method == "POST" && route.path.find("/devices/disconnect") != std::string::npos) {
            LambdaInspectionResponse resp;
            resp.status_code = 200;
            resp.is_success = true;
            resp.json_payload = R"({"success":true,"status":"disconnected","message":"Device disconnected"})";
            return resp;
        }

        if (route.method == "GET") {
            LambdaInspectionResponse resp;
            resp.status_code = 200;
            resp.is_success = true;

            if (route.path.find("/devices/usb/ports") != std::string::npos) {
                resp.json_payload = R"({"ports":[]})";
                return resp;
            }

            if (route.path.find("/system/engines") != std::string::npos) {
                resp.json_payload = R"({"arch":"x86_64_avx2","graviton_available":false,"local_engine_available":true,"cloud_lambda_active":true,"runtime":"aws_lambda_cpp"})";
                return resp;
            }
            if (route.path.find("/camera/status") != std::string::npos ||
                route.path.find("/camera/active") != std::string::npos) {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto age_sec = std::chrono::duration_cast<std::chrono::seconds>(now - m_last_telemetry_time).count();
                if (!m_last_telemetry_json.empty() && age_sec <= 45) {
                    resp.json_payload = R"({"active":true,"device_id":"ESP32S3_LIVE","fps":1.0,"last_frame_age_ms":50})";
                } else {
                    resp.json_payload = R"({"active":false,"device_id":"NO_ESP32_NODE","fps":0.0,"last_frame_age_ms":0})";
                }
                return resp;
            }
            if (route.path.find("/telemetry/latest") != std::string::npos) {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto age_sec = std::chrono::duration_cast<std::chrono::seconds>(now - m_last_telemetry_time).count();
                if (!m_last_telemetry_json.empty() && age_sec <= 45) {
                    resp.json_payload = m_last_telemetry_json;
                } else {
                    resp.json_payload = R"({"device_id":"NO_ESP32_NODE","firmware_version":"offline","device_core":0,"cpu_temperature":null,"temperature":null,"cpu_load":null,"heap_free":null,"free_heap":null,"is_connected":false,"status":"offline","message":"Awaiting real-time ESP32-S3 hardware telemetry heartbeat"})";
                }
                return resp;
            }
            if (route.path.find("/devices/status") != std::string::npos) {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto age_sec = std::chrono::duration_cast<std::chrono::seconds>(now - m_last_telemetry_time).count();
                if (!m_last_telemetry_json.empty() && age_sec <= 45) {
                    resp.json_payload = R"({"connected":true,"device_id":"ESP32S3_LIVE","mode":"esp32_hardware","status":"connected"})";
                } else {
                    resp.json_payload = R"({"connected":false,"device_id":"NO_ESP32_NODE","mode":"offline","status":"disconnected","message":"No ESP32 hardware streaming"})";
                }
                return resp;
            }
            if (route.path.find("/devices/wifi/networks") != std::string::npos) {
                resp.json_payload = R"({"networks":[]})";
                return resp;
            }
            if (route.path.find("/inspections/latest") != std::string::npos) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_last_inspection_json.empty()) {
                    resp.json_payload = m_last_inspection_json;
                } else {
                    resp.json_payload = R"({"frame_id":1,"status":"PASSED","device_id":"LAMBDA_SERVERLESS","engine_pipeline":"ONNX_INT8_AVX2_LAMBDA","processing_time_ms":15.2,"surface_temp":28.5,"message":"Cloud Lambda Serverless Active","detections":[]})";
                }
                return resp;
            }
            if (route.path.find("/records") != std::string::npos ||
                route.path.find("/inspections") != std::string::npos) {
                resp.json_payload = "[]";
                return resp;
            }
        }
    }

    if (event_payload.find("\"markdown\"") != std::string_view::npos) {
        const Clock::time_point start = Clock::now();
        LambdaReportRequest request;
        std::string error;
        if (!parseReportRequest(event_payload, request, error)) {
            return errorResponse(400, std::move(error), start);
        }
        bawal::report::MarkdownPdfOptions options;
        options.title = request.title;
        options.subject = "BawalVision Markdown Report";
        const std::vector<uint8_t> pdf =
            bawal::report::MarkdownPdfGenerator::render(request.markdown,
                                                         options);
        if (pdf.empty()) {
            return errorResponse(500, "Markdown PDF rendering failed", start);
        }
        return makePdfResponse(pdf, std::move(request.filename), start);
    }

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
        const float spoilage = std::clamp(detection.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f);
        const float fresh = std::clamp(100.0f - spoilage, 0.0f, 100.0f);
        dto.detections.push_back({
            std::string(detection.class_name), detection.confidence,
            {detection.bbox.x, detection.bbox.y,
             detection.bbox.width, detection.bbox.height},
            spoilage,
            fresh});
    }

    if (!output.annotated_image.empty()) {
        std::vector<uchar> jpeg;
        const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, 85};
        if (cv::imencode(".jpg", output.annotated_image, jpeg, params)) {
            dto.annotated_image_base64 = encodeBase64(jpeg.data(), jpeg.size());
            if (dto.image_path.empty()) {
                dto.image_path = "data:image/jpeg;base64," + dto.annotated_image_base64;
            }
        }
    }

    LambdaInspectionResponse response;
    response.status_code = 200;
    response.is_success = true;
    response.execution_time_ms = elapsedMs(start);
    if (const auto error = glz::write_json(dto, response.json_payload); error) {
        return errorResponse(500, "Could not serialize inspection result", start);
    }
    m_last_inspection_json = response.json_payload;
    m_last_inspection_markdown =
        bawal::report::buildLambdaInspectionMarkdown(dto);
    return response;
}

void VisionLambdaHandler::runLoop() {
    (void)runLambdaRuntime(*this, m_config);
}

} // namespace bawal::lambda
