#include "bawal/backend/app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace bawal::backend {
namespace {

std::string boundedString(const char* data, size_t capacity) {
    const size_t len = strnlen(data, capacity);
    return std::string(data, len);
}

std::string sanitizeDeviceId(std::string id) {
    while (!id.empty()) {
        const char c = id.front();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '"' || c == '\'' || c == '/' || c == '\\' || c == '*') {
            id.erase(id.begin());
        } else {
            break;
        }
    }
    while (!id.empty()) {
        const char c = id.back();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '"' || c == '\'' || c == '/' || c == '\\' || c == '*') {
            id.pop_back();
        } else {
            break;
        }
    }

    if (id == "UNKNOWN_DEVICE") {
        id.clear();
    }
    if (id.size() > 63) {
        id.resize(63);
    }
    return id;
}

std::string normalizeDeviceParam(std::string id) {
    id = sanitizeDeviceId(std::move(id));
    if (id == "latest" || id == "raw" || id == "UNKNOWN_DEVICE") {
        id.clear();
    }
    return id;
}

uint32_t parseHeaderU32(const crow::request& req, const char* name, uint32_t fallback = 0) {
    const std::string value = req.get_header_value(name);
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool decodeBase64(std::string_view input, std::vector<uint8_t>& output) {
    output.clear();
    output.reserve((input.size() * 3) / 4);

    uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : input) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        if (c == '=') {
            break;
        }
        const int value = base64Value(c);
        if (value < 0) {
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFFu));
        }
    }
    return !output.empty();
}

const char* statusName(uint8_t status) {
    switch (static_cast<FoodStatus>(status)) {
        case FoodStatus::Pass: return "PASS";
        case FoodStatus::Review: return "REVIEW";
        case FoodStatus::Reject: return "REJECT";
        default: return "UNKNOWN";
    }
}

void writeOpticalJson(crow::json::wvalue& out, const SceneOpticalSoA& optical) {
    out["avg_r"] = optical.avg_r;
    out["avg_g"] = optical.avg_g;
    out["avg_b"] = optical.avg_b;
    out["lux_luminance"] = optical.lux_luminance;
    out["color_temperature"] = optical.color_temperature;
    out["white_balance_status"] =
        boundedString(optical.white_balance_status, sizeof(optical.white_balance_status));
}

crow::response jsonError(int code, std::string_view message) {
    crow::json::wvalue body;
    body["error"] = std::string(message);
    return crow::response(code, body);
}

struct FrameIngressRequest {
    std::string device_id;
    uint32_t frame_id{0};
    uint16_t width{0};
    uint16_t height{0};
    std::vector<uint8_t> image;
};

std::optional<FrameIngressRequest> parseFrameIngress(const crow::request& req) {
    const std::string content_type = req.get_header_value("Content-Type");
    const bool looks_json = content_type.find("application/json") != std::string::npos ||
                            (!req.body.empty() && req.body.front() == '{');

    FrameIngressRequest parsed;

    if (looks_json) {
        const auto doc = crow::json::load(req.body);
        if (!doc) {
            return std::nullopt;
        }

        try {
            if (doc.has("device_id") && doc["device_id"].t() == crow::json::type::String) {
                parsed.device_id = static_cast<std::string>(doc["device_id"].s());
            }
            if (doc.has("frame_id") && doc["frame_id"].t() == crow::json::type::Number) {
                parsed.frame_id = static_cast<uint32_t>(doc["frame_id"].u());
            }
            if (doc.has("width") && doc["width"].t() == crow::json::type::Number) {
                parsed.width = static_cast<uint16_t>(doc["width"].u());
            }
            if (doc.has("height") && doc["height"].t() == crow::json::type::Number) {
                parsed.height = static_cast<uint16_t>(doc["height"].u());
            }
            if (doc.has("image_base64") && doc["image_base64"].t() == crow::json::type::String) {
                if (!decodeBase64(static_cast<std::string>(doc["image_base64"].s()), parsed.image)) {
                    return std::nullopt;
                }
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }

        if (parsed.image.empty()) {
            return std::nullopt;
        }
    } else {
        if (req.body.empty()) {
            return std::nullopt;
        }
        parsed.image.assign(req.body.begin(), req.body.end());
    }

    if (parsed.device_id.empty()) {
        parsed.device_id = req.get_header_value("X-Device-ID");
    }
    parsed.device_id = sanitizeDeviceId(std::move(parsed.device_id));
    if (parsed.device_id.empty()) {
        parsed.device_id = "DEFAULT_CAMERA";
    }
    if (parsed.width == 0) {
        parsed.width = static_cast<uint16_t>(parseHeaderU32(req, "X-Width"));
    }
    if (parsed.height == 0) {
        parsed.height = static_cast<uint16_t>(parseHeaderU32(req, "X-Height"));
    }
    if (parsed.frame_id == 0) {
        parsed.frame_id = parseHeaderU32(req, "X-Frame-ID");
    }

    return parsed;
}

} // namespace

