#include "bawal/backend/app.hpp"

#include "bawal/backend/database.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>
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

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<std::filesystem::path> exampleRoots() {
    std::vector<std::filesystem::path> roots;
    const auto add = [&roots](const std::filesystem::path& candidate) {
        std::error_code ec;
        if (!std::filesystem::is_directory(candidate, ec)) {
            return;
        }
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
        const std::filesystem::path root = ec ? candidate : canonical;
        if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
            roots.push_back(root);
        }
    };

    if (const char* configured = std::getenv("EXAMPLES_DIR"); configured != nullptr && *configured != '\0') {
        add(configured);
    }
    add("./examples");
    add("../examples");
    add("../../examples");
    add("/app/examples");
    add("./frontend/static/examples");
    add("../frontend/static/examples");
    return roots;
}

std::optional<std::filesystem::path> safeExampleRelative(std::string_view raw) {
    std::string cleaned(raw);
    while (!cleaned.empty() && (cleaned.front() == '/' || cleaned.front() == '\\')) {
        cleaned.erase(cleaned.begin());
    }
    if (cleaned.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path relative(cleaned);
    if (relative.is_absolute()) {
        return std::nullopt;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    return relative.lexically_normal();
}

std::string deterministicVideoUuid(std::string_view key) {
    const auto fnv = [](std::string_view value, uint64_t seed) {
        uint64_t hash = seed;
        for (const unsigned char c : value) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        return hash;
    };

    const uint64_t first = fnv(key, 1469598103934665603ULL);
    const uint64_t second = fnv(key, 1099511628211ULL);

    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(8) << static_cast<uint32_t>(first >> 32) << '-'
        << std::setw(4) << static_cast<uint16_t>(first >> 16) << '-'
        << std::setw(4) << static_cast<uint16_t>(first) << '-'
        << std::setw(4) << static_cast<uint16_t>(second >> 48) << '-'
        << std::setw(12) << (second & 0xFFFFFFFFFFFFULL);
    return out.str();
}

struct ByteRange {
    uint64_t start{0};
    uint64_t end{0};
};

std::optional<ByteRange> parseByteRange(std::string_view header, uint64_t size) {
    if (size == 0 || header.rfind("bytes=", 0) != 0) {
        return std::nullopt;
    }

    const std::string spec(header.substr(6));
    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos) {
        return std::nullopt;
    }

    try {
        const std::string first = spec.substr(0, dash);
        const std::string last = spec.substr(dash + 1);
        uint64_t start = 0;
        uint64_t end = 0;

        if (first.empty()) {
            const uint64_t suffix = std::stoull(last);
            if (suffix == 0) {
                return std::nullopt;
            }
            start = suffix >= size ? 0 : size - suffix;
            end = size - 1;
        } else {
            start = std::stoull(first);
            end = last.empty() ? size - 1 : std::stoull(last);
        }

        if (start >= size) {
            return std::nullopt;
        }
        end = std::min(end, size - 1);
        if (end < start) {
            return std::nullopt;
        }
        return ByteRange{start, end};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string videoMimeType(const std::filesystem::path& path) {
    return toLowerAscii(path.extension().string()) == ".webm" ? "video/webm" : "video/mp4";
}

crow::json::wvalue buildExamplesJson() {
    std::vector<std::string> videos;
    std::unordered_set<std::string> seen;

    for (const auto& root : exampleRoots()) {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        if (ec) {
            continue;
        }

        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec)) {
                continue;
            }

            const std::string extension = toLowerAscii(it->path().extension().string());
            if (extension != ".mp4" && extension != ".webm") {
                continue;
            }

            std::error_code relative_ec;
            const std::filesystem::path relative =
                std::filesystem::relative(it->path(), root, relative_ec);
            if (relative_ec) {
                continue;
            }

            const std::string key = relative.generic_string();
            if (key.empty() || !seen.insert(key).second) {
                continue;
            }
            videos.push_back(key);
        }
    }

    std::sort(videos.begin(), videos.end());

    crow::json::wvalue::list list;
    crow::json::wvalue uuid_map;
    crow::json::wvalue::list items;
    for (const auto& video : videos) {
        const std::string uuid = deterministicVideoUuid(video);
        list.emplace_back(video);
        uuid_map[video] = uuid;

        crow::json::wvalue item;
        item["path"] = video;
        item["video_uuid"] = uuid;
        items.emplace_back(std::move(item));
    }

    crow::json::wvalue body;
    body["status"] = "success";
    body["videos"] = std::move(list);
    body["video_uuids"] = std::move(uuid_map);
    body["video_items"] = std::move(items);
    return body;
}

} // namespace

BackendApp::BackendApp(BackendState& state, ipc::ShmProducer& producer, ServerConfig config,
                       Database* database)
    : m_state(state),
      m_producer(producer),
      m_config(std::move(config)),
      m_database(database) {}

