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
#include <fstream>
#include <omp.h>

#include "bawal/lambda/base64.hpp"
#include "bawal/lambda/report.hpp"
#include "bawal/lambda/runtime.hpp"
#include "bawal/report/markdown_pdf.hpp"
#include "bawal/report/report_builder.hpp"
#include "bawal/server/inspection_dto.hpp"
#include "bawal/vision/pipeline.hpp"
#include "bawal/vision/image/batch_inspector.hpp"

namespace {

using bawal::lambda::decodeBase64;
using bawal::lambda::kMaxMarkdownBytes;
using bawal::lambda::LambdaInspectionResponse;
using bawal::lambda::LambdaReportRequest;

using Clock = std::chrono::steady_clock;

constexpr auto kInspectionTtl = std::chrono::seconds(60);

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

bool constantTimeEquals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        difference |= static_cast<unsigned char>(lhs[i] ^ rhs[i]);
    }
    return difference == 0;
}

std::string noInspectionPayload() {
    return R"({"frame_id":0,"status":"NO_DATA","device_id":"LAMBDA_SERVERLESS","engine_pipeline":"ONNX_INT8_LAMBDA","processing_time_ms":0,"surface_temp":0,"message":"No live inspection snapshot available","is_live":false,"detections":[]})";
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
    if (request.markdown.size() > kMaxMarkdownBytes) {
        error = "Markdown payload exceeds the 2 MiB limit";
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

std::string serializeBatchSummaryJson(const bawal::vision::image::BatchReportSummary& summary) {
    glz::json_t::object_t root;
    root["status"] = "success";
    root["batch_id"] = summary.batch_id;
    root["inspection_type"] = summary.inspection_type;
    root["generated_at"] = summary.generated_at;
    root["model_arch"] = summary.model_arch;
    root["engine"] = summary.engine;
    root["omp_threads"] = static_cast<double>(summary.omp_threads);
    root["total_trays"] = static_cast<double>(summary.total_images);
    root["accepted"] = static_cast<double>(summary.accepted_count);
    root["rejected"] = static_cast<double>(summary.rejected_count);
    root["processing_time_ms"] = summary.total_processing_time_ms;
    root["throughput_trays_per_sec"] = summary.throughput_images_per_sec;
    const double pass_rate = summary.total_images > 0
        ? static_cast<double>(summary.accepted_count) / static_cast<double>(summary.total_images) * 100.0
        : 0.0;
    root["pass_rate"] = pass_rate;
    root["quality_index"] = static_cast<double>(summary.quality.quality_index);
    root["avg_similarity"] = static_cast<double>(summary.quality.avg_similarity);
    root["avg_quality"] = static_cast<double>(summary.quality.avg_quality);
    root["min_quality"] = static_cast<double>(summary.quality.min_quality);
    root["max_quality"] = static_cast<double>(summary.quality.max_quality);
    root["quality_stddev"] = static_cast<double>(summary.quality.stddev_quality);
    root["total_detections"] = static_cast<double>(summary.quality.total_detections);
    root["pass_detections"] = static_cast<double>(summary.quality.pass_detections);
    root["review_detections"] = static_cast<double>(summary.quality.review_detections);
    root["reject_detections"] = static_cast<double>(summary.quality.reject_detections);
    root["report_checksum"] = summary.report_checksum;
    root["persisted_to_postgres"] = false;
    root["pdf_download_url"] = "/api/v1/inspections/batch/" + summary.batch_id + "/report.pdf";

    glz::json_t::array_t histogram;
    for (const uint32_t val : summary.quality_histogram) {
        histogram.emplace_back(static_cast<double>(val));
    }
    root["quality_histogram"] = std::move(histogram);

    glz::json_t::array_t class_breakdown;
    for (const auto& cls : summary.class_breakdown) {
        glz::json_t::object_t item;
        item["class_name"] = std::string(bawal::vision::image::fixedStringView(cls.class_name));
        item["detections"] = static_cast<double>(cls.detections);
        item["pass_count"] = static_cast<double>(cls.pass_count);
        item["review_count"] = static_cast<double>(cls.review_count);
        item["reject_count"] = static_cast<double>(cls.reject_count);
        item["avg_similarity"] = static_cast<double>(cls.avg_similarity);
        item["avg_quality"] = static_cast<double>(cls.avg_quality);
        class_breakdown.emplace_back(std::move(item));
    }
    root["class_breakdown"] = std::move(class_breakdown);

    glz::json_t::array_t items;
    for (const auto& it : summary.items) {
        glz::json_t::object_t item;
        item["index"] = static_cast<double>(it.index);
        item["filename"] = it.filename;
        item["accepted"] = it.accepted;
        item["surface_temp"] = static_cast<double>(it.surface_temp);
        item["t_safe_hours"] = static_cast<double>(it.t_safe_hours);
        item["avg_similarity"] = static_cast<double>(it.avg_similarity);
        item["avg_quality"] = static_cast<double>(it.avg_quality);
        item["pass_detections"] = static_cast<double>(it.pass_detections);
        item["review_detections"] = static_cast<double>(it.review_detections);
        item["reject_detections"] = static_cast<double>(it.reject_detections);
        item["annotated_url"] = "/api/v1/inspections/batch/" + summary.batch_id +
                                "/annotated/" + std::to_string(it.index);

        if (!it.annotated_image_path.empty() && std::filesystem::is_regular_file(it.annotated_image_path)) {
            std::ifstream img_stream(it.annotated_image_path, std::ios::binary);
            if (img_stream) {
                std::vector<uint8_t> img_bytes((std::istreambuf_iterator<char>(img_stream)),
                                                std::istreambuf_iterator<char>());
                if (!img_bytes.empty()) {
                    item["annotated_data_url"] = "data:image/jpeg;base64," + bawal::lambda::encodeBase64(img_bytes);
                }
            }
        }

        glz::json_t::array_t detections;
        for (const auto& det : it.detections) {
            glz::json_t::object_t d;
            d["class_name"] = std::string(det.class_name);
            d["confidence"] = static_cast<double>(det.confidence);
            d["freshness"] = static_cast<double>(bawal::vision::image::freshnessScore(det) / 100.0f);
            d["status"] = std::string(bawal::vision::image::qualityStatusName(
                bawal::vision::image::classifyQuality(det)));

            glz::json_t::array_t bbox;
            bbox.emplace_back(static_cast<double>(det.bbox.x));
            bbox.emplace_back(static_cast<double>(det.bbox.y));
            bbox.emplace_back(static_cast<double>(det.bbox.width));
            bbox.emplace_back(static_cast<double>(det.bbox.height));
            d["bbox"] = std::move(bbox);
            detections.emplace_back(std::move(d));
        }
        item["detections"] = std::move(detections);
        items.emplace_back(std::move(item));
    }
    root["items"] = std::move(items);

    std::string out;
    if (const auto err = glz::write_json(root, out); err) {
        return R"({"error":"Failed to serialize batch inspection summary"})";
    }
    return out;
}

} // namespace

namespace bawal::lambda {

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
    const Clock::time_point request_start = Clock::now();
    if (event_payload.size() > kMaxEventPayloadBytes) {
        return errorResponse(413, "Event payload exceeds the 6 MiB limit",
                             request_start);
    }

    const LambdaHttpRoute route = LambdaEventParser::extractHttpRoute(event_payload);

    // Unauthenticated Health Check for CI RIE, API Gateway, and monitoring probes
    if (route.is_http_event &&
        (route.path.find("/lambda/health") != std::string::npos ||
         route.path.find("/health") != std::string::npos)) {
        LambdaInspectionResponse resp;
        resp.status_code = 200;
        resp.is_success = true;
        resp.json_payload = R"({"status":"ok","runtime":"bawal::lambda","ready":true,"arch":"aarch64_graviton3"})";
        return resp;
    }

    if (route.is_http_event && route.method != "OPTIONS" &&
        m_config.enforce_http_auth &&
        !LambdaEventParser::isIamAuthorized(event_payload)) {
        if (m_config.auth_token.empty()) {
            return errorResponse(503, "Lambda auth token is not configured",
                                 request_start);
        }
        const std::string authorization = trimView(
            LambdaEventParser::extractHeader(event_payload, "authorization"));
        const std::string header_token = trimView(
            LambdaEventParser::extractHeader(event_payload, "x-bawal-token"));
        const std::string expected = "Bearer " + m_config.auth_token;
        if (!constantTimeEquals(authorization, expected) &&
            !constantTimeEquals(header_token, m_config.auth_token)) {
            return errorResponse(401, "Unauthorized", request_start);
        }
    }

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
            if (body.size() > kMaxMarkdownBytes) {
                return errorResponse(413, "Markdown payload exceeds the 2 MiB limit",
                                     start);
            }
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
            if (pdf.size() > kMaxBinaryResponseBytes) {
                return errorResponse(413, "PDF report exceeds the 4 MiB response limit",
                                     start);
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
                if (!m_last_inspection_markdown.empty() &&
                    m_last_inspection_time != Clock::time_point{} &&
                    Clock::now() - m_last_inspection_time <= kInspectionTtl) {
                    markdown = m_last_inspection_markdown;
                }
            }
            if (markdown.empty()) {
                return errorResponse(404,
                                     "No live Lambda inspection snapshot available",
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
            if (pdf.size() > kMaxBinaryResponseBytes) {
                return errorResponse(413, "PDF report exceeds the 4 MiB response limit",
                                     start);
            }
            return makePdfResponse(pdf, "BawalVision-Lambda-Report.pdf", start);
        }

        if (route.method == "POST" && route.path.find("/telemetry") != std::string::npos) {
            const Clock::time_point start = Clock::now();
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

            if (body_content.size() > kMaxTelemetryBytes) {
                return errorResponse(413, "Telemetry payload exceeds the 64 KiB limit",
                                     start);
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
            resp.status_code = 501;
            resp.is_success = false;
            resp.json_payload = R"({"success":false,"status":"unsupported","message":"Serverless Lambda cannot disconnect local hardware; use the Local Agent."})";
            return resp;
        }

        if (route.method == "POST" && route.path.find("/inspections/batch-zip") != std::string::npos) {
            const Clock::time_point start = Clock::now();
            const std::vector<uint8_t> zip_bytes = LambdaEventParser::extractBodyBytes(event_payload);

            if (zip_bytes.empty()) {
                return errorResponse(400, "Empty payload body. Please upload a valid ZIP archive.", start);
            }
            if (zip_bytes.size() > 15 * 1024 * 1024) {
                return errorResponse(413, "ZIP archive exceeds maximum limit of 15 MB.", start);
            }
            if (zip_bytes.size() < 4 ||
                zip_bytes[0] != 0x50 || zip_bytes[1] != 0x4B ||
                (zip_bytes[2] != 0x03 && zip_bytes[2] != 0x05) ||
                (zip_bytes[3] != 0x04 && zip_bytes[3] != 0x06)) {
                return errorResponse(400, "Invalid ZIP archive: missing ZIP magic header", start);
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_initialized.load(std::memory_order_acquire) || !m_pipeline) {
                LambdaConfig config = m_config;
                if (config.model_path.empty()) {
                    config.model_path = "/var/task/models/yolov8s_int8.onnx";
                }
                if (!initializeLocked(config)) {
                    return errorResponse(503, "Lambda batch vision engine not initialized (ONNX model unavailable)", start);
                }
            }

            std::filesystem::path template_path = "/var/task/templates/batch_report.tex.in";
            if (!std::filesystem::exists(template_path)) {
                template_path = "./services/templates/batch_report.tex.in";
            }
            if (!std::filesystem::exists(template_path)) {
                template_path = "./templates/batch_report.tex.in";
            }

            const bawal::vision::image::BatchReportSummary summary =
                bawal::vision::image::BatchInspector::processZipBuffer(
                    zip_bytes.data(),
                    zip_bytes.size(),
                    m_pipeline->getClassifier(),
                    template_path,
                    nullptr);

            if (summary.total_images == 0) {
                return errorResponse(400, "Failed to extract valid image files (.jpg/.png) from ZIP archive", start);
            }

            LambdaInspectionResponse resp;
            resp.status_code = 200;
            resp.is_success = true;
            resp.content_type = "application/json";
            resp.execution_time_ms = elapsedMs(start);
            resp.json_payload = serializeBatchSummaryJson(summary);
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
#if defined(__aarch64__) || defined(_M_ARM64)
                resp.json_payload = R"({"arch":"aarch64_graviton3","graviton_available":true,"local_engine_available":false,"cloud_lambda_active":true,"runtime":"aws_lambda_cpp"})";
#else
                resp.json_payload = R"({"arch":"x86_64_simd","graviton_available":true,"local_engine_available":false,"cloud_lambda_active":true,"runtime":"aws_lambda_cpp"})";
#endif
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
                resp.json_payload = R"({"frame_id":0,"status":"NO_DATA","capability":false,"device_id":"LAMBDA_SERVERLESS","engine_pipeline":"ONNX_INT8_LAMBDA","processing_time_ms":0,"surface_temp":0,"message":"Stateless Lambda: live inspection state resides on the local agent","is_live":false,"detections":[]})";
                return resp;
            }
            if (route.path.find("/records") != std::string::npos ||
                route.path.find("/inspections") != std::string::npos) {
                const char* s3_bucket = std::getenv("BAWAL_S3_AUDIT_BUCKET");
                const char* ddb_table = std::getenv("BAWAL_DYNAMODB_TABLE");
                if ((s3_bucket == nullptr || *s3_bucket == '\0') &&
                    (ddb_table == nullptr || *ddb_table == '\0')) {
                    resp.json_payload = R"({"status":"NO_DATA","capability":false,"records":[],"message":"Cloud audit storage (S3/DynamoDB) is not configured. Historical inspection records reside on the local agent."})";
                } else {
                    resp.json_payload = R"({"status":"OK","capability":true,"records":[]})";
                }
                return resp;
            }

            if (route.path.find("/inspections/batch/") != std::string::npos &&
                route.path.find("/report.pdf") != std::string::npos) {
                const Clock::time_point start = Clock::now();
                const size_t bpos = route.path.find("/inspections/batch/");
                std::string batch_id;
                if (bpos != std::string::npos) {
                    const size_t start_id = bpos + 19;
                    const size_t end_id = route.path.find('/', start_id);
                    if (end_id != std::string::npos) {
                        batch_id = route.path.substr(start_id, end_id - start_id);
                    }
                }
                const std::filesystem::path pdf_file =
                    std::filesystem::path("/tmp/bawal_batches") / batch_id / "master_report.pdf";
                if (std::filesystem::is_regular_file(pdf_file)) {
                    std::ifstream in(pdf_file, std::ios::binary);
                    std::vector<uint8_t> pdf_bytes((std::istreambuf_iterator<char>(in)),
                                                   std::istreambuf_iterator<char>());
                    return makePdfResponse(std::move(pdf_bytes), "BawalVision-Batch-" + batch_id + ".pdf", start);
                }
                return errorResponse(404, "Batch master report PDF not found", start);
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
        if (pdf.size() > kMaxBinaryResponseBytes) {
            return errorResponse(413, "PDF report exceeds the 4 MiB response limit",
                                 start);
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
        LambdaInspectionResponse response = m_batch_processor.process(batch);
        if (response.json_payload.size() > kMaxJsonResponseBytes) {
            return errorResponse(413, "Batch response exceeds the 4 MiB limit",
                                 Clock::now());
        }
        return response;
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
    if (request.raw_image_bytes.size() > kMaxImageBytes ||
        !validateImagePayload(request.raw_image_bytes)) {
        return errorResponse(413, "Image payload exceeds the Lambda image limits",
                             start);
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

    const bool want_annotated = request.return_annotated || m_config.return_annotated;
    if (want_annotated && !output.annotated_image.empty()) {
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
    if (response.json_payload.size() > kMaxJsonResponseBytes &&
        !dto.annotated_image_base64.empty()) {
        dto.annotated_image_base64.clear();
        if (const auto error = glz::write_json(dto, response.json_payload); error) {
            return errorResponse(500, "Could not serialize inspection result", start);
        }
    }
    if (response.json_payload.size() > kMaxJsonResponseBytes) {
        return errorResponse(413, "Inspection response exceeds the 4 MiB limit", start);
    }
    // Lambda is stateless: detection results are returned immediately to the local agent
    return response;
}

void VisionLambdaHandler::runLoop() {
    (void)runLambdaRuntime(*this, m_config);
}

} // namespace bawal::lambda