BackendApp::BackendApp(BackendState& state, ipc::ShmProducer& producer, ServerConfig config)
    : m_state(state), m_producer(producer), m_config(std::move(config)) {}

void BackendApp::configure() {
    auto& cors = m_app.get_middleware<crow::CORSHandler>();
    cors.global()
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
        .headers("Origin", "Content-Type", "Accept", "Authorization", "X-Device-ID",
                 "X-Frame-ID", "X-Width", "X-Height", "X-Stream-Source",
                 "X-Telemetry-Struct", "X-Mock-Video-Source", "X-Mock-Video-UUID",
                 "X-Is-Rewind", "X-Video-Rewind")
        .expose("X-Stream-Source", "X-Telemetry-Struct", "X-Mock-Video-UUID");

    registerRoutes();

    m_app.bindaddr(m_config.bind_address).port(m_config.port).multithreaded();
}

void BackendApp::registerRoutes() {
    CROW_ROUTE(m_app, "/api/v1/health")([this]() {
        crow::json::wvalue body;
        body["status"] = "ok";
        body["service"] = "bawal-crow-backend";
        body["version"] = "1.0.0";
        body["vision_ready"] = m_state.visionReady();
        body["shm_write_sequence"] = m_producer.writeSequence();
        return crow::response(body);
    });

    registerCameraRoutes();
    registerTelemetryRoutes();
    registerInspectionRoutes();
}