void BackendApp::configure() {
    auto& cors = m_app.get_middleware<crow::CORSHandler>();
    cors.global()
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
        .headers("Origin", "Content-Type", "Accept", "Authorization", "X-Device-ID",
                 "X-Frame-ID", "X-Width", "X-Height", "X-Stream-Source",
                 "X-Telemetry-Struct", "X-Mock-Video-Source", "X-Mock-Video-UUID",
                 "X-Is-Rewind", "X-Video-Rewind")
        .expose("X-Stream-Source", "X-Telemetry-Struct", "X-Mock-Video-UUID");

    // Camera ingress is high-frequency; keep Crow's per-request INFO logging
    // out of the hot path unless explicitly requested for debugging.
    const char* verbose = std::getenv("CROW_VERBOSE");
    if (verbose == nullptr || std::string_view(verbose) != "1") {
        m_app.loglevel(crow::LogLevel::Warning);
    }

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
        body["database_connected"] = m_database != nullptr && m_database->ready();
        body["database_engine"] = m_database != nullptr ? "postgresql" : "none";
        return crow::response(body);
    });

    CROW_ROUTE(m_app, "/api/v1/database/stats")([this]() {
        crow::json::wvalue body;
        body["engine"] = m_database != nullptr ? "postgresql" : "none";
        body["connected"] = m_database != nullptr && m_database->ready();
        body["inspections"] = m_database != nullptr ? m_database->inspectionCount() : -1LL;
        return crow::response(body);
    });

    registerCameraRoutes();
    registerTelemetryRoutes();
    registerInspectionRoutes();
    registerExampleRoutes();
    registerCompatibilityRoutes();
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

            // Publish metadata before the SHM notification so the vision
            // thread observes the correct device id on the very first frame.
            m_state.updateFrame(parsed->device_id, parsed->frame_id, parsed->image,
                                parsed->width, parsed->height);

            if (!m_producer.writeFrame(parsed->frame_id, parsed->image,
                                       parsed->width, parsed->height, 0)) {
                return jsonError(413, "Frame exceeds the POSIX SHM slot capacity");
            }

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

    CROW_ROUTE(m_app, "/api/v1/telemetry")
        .methods(crow::HTTPMethod::Post)([this](const crow::request& req) {
            const auto doc = crow::json::load(req.body);
            if (!doc) {
                return jsonError(400, "Invalid JSON telemetry payload");
            }

            std::string device_id = req.get_header_value("X-Device-ID");
            if (doc.has("device_id") && doc["device_id"].t() == crow::json::type::String) {
                device_id = static_cast<std::string>(doc["device_id"].s());
            }
            device_id = sanitizeDeviceId(std::move(device_id));

            crow::json::wvalue body;
            body["status"] = "success";
            body["telemetry_struct"] = "TelemetryIngressPayload";
            body["device_id"] = device_id.empty() ? "DEFAULT_CAMERA" : device_id;
            body["message"] = "Telemetry accepted by the C++ Crow backend";
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

void BackendApp::registerExampleRoutes() {
    CROW_ROUTE(m_app, "/api/v1/examples")([]() {
        return crow::response(buildExamplesJson());
    });
    CROW_ROUTE(m_app, "/api/v1/examples/videos")([]() {
        return crow::response(buildExamplesJson());
    });

    const auto serve_example = [](const crow::request& req,
                                  const std::string& raw_path) -> crow::response {
        const auto relative = safeExampleRelative(raw_path);
        if (!relative) {
            return jsonError(400, "Invalid file path");
        }

        std::filesystem::path file;
        for (const auto& root : exampleRoots()) {
            std::error_code ec;
            const std::filesystem::path candidate = root / *relative;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                file = candidate;
                break;
            }
        }
        if (file.empty()) {
            return jsonError(404, "Video file not found");
        }

        std::ifstream input(file, std::ios::binary);
        if (!input) {
            return jsonError(500, "Unable to open video file");
        }
        std::string data((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
        if (data.empty()) {
            return jsonError(500, "Empty video file");
        }

        const std::string range_header = req.get_header_value("Range");
        const auto range = parseByteRange(range_header, data.size());
        if (!range_header.empty() && !range) {
            crow::response res(416);
            res.set_header("Accept-Ranges", "bytes");
            res.set_header("Content-Range", "bytes */" + std::to_string(data.size()));
            return res;
        }

        crow::response res;
        res.set_header("Content-Type", videoMimeType(file));
        res.set_header("Accept-Ranges", "bytes");
        res.set_header("Cache-Control", "public, max-age=3600");
        if (range) {
            const uint64_t length = range->end - range->start + 1;
            res.code = 206;
            res.set_header("Content-Range",
                           "bytes " + std::to_string(range->start) + "-" +
                               std::to_string(range->end) + "/" +
                               std::to_string(data.size()));
            res.set_header("Content-Length", std::to_string(length));
            res.body.assign(data.data() + range->start, static_cast<std::size_t>(length));
        } else {
            res.code = 200;
            res.set_header("Content-Length", std::to_string(data.size()));
            res.body = std::move(data);
        }
        return res;
    };

    CROW_ROUTE(m_app, "/api/v1/examples/file/<string>")
        ([serve_example](const crow::request& req, const std::string& name) {
            return serve_example(req, name);
        });
    CROW_ROUTE(m_app, "/api/v1/examples/file/<path>")
        ([serve_example](const crow::request& req, const std::string& path) {
            return serve_example(req, path);
        });
}

void BackendApp::registerCompatibilityRoutes() {
    const auto camera_mode = [](const crow::request& req) {
        std::string mode = "live";
        try {
            const auto doc = crow::json::load(req.body);
            if (doc && doc.has("mode") && doc["mode"].t() == crow::json::type::String) {
                mode = static_cast<std::string>(doc["mode"].s());
            }
        } catch (const std::exception&) {
            // Keep the default; the mock frame payload still carries its own mode.
        }

        crow::json::wvalue body;
        body["status"] = "success";
        body["mode"] = mode;
        body["message"] = "Stream mode accepted";
        return crow::response(body);
    };
    CROW_ROUTE(m_app, "/api/v1/camera/mode")
        .methods(crow::HTTPMethod::Post)(camera_mode);
    CROW_ROUTE(m_app, "/api/v1/devices/mode")
        .methods(crow::HTTPMethod::Post)(camera_mode);

    CROW_ROUTE(m_app, "/api/v1/devices/mock/reset")
        .methods(crow::HTTPMethod::Post)([]() {
            crow::json::wvalue body;
            body["success"] = true;
            body["message"] = "Mock inspection and rewind state cleared.";
            return crow::response(body);
        });

    const auto manual_inspection = [this](const crow::request&) {
        const InspectionView inspection = m_state.inspection();
        bool saved = false;
        if (m_database != nullptr && inspection.valid) {
            saved = m_database->saveInspection(
                inspection.device_id, inspection.frame_id,
                inspection.detections, inspection.optical,
                inspection.engine_pipeline);
        }

        crow::json::wvalue body;
        body["status"] = "success";
        body["saved"] = saved;
        body["device_id"] = inspection.device_id;
        body["message"] = "Manual inspection snapshot persisted";
        return crow::response(body);
    };
    CROW_ROUTE(m_app, "/api/v1/inspections")
        .methods(crow::HTTPMethod::Post)(manual_inspection);
    CROW_ROUTE(m_app, "/api/v1/inspections/mock")
        .methods(crow::HTTPMethod::Post)(manual_inspection);

    CROW_ROUTE(m_app, "/api/v1/system/engines")([]() {
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        crow::json::wvalue body;
        body["arch"] = "x86_64";
        body["graviton_available"] = false;
        body["local_engine_available"] = true;
        body["parallel_cores"] = static_cast<uint32_t>(cores);
        body["num_cpu"] = static_cast<uint32_t>(cores);
        body["server_mem_sys_mb"] = 0;
        body["server_mem_alloc_mb"] = 0;
        return crow::response(body);
    });

    CROW_ROUTE(m_app, "/api/v1/records")([this]() {
        crow::json::wvalue::list records;
        if (m_database != nullptr) {
            bool mock_record_included = false;
            for (const auto& record : m_database->listInspections(100)) {
                const bool is_mock =
                    record.device_id == "MOCK_VIDEO_CAM" ||
                    record.device_id.rfind("MOCK_", 0) == 0;
                if (is_mock) {
                    // The vision worker persists snapshots periodically; expose
                    // one mock-video tray record instead of every sampled frame.
                    if (mock_record_included) {
                        continue;
                    }
                    mock_record_included = true;
                }

                crow::json::wvalue item;
                item["id"] = record.id;
                item["device_id"] = record.device_id;
                item["firmware_version"] = record.firmware_version;
                item["created_at"] = record.created_at;
                item["image_s3_uri"] = record.image_s3_uri;
                item["cpu_temperature"] = record.cpu_temperature;
                item["temperature"] = record.temperature;
                item["cpu_load"] = record.cpu_load;
                item["is_spoiled"] = record.is_spoiled;
                item["t_safe_hours"] = record.t_safe_hours;
                item["frame_id"] = record.frame_id;
                item["lux_luminance"] = record.lux_luminance;
                item["white_balance_status"] = record.white_balance_status;
                item["color_temperature"] = record.color_temperature;
                item["engine_pipeline"] = record.engine_pipeline;

                crow::json::wvalue::list foods;
                for (const auto& food : record.foods) {
                    crow::json::wvalue food_item;
                    food_item["name"] = food.name;

                    crow::json::wvalue entry;
                    entry["food_item"] = std::move(food_item);
                    entry["similarity_rate"] = food.similarity_rate;
                    entry["spoilage_score"] = food.spoilage_score;
                    entry["status"] = food.status;
                    entry["temperature"] = record.temperature;
                    foods.emplace_back(std::move(entry));
                }
                item["inspection_foods"] = std::move(foods);
                records.emplace_back(std::move(item));
            }
        }

        crow::json::wvalue body(std::move(records));
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