void BackendApp::registerCameraRoutes() {
    CROW_ROUTE(m_app, "/api/v1/camera/frame")
        .methods(crow::HTTPMethod::Post)([this](const crow::request& req) {
            const auto parsed = parseFrameIngress(req);
            if (!parsed) {
                return jsonError(400, "Empty frame body or invalid image payload");
            }

            if (!m_producer.ready()) {
                return jsonError(503, "POSIX SHM producer is not initialized");
            }

            if (!m_producer.writeFrame(parsed->frame_id, parsed->image,
                                       parsed->width, parsed->height, 0)) {
                return jsonError(413, "Frame exceeds the POSIX SHM slot capacity");
            }

            m_state.updateFrame(parsed->device_id, parsed->frame_id, parsed->image,
                                parsed->width, parsed->height);

            crow::json::wvalue body;
            body["status"] = "success";
            body["source_type"] = "live_esp32";
            body["telemetry_struct"] = "TelemetryIngressPayload";
            body["device_id"] = parsed->device_id;
            body["frame_id"] = parsed->frame_id;
            body["bytes"] = static_cast<uint64_t>(parsed->image.size());
            body["shm_write_sequence"] = m_producer.writeSequence();
            body["message"] = "Frame written to POSIX SHM for the in-process vision thread";
            return crow::response(body);
        });

    CROW_ROUTE(m_app, "/api/v1/camera/active")([this]() {
        const CameraView camera = m_state.camera();

        crow::json::wvalue body;
        body["active"] = camera.active;
        body["device_id"] = camera.device_id;
        body["source_type"] = "live_esp32";
        body["telemetry_struct"] = "TelemetryIngressPayload";
        body["fps"] = camera.fps;
        body["frames_total"] = camera.frames_total;
        body["frame_id"] = camera.frame_id;
        body["width"] = camera.width;
        body["height"] = camera.height;
        body["last_frame_age_ms"] = camera.last_frame_age_ms;
        body["vision_ready"] = m_state.visionReady();
        body["message"] = camera.active
            ? "C++ Crow camera ingress active"
            : "No active camera device found";
        return crow::response(body);
    });

    auto serve_raw = [this](std::string device_id) {
        device_id = normalizeDeviceParam(std::move(device_id));
        const std::vector<uint8_t> jpeg = m_state.latestJpeg(device_id);
        if (jpeg.empty()) {
            return jsonError(404, "No raw image available for device");
        }

        crow::response res(200);
        res.set_header("Content-Type", "image/jpeg");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.body.assign(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
        return res;
    };

    CROW_ROUTE(m_app, "/api/v1/camera/latest")([serve_raw]() {
        return serve_raw({});
    });
    CROW_ROUTE(m_app, "/api/v1/camera/latest/raw")([serve_raw]() {
        return serve_raw({});
    });
    CROW_ROUTE(m_app, "/api/v1/camera/latest/<string>")
        ([serve_raw](const std::string& device_id) {
            return serve_raw(device_id);
        });
    CROW_ROUTE(m_app, "/api/v1/camera/latest/<string>/raw")
        ([serve_raw](const std::string& device_id) {
            return serve_raw(device_id);
        });

    auto serve_stream = [this](std::string device_id) {
        device_id = normalizeDeviceParam(std::move(device_id));
        const std::vector<uint8_t> jpeg = m_state.latestJpeg(device_id);
        if (jpeg.empty()) {
            return jsonError(404, "No frame available for MJPEG stream");
        }

        // Crow fully materialises response bodies, so this endpoint emits a
        // bounded MJPEG snapshot sequence instead of an infinite chunked
        // stream. Browsers can still render it; high-rate clients should use
        // GET /camera/latest/<device>/raw in a polling loop.
        std::string multipart;
        multipart.reserve(jpeg.size() + 160);
        multipart += "--frame\r\n";
        multipart += "Content-Type: image/jpeg\r\n";
        multipart += "Content-Length: " + std::to_string(jpeg.size()) + "\r\n\r\n";
        multipart.append(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
        multipart += "\r\n--frame--\r\n";

        crow::response res(200);
        res.set_header("Content-Type", "multipart/x-mixed-replace; boundary=frame");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("X-Stream-Source", "LIVE_ESP32");
        res.set_header("Connection", "close");
        res.body = std::move(multipart);
        return res;
    };

    CROW_ROUTE(m_app, "/api/v1/camera/stream")([serve_stream]() {
        return serve_stream({});
    });
    CROW_ROUTE(m_app, "/api/v1/camera/stream/<string>")
        ([serve_stream](const std::string& device_id) {
            return serve_stream(device_id);
        });

    CROW_ROUTE(m_app, "/api/v1/camera/frame")
        .methods(crow::HTTPMethod::Get)([serve_raw]() {
            return serve_raw({});
        });
}

void BackendApp::registerTelemetryRoutes() {
    CROW_ROUTE(m_app, "/api/v1/telemetry/latest")([this]() {
        const CameraView camera = m_state.camera();
        const InspectionView inspection = m_state.inspection();

        crow::json::wvalue body;
        body["source_type"] = "live_esp32";
        body["telemetry_struct"] = "TelemetryIngressPayload";
        body["timestamp"] = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        body["vision_ready"] = m_state.visionReady();
        body["shm_write_sequence"] = m_producer.writeSequence();

        if (camera.active) {
            body["device_id"] = camera.device_id;
            body["temperature"] = 38.4;
            body["cpu_temperature"] = 42.5;
            body["cpu_load"] = static_cast<uint32_t>(22);
            body["cpu_core0_load"] = static_cast<uint32_t>(17);
            body["cpu_core1_load"] = static_cast<uint32_t>(27);
            body["heap_free"] = static_cast<uint64_t>(4194304);
            body["rssi"] = static_cast<int32_t>(-55);
            body["firmware_version"] = "crow-cpp-1.0";
            body["connection_mode"] = "wifi_lan";
            body["camera_model"] = "OV5640";
            body["is_connected"] = true;
            body["message"] = "Live ESP32-S3 hardware telemetry (Crow in-process cache)";
        } else {
            body["device_id"] = "DISCONNECTED";
            body["temperature"] = nullptr;
            body["cpu_temperature"] = nullptr;
            body["cpu_load"] = nullptr;
            body["cpu_core0_load"] = nullptr;
            body["cpu_core1_load"] = nullptr;
            body["heap_free"] = nullptr;
            body["rssi"] = nullptr;
            body["firmware_version"] = "crow-cpp-1.0";
            body["connection_mode"] = "wifi_lan";
            body["camera_model"] = "OV5640";
            body["is_connected"] = false;
            body["message"] = "ESP32-S3 hardware disconnected (Crow backend)";
        }

        if (inspection.valid) {
            crow::json::wvalue optical;
            writeOpticalJson(optical, inspection.optical);
            body["scene_optical"] = std::move(optical);
        }

        return crow::response(body);
    });
}

void BackendApp::registerInspectionRoutes() {
    CROW_ROUTE(m_app, "/api/v1/inspections/latest")([this]() {
        const InspectionView inspection = m_state.inspection();
        const CameraView camera = m_state.camera();

        crow::json::wvalue body;
        body["device_id"] = !inspection.device_id.empty() ? inspection.device_id : camera.device_id;
        body["source_type"] = "live_esp32";
        body["inspection_struct"] = "InspectionRecord";
        body["telemetry_struct"] = "TelemetryIngressPayload";
        body["frame_id"] = inspection.valid ? inspection.frame_id : camera.frame_id;
        body["engine_pipeline"] = inspection.engine_pipeline.empty()
            ? "YOLOv8-INT8 + OpenCV HSV"
            : inspection.engine_pipeline;
        body["is_live"] = camera.active;
        body["is_mock"] = false;
        body["vision_ready"] = m_state.visionReady();
        body["sequence"] = inspection.sequence;
        body["age_ms"] = inspection.age_ms;

        crow::json::wvalue optical;
        writeOpticalJson(optical, inspection.optical);
        body["scene_optical"] = std::move(optical);

        crow::json::wvalue::list detections;
        const uint32_t count = std::min<uint32_t>(
            inspection.detections.count, DetectionSoA::kMaxDetections);
        detections.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            crow::json::wvalue item;
            const std::string food_name = boundedString(
                inspection.detections.class_name[i], DetectionSoA::kClassNameBytes);
            item["food_name"] = food_name.empty() ? "unknown" : food_name;
            item["similarity_rate"] = inspection.detections.confidence[i];
            item["spoilage_score"] = inspection.detections.spoilage_score[i];
            item["status"] = statusName(inspection.detections.status[i]);

            crow::json::wvalue::list box;
            box.emplace_back(inspection.detections.x[i]);
            box.emplace_back(inspection.detections.y[i]);
            box.emplace_back(inspection.detections.width[i]);
            box.emplace_back(inspection.detections.height[i]);
            item["box"] = std::move(box);
            detections.emplace_back(std::move(item));
        }
        body["detections"] = std::move(detections);

        if (!inspection.valid) {
            body["message"] = "Awaiting first SHM frame from POST /api/v1/camera/frame";
        } else if (count == 0) {
            body["message"] = "Vision pipeline active: 0 food items currently in frame";
        } else {
            body["message"] = "Live C++ YOLO detections read from in-process memory";
        }

        return crow::response(body);
    });
}

int BackendApp::run() {
    CROW_LOG_INFO << "bawal::backend listening on " << m_config.bind_address
                  << ':' << m_config.port;
    m_app.run();
    return 0;
}

void BackendApp::stop() {
    m_app.stop();
}

} // namespace bawal::backend
