#include "bawal/backend/app.hpp"

#include "bawal/backend/byte_range.hpp"
#include "bawal/backend/database.hpp"
#include "bawal/backend/net_utils.hpp"
#include "bawal/backend/serial_provisioner.hpp"
#include "bawal/backend/text_utils.hpp"
#include "bawal/backend/usb_probe.hpp"
#include "bawal/backend/wifi_probe.hpp"
#include "bawal/server/rolling_image_store.hpp"
#include "bawal/report/latex_generator.hpp"
#include "bawal/lambda/events.hpp"
#include "bawal/lambda/handler.hpp"
#include "bawal/vision/classifier.hpp"
#include "bawal/vision/image/batch_inspector.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <opencv2/imgcodecs.hpp>

namespace bawal::backend {
namespace {

std::string boundedString(const char* data, size_t capacity) {
    const size_t len = strnlen(data, capacity);
    return std::string(data, len);
}

bool isValidJpeg(const std::vector<uint8_t>& image) {
    return image.size() >= 4 && image[0] == 0xFF && image[1] == 0xD8 &&
           image[image.size() - 2] == 0xFF && image.back() == 0xD9;
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

bool isMockDeviceId(std::string_view id) {
    return id == "MOCK_VIDEO_CAM" || id.rfind("MOCK_", 0) == 0;
}

uint64_t nowUnixSeconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string requestClientIp(const crow::request& req) {
    std::string ip = req.remote_ip_address;
    if (ip.rfind("::ffff:", 0) == 0) {
        ip = ip.substr(7);
    }
    // Docker bridge / Desktop VM addresses are not the device's LAN address;
    // report nothing instead of a misleading container gateway IP.
    if (looksDockerOrLoopbackIp(ip)) {
        return {};
    }
    return ip;
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

constexpr std::string_view kBatchTemplateName = "batch_report.tex.in";

/** Locate the multi-page batch LaTeX template relative to the backend CWD or binary. */
std::filesystem::path resolveBatchTemplatePath() {
    namespace fs = std::filesystem;
    if (const char* env = std::getenv("BAWAL_BATCH_TEMPLATE");
        env != nullptr && *env != '\0' && fs::is_regular_file(env)) {
        return fs::path(env);
    }

    std::error_code ec;
    const fs::path exe_dir = fs::canonical("/proc/self/exe", ec).parent_path();

    const fs::path candidates[] = {
        fs::path("./services/templates") / kBatchTemplateName,
        fs::path("../services/templates") / kBatchTemplateName,
        fs::path("../../services/templates") / kBatchTemplateName,
        fs::path("./templates") / kBatchTemplateName,
        fs::path("../templates") / kBatchTemplateName,
        fs::path("/app/services/templates") / kBatchTemplateName,
        exe_dir / "services/templates" / kBatchTemplateName,
        exe_dir / "templates" / kBatchTemplateName,
        exe_dir / "../templates" / kBatchTemplateName,
        exe_dir / "../../templates" / kBatchTemplateName,
        exe_dir / "../../../services/templates" / kBatchTemplateName,
    };
    for (const fs::path& candidate : candidates) {
        if (!candidate.empty() && fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}



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

    const std::string temperature_header = req.get_header_value("X-ESP-Temp");
    if (!temperature_header.empty()) {
        char* end = nullptr;
        const float value = std::strtof(temperature_header.c_str(), &end);
        if (end != temperature_header.c_str()) {
            parsed.temperature = value;
            parsed.has_temperature = true;
        }
    }
    const std::string load_header = req.get_header_value("X-ESP-CPU-Load");
    if (!load_header.empty()) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(load_header.c_str(), &end, 10);
        if (end != load_header.c_str() && value <= 100) {
            parsed.cpu_load = static_cast<uint8_t>(value);
            parsed.has_cpu_load = true;
        }
    }
    const std::string heap_internal_header = req.get_header_value("X-ESP-Free-Heap-Int");
    if (!heap_internal_header.empty()) {
        parsed.free_heap_internal = parseHeaderU32(req, "X-ESP-Free-Heap-Int");
        parsed.has_free_heap_internal = true;
    }
    const std::string heap_psram_header = req.get_header_value("X-ESP-Free-Heap-PSRAM");
    if (!heap_psram_header.empty()) {
        parsed.free_heap_psram = parseHeaderU32(req, "X-ESP-Free-Heap-PSRAM");
        parsed.has_free_heap_psram = true;
    }
    parsed.camera_model = req.get_header_value("X-Camera-Model");

    if (!isValidJpeg(parsed.image)) {
        return std::nullopt;
    }

    return parsed;
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
                       Database* database, server::RollingImageStore* image_store)
    : m_state(state),
      m_producer(producer),
      m_config(std::move(config)),
      m_database(database),
      m_image_store(image_store) {}

void BackendApp::setPeerCallback(std::function<void(const std::string&)> callback) {
    m_peer_callback = std::move(callback);
}

void BackendApp::setBatchClassifier(bawal::vision::FoodClassifier* classifier) noexcept {
    m_batch_classifier = classifier;
}

void BackendApp::setLambdaHandler(
    bawal::lambda::LambdaRuntimeHandler* handler) noexcept {
    m_lambda_handler = handler;
}

void BackendApp::configure() {
    auto& cors = m_app.get_middleware<crow::CORSHandler>();
    cors.global()
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Options)
        .headers("Origin", "Content-Type", "Accept", "Authorization", "X-Device-ID",
                 "X-Device-Token", "X-Frame-ID", "X-Width", "X-Height",
                 "X-ESP-Temp", "X-ESP-CPU-Load", "X-Camera-Model",
                 "X-Stream-Source", "X-Telemetry-Struct", "X-Inspection-Source",
                 "X-Inspection-Struct", "X-Mock-Video-Source", "X-Mock-Video-UUID",
                 "X-Is-Rewind", "X-Video-Rewind")
        .expose("X-Stream-Source", "X-Telemetry-Struct", "X-Mock-Video-UUID",
                "X-Report-Format");

    if (const char* configured = std::getenv("CONF_THRESHOLD");
        configured != nullptr && *configured != '\0') {
        char* end = nullptr;
        const double value = std::strtod(configured, &end);
        if (end != configured && value >= 0.25 && value <= 0.98) {
            m_confidence_threshold.store(value);
        }
    }

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
    registerImageRoutes();
    registerExampleRoutes();
    registerCompatibilityRoutes();
    registerDeviceRoutes();
    registerSystemRoutes();
    registerLambdaRoutes();
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

            const std::string peer_ip = requestClientIp(req);
            if (!peer_ip.empty() && !isMockDeviceId(parsed->device_id) &&
                m_peer_callback) {
                m_peer_callback(peer_ip);
            }

            // Publish metadata before the SHM notification so the vision
            // thread observes the correct device id on the very first frame.
            m_state.updateFrame(parsed->device_id, parsed->frame_id, parsed->image,
                                parsed->width, parsed->height);

            // Real ESP32-S3 nodes attach temperature, CPU load, and camera
            // model headers to every frame. Fold them into the telemetry cache
            // so the UI stays live even between JSON heartbeat POSTs.
            if (parsed->has_temperature || parsed->has_cpu_load ||
                parsed->has_free_heap_internal || parsed->has_free_heap_psram ||
                !parsed->camera_model.empty()) {
                TelemetrySample sample;
                sample.device_id = parsed->device_id;
                sample.camera_model = parsed->camera_model;
                sample.ip_address = peer_ip;
                sample.timestamp = nowUnixSeconds();
                sample.temperature = parsed->temperature;
                sample.cpu_temperature = parsed->temperature;
                sample.cpu_load = parsed->cpu_load;
                sample.free_heap_internal = parsed->free_heap_internal;
                sample.free_heap_psram = parsed->free_heap_psram;
                sample.has_temperature = parsed->has_temperature;
                sample.has_cpu_temperature = parsed->has_temperature;
                sample.has_cpu_load = parsed->has_cpu_load;
                sample.has_free_heap_internal = parsed->has_free_heap_internal;
                sample.has_free_heap_psram = parsed->has_free_heap_psram;
                m_state.updateTelemetry(sample);
            }

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
        const CameraView camera = m_state.liveCamera();

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

    // Effective source status: live ESP32 or mock video. The HMI uses this to
    // report the real ingestion FPS for whichever source is currently active.
    CROW_ROUTE(m_app, "/api/v1/camera/status")([this]() {
        const CameraView camera = m_state.camera();
        const bool is_mock = isMockDeviceId(camera.device_id);

        crow::json::wvalue body;
        body["active"] = camera.active;
        body["device_id"] = camera.device_id;
        body["source_type"] = is_mock ? "mock_video" : "live_esp32";
        body["is_mock"] = is_mock;
        body["telemetry_struct"] = "TelemetryIngressPayload";
        body["fps"] = camera.fps;
        body["frames_total"] = camera.frames_total;
        body["frame_id"] = camera.frame_id;
        body["width"] = camera.width;
        body["height"] = camera.height;
        body["last_frame_age_ms"] = camera.last_frame_age_ms;
        body["vision_ready"] = m_state.visionReady();
        body["message"] = camera.active
            ? "Camera ingress active"
            : "No active camera source found";
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
        const TelemetryView telemetry = m_state.telemetry();
        const InspectionView inspection = m_state.inspection();

        const bool mock_camera = camera.active && isMockDeviceId(camera.device_id);
        const bool telemetry_fresh = telemetry.valid && telemetry.age_ms <= 30'000ULL;
        const bool live_camera = camera.active && !mock_camera;
        const bool connected = telemetry_fresh || live_camera;

        crow::json::wvalue body;
        body["timestamp"] = nowUnixSeconds();
        body["vision_ready"] = m_state.visionReady();
        body["shm_write_sequence"] = m_producer.writeSequence();

        if (!connected && mock_camera) {
            // Mock video simulation: serve MockTelemetryPayload so the UI can
            // label the source clearly instead of faking hardware data.
            float sim_temperature = 37.2f;
            if (inspection.valid && inspection.optical.lux_luminance > 0.0f) {
                sim_temperature = std::round(
                    (36.5f + inspection.optical.lux_luminance / 300.0f) * 10.0f) / 10.0f;
            }

            body["mock_id"] = "MOCK_VIDEO_CAM";
            body["device_id"] = "MOCK_VIDEO_CAM";
            body["source_type"] = "mock_video";
            body["telemetry_struct"] = "MockTelemetryPayload";
            body["connection_mode"] = "mock_simulation";
            body["engine_pipeline"] = inspection.engine_pipeline.empty()
                ? "YOLOv8s-INT8 + OpenCV HSV"
                : inspection.engine_pipeline;
            body["temperature"] = sim_temperature;
            body["cpu_temperature"] = 42.5;
            body["cpu_load"] = static_cast<uint32_t>(18);
            body["cpu_core0_load"] = static_cast<uint32_t>(12);
            body["cpu_core1_load"] = static_cast<uint32_t>(24);
            body["heap_free"] = static_cast<uint64_t>(4194304);
            body["free_heap"] = static_cast<uint64_t>(4194304);
            body["free_heap_internal"] = static_cast<uint64_t>(262144);
            body["free_heap_psram"] = static_cast<uint64_t>(7340032);
            body["rssi"] = static_cast<int32_t>(-55);
            body["firmware_version"] = "sim-v2.1";
            body["is_connected"] = true;
            body["is_mock_active"] = true;
            body["frame_id"] = camera.frame_id;
            body["message"] = "Mock video simulation telemetry active";
        } else if (connected) {
            body["device_id"] = !telemetry.device_id.empty() ? telemetry.device_id
                                                             : camera.device_id;
            body["source_type"] = "live_esp32";
            body["telemetry_struct"] = "TelemetryIngressPayload";
            body["firmware_version"] = telemetry.firmware_version.empty()
                ? "1.0.0"
                : telemetry.firmware_version;
            body["ip_address"] = telemetry.ip_address;
            body["connection_mode"] = telemetry.connection_mode.empty()
                ? "wifi_lan"
                : telemetry.connection_mode;
            body["camera_model"] = telemetry.camera_model.empty()
                ? "OV5640"
                : telemetry.camera_model;
            body["is_connected"] = true;

            body["temperature"] = telemetry.has_temperature
                ? crow::json::wvalue(telemetry.temperature)
                : crow::json::wvalue(nullptr);
            body["cpu_temperature"] = telemetry.has_cpu_temperature
                ? crow::json::wvalue(telemetry.cpu_temperature)
                : crow::json::wvalue(nullptr);
            if (telemetry.has_cpu_load) {
                const uint8_t core0 = telemetry.cpu_core0_load >= 0
                    ? static_cast<uint8_t>(telemetry.cpu_core0_load)
                    : telemetry.cpu_load;
                const uint8_t core1 = telemetry.cpu_core1_load >= 0
                    ? static_cast<uint8_t>(telemetry.cpu_core1_load)
                    : telemetry.cpu_load;
                body["cpu_load"] = static_cast<uint32_t>(telemetry.cpu_load);
                body["cpu_core0_load"] = static_cast<uint32_t>(core0);
                body["cpu_core1_load"] = static_cast<uint32_t>(core1);
            } else {
                body["cpu_load"] = nullptr;
            }
            if (telemetry.has_heap_free) {
                body["heap_free"] = telemetry.heap_free;
                body["free_heap"] = telemetry.heap_free;
            }
            if (telemetry.has_free_heap_internal) {
                body["free_heap_internal"] = telemetry.free_heap_internal;
            }
            if (telemetry.has_free_heap_psram) {
                body["free_heap_psram"] = telemetry.free_heap_psram;
            }
            if (telemetry.has_rssi) {
                body["rssi"] = static_cast<int32_t>(telemetry.rssi);
            }
            if (inspection.valid) {
                crow::json::wvalue optical;
                writeOpticalJson(optical, inspection.optical);
                body["scene_optical"] = std::move(optical);
            }
            body["message"] = "Live ESP32-S3 hardware telemetry";
        } else {
            body["device_id"] = "DISCONNECTED";
            body["source_type"] = "live_esp32";
            body["telemetry_struct"] = "TelemetryIngressPayload";
            body["firmware_version"] = "1.0.0";
            body["connection_mode"] = "disconnected";
            body["is_connected"] = false;
            body["temperature"] = nullptr;
            body["cpu_temperature"] = nullptr;
            body["cpu_load"] = nullptr;
            body["heap_free"] = nullptr;
            body["free_heap_internal"] = nullptr;
            body["free_heap_psram"] = nullptr;
            body["rssi"] = nullptr;
            body["message"] = "ESP32-S3 hardware disconnected (Crow backend)";
        }

        return crow::response(body);
    });

    CROW_ROUTE(m_app, "/api/v1/telemetry")
        .methods(crow::HTTPMethod::Post)([this](const crow::request& req) {
            const auto doc = crow::json::load(req.body);
            if (!doc) {
                return jsonError(400, "Invalid JSON telemetry payload");
            }

            const std::string peer_ip = requestClientIp(req);
            if (!peer_ip.empty() && m_peer_callback) {
                m_peer_callback(peer_ip);
            }

            TelemetrySample sample;
            sample.ip_address = peer_ip;
            sample.connection_mode = "wifi_lan";
            sample.timestamp = nowUnixSeconds();

            const auto read_string = [&doc](const char* key, std::string& out) {
                if (doc.has(key) && doc[key].t() == crow::json::type::String) {
                    out = static_cast<std::string>(doc[key].s());
                }
            };
            const auto read_number = [&doc](const char* key, double& out) {
                if (doc.has(key) && doc[key].t() == crow::json::type::Number) {
                    out = doc[key].d();
                    return true;
                }
                return false;
            };

            read_string("device_id", sample.device_id);
            if (sample.device_id.empty()) {
                sample.device_id = req.get_header_value("X-Device-ID");
            }
            sample.device_id = sanitizeDeviceId(std::move(sample.device_id));
            read_string("firmware_version", sample.firmware_version);
            read_string("camera_model", sample.camera_model);
            read_string("connection_mode", sample.connection_mode);

            double number = 0.0;
            if (read_number("timestamp", number) && number > 0) {
                sample.timestamp = static_cast<uint64_t>(number);
            }
            if (read_number("temperature", number)) {
                sample.temperature = static_cast<float>(number);
                sample.has_temperature = true;
            }
            if (read_number("cpu_temperature", number)) {
                sample.cpu_temperature = static_cast<float>(number);
                sample.has_cpu_temperature = true;
            }
            if (read_number("cpu_load", number) || read_number("current_cpu_load", number)) {
                sample.cpu_load = static_cast<int>(number);
                sample.has_cpu_load = true;
            }
            if (read_number("cpu_core0_load", number)) {
                sample.cpu_core0_load = static_cast<int>(number);
            }
            if (read_number("cpu_core1_load", number)) {
                sample.cpu_core1_load = static_cast<int>(number);
            }
            if (read_number("free_heap", number) || read_number("heap_free", number)) {
                if (number > 0) {
                    sample.heap_free = static_cast<uint64_t>(number);
                    sample.has_heap_free = true;
                }
            }
            if (read_number("free_heap_internal", number)) {
                sample.free_heap_internal = static_cast<uint64_t>(number);
                sample.has_free_heap_internal = true;
            }
            if (read_number("free_heap_psram", number)) {
                sample.free_heap_psram = static_cast<uint64_t>(number);
                sample.has_free_heap_psram = true;
            }
            if (read_number("rssi", number)) {
                sample.rssi = static_cast<int>(number);
                sample.has_rssi = true;
            }

            m_state.updateTelemetry(sample);

            crow::json::wvalue body;
            body["status"] = "success";
            body["source_type"] = "live_esp32";
            body["telemetry_struct"] = "TelemetryIngressPayload";
            body["device_id"] = sample.device_id.empty() ? "DEFAULT_CAMERA"
                                                         : sample.device_id;
            body["message"] = "Live ESP32-S3 hardware telemetry heartbeat recorded";
            return crow::response(body);
        });

    CROW_ROUTE(m_app, "/api/v1/telemetry/mock")
        .methods(crow::HTTPMethod::Post)([](const crow::request& req) {
            const auto doc = crow::json::load(req.body);
            if (!doc) {
                return jsonError(400, "Invalid MockTelemetryPayload JSON");
            }
            crow::json::wvalue body;
            body["status"] = "success";
            body["source_type"] = "mock_video";
            body["telemetry_struct"] = "MockTelemetryPayload";
            body["device_id"] = "MOCK_VIDEO_CAM";
            body["message"] = "Mock telemetry accepted";
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

    CROW_ROUTE(m_app, "/api/v1/inspections")([this](const crow::request& req) {
        std::size_t limit = 100;
        if (const char* value = req.url_params.get("limit"); value != nullptr) {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (end != value && parsed > 0) {
                limit = std::min<std::size_t>(parsed, 100);
            }
        }

        const std::vector<InspectionHistoryEntry> entries =
            m_state.inspectionHistory(limit);
        crow::json::wvalue::list records;
        records.reserve(entries.size());
        for (const auto& entry : entries) {
            crow::json::wvalue item;
            item["frame_id"] = entry.frame_id;
            item["timestamp"] = entry.timestamp;
            item["device_id"] = entry.device_id;
            item["engine_pipeline"] = entry.engine_pipeline;
            item["verdict"] = entry.verdict;
            item["surface_temp"] = entry.surface_temp;
            item["lux_luminance"] = entry.lux_luminance;
            item["color_temperature"] = entry.color_temperature;
            item["white_balance_status"] = entry.white_balance_status;
            item["spoilage_risk"] = entry.spoilage_risk;
            item["tray_coverage"] = entry.tray_coverage;
            item["image_path"] = entry.image_path;
            item["t_safe_hours"] = entry.t_safe_hours;

            crow::json::wvalue::list detections;
            detections.reserve(entry.detections.size());
            for (const auto& detection : entry.detections) {
                crow::json::wvalue detection_item;
                detection_item["class_name"] = detection.class_name;
                detection_item["confidence"] = detection.confidence;
                crow::json::wvalue::list bbox;
                bbox.emplace_back(detection.bbox[0]);
                bbox.emplace_back(detection.bbox[1]);
                bbox.emplace_back(detection.bbox[2]);
                bbox.emplace_back(detection.bbox[3]);
                detection_item["bbox"] = std::move(bbox);
                detection_item["freshness_score"] = detection.freshness_score;
                detection_item["portion_ratio"] = detection.portion_ratio;
                detections.emplace_back(std::move(detection_item));
            }
            item["detections"] = std::move(detections);
            item["checksum_sha256"] = entry.checksum_sha256;
            records.emplace_back(std::move(item));
        }

        crow::json::wvalue body(std::move(records));
        return crow::response(body);
    });

    CROW_ROUTE(m_app, "/api/v1/inspections/<uint>/report.pdf")
    ([this](const crow::request&, crow::response& res, uint32_t frame_id) {
        const std::vector<InspectionHistoryEntry> entries = m_state.inspectionHistory(100);
        auto it = std::find_if(entries.begin(), entries.end(), [frame_id](const auto& e) {
            return e.frame_id == frame_id;
        });

        if (it == entries.end()) {
            res.code = 404;
            res.set_header("Content-Type", "application/json");
            res.write("{\"error\":\"Inspection frame record not found in rolling buffer\"}");
            res.end();
            return;
        }

        const std::string compiled_path =
            bawal::report::LatexPdfGenerator::generateReportPdf(*it);

        if (compiled_path.empty() || !std::filesystem::exists(compiled_path)) {
            res.code = 500;
            res.set_header("Content-Type", "application/json");
            res.write("{\"error\":\"Failed to generate or compile LaTeX PDF report\"}");
            res.end();
            return;
        }

        // pdflatex may be absent on the host; in that case serve the .tex
        // source and tell the client which artifact it actually received.
        const bool is_pdf = compiled_path.ends_with(".pdf");
        if (is_pdf) {
            res.set_static_file_info_unsafe(compiled_path, "application/pdf");
            res.add_header(
                "Content-Disposition",
                "attachment; filename=\"BawalVision-Report-" + std::to_string(frame_id) + ".pdf\"");
            res.add_header("X-Report-Format", "pdf");
        } else {
            res.set_static_file_info_unsafe(compiled_path, "text/x-tex");
            res.add_header(
                "Content-Disposition",
                "attachment; filename=\"BawalVision-Report-" + std::to_string(frame_id) + ".tex\"");
            res.add_header("X-Report-Format", "tex");
        }
        res.end();
    });

    // Raw LaTeX source download (works without a pdflatex installation).
    CROW_ROUTE(m_app, "/api/v1/inspections/<uint>/report.tex")
    ([this](const crow::request&, crow::response& res, uint32_t frame_id) {
        const std::vector<InspectionHistoryEntry> entries = m_state.inspectionHistory(100);
        auto it = std::find_if(entries.begin(), entries.end(), [frame_id](const auto& e) {
            return e.frame_id == frame_id;
        });

        if (it == entries.end()) {
            res.code = 404;
            res.set_header("Content-Type", "application/json");
            res.write("{\"error\":\"Inspection frame record not found in rolling buffer\"}");
            res.end();
            return;
        }

        const std::string tex_path =
            bawal::report::LatexPdfGenerator::generateReportTex(*it);

        if (tex_path.empty() || !std::filesystem::exists(tex_path)) {
            res.code = 500;
            res.set_header("Content-Type", "application/json");
            res.write("{\"error\":\"Failed to generate LaTeX source template\"}");
            res.end();
            return;
        }

        res.set_static_file_info_unsafe(tex_path, "text/x-tex");
        res.add_header(
            "Content-Disposition",
            "attachment; filename=\"BawalVision-Report-" + std::to_string(frame_id) + ".tex\"");
        res.add_header("X-Report-Format", "tex");
        res.end();
    });

    // ------------------------------------------------------------------
    // Batch image inspection: ZIP upload -> parallel OpenMP inspection ->
    // multi-page master PDF report.
    // ------------------------------------------------------------------
    CROW_ROUTE(m_app, "/api/v1/inspections/batch-zip")
        .methods(crow::HTTPMethod::Post)([this](const crow::request& req) {
            if (req.body.empty()) {
                return jsonError(400, "Empty payload body. Please upload a valid ZIP archive.");
            }
            if (req.body.size() > bawal::vision::image::BatchInspector::kMaxZipBytes) {
                return jsonError(413, "ZIP archive exceeds maximum limit of 15 MB.");
            }
            if (m_batch_classifier == nullptr || !m_batch_classifier->isReady()) {
                return jsonError(503, "Batch classifier is not ready (ONNX model unavailable).");
            }

            const std::filesystem::path template_path = resolveBatchTemplatePath();
            if (template_path.empty()) {
                return jsonError(500, "Batch LaTeX template not found on the server.");
            }

            // Fleet analytics: read the same PostgreSQL tables the provisioned
            // Grafana dashboard renders, so the master PDF carries fleet context
            // next to the batch-local quality metrics.
            bawal::vision::image::GrafanaAnalytics analytics;
            if (m_database != nullptr && m_database->ready()) {
                analytics = m_database->fetchGrafanaAnalytics();
            }

            // One dedicated batch classifier: serialize concurrent uploads
            // instead of interleaving tracker state across Crow worker threads.
            std::lock_guard<std::mutex> batch_lock(m_batch_mutex);
            const bawal::vision::image::BatchReportSummary summary =
                bawal::vision::image::BatchInspector::processZipBuffer(
                    reinterpret_cast<const uint8_t*>(req.body.data()),
                    req.body.size(),
                    *m_batch_classifier,
                    template_path,
                    &analytics);

            if (summary.total_images == 0) {
                return jsonError(400, "Failed to extract valid image files (.jpg/.png) from ZIP archive.");
            }

            // Persist the batch as `img-batch` audit rows so the Grafana
            // quality panels include operator ZIP uploads alongside live
            // camera inspections. Fails soft when PostgreSQL is offline.
            const bool persisted =
                m_database != nullptr && m_database->saveBatchInspection(summary);

            crow::json::wvalue res;
            res["status"] = "success";
            res["batch_id"] = summary.batch_id;
            res["inspection_type"] = summary.inspection_type;
            res["generated_at"] = summary.generated_at;
            res["model_arch"] = summary.model_arch;
            res["engine"] = summary.engine;
            res["omp_threads"] = summary.omp_threads;
            res["total_trays"] = summary.total_images;
            res["accepted"] = summary.accepted_count;
            res["rejected"] = summary.rejected_count;
            res["processing_time_ms"] = summary.total_processing_time_ms;
            res["throughput_trays_per_sec"] = summary.throughput_images_per_sec;
            res["pass_rate"] = summary.total_images > 0
                ? static_cast<double>(summary.accepted_count) /
                      static_cast<double>(summary.total_images) * 100.0
                : 0.0;
            res["quality_index"] = summary.quality.quality_index;
            res["avg_similarity"] = summary.quality.avg_similarity;
            res["avg_quality"] = summary.quality.avg_quality;
            res["min_quality"] = summary.quality.min_quality;
            res["max_quality"] = summary.quality.max_quality;
            res["quality_stddev"] = summary.quality.stddev_quality;
            res["total_detections"] = summary.quality.total_detections;
            res["pass_detections"] = summary.quality.pass_detections;
            res["review_detections"] = summary.quality.review_detections;
            res["reject_detections"] = summary.quality.reject_detections;
            res["report_checksum"] = summary.report_checksum;
            res["persisted_to_postgres"] = persisted;
            res["pdf_download_url"] =
                "/api/v1/inspections/batch/" + summary.batch_id + "/report.pdf";

            crow::json::wvalue::list histogram;
            histogram.reserve(summary.quality_histogram.size());
            for (const uint32_t bucket : summary.quality_histogram) {
                histogram.emplace_back(bucket);
            }
            res["quality_histogram"] = std::move(histogram);

            crow::json::wvalue::list class_rows;
            class_rows.reserve(summary.class_breakdown.size());
            for (const auto& cls : summary.class_breakdown) {
                crow::json::wvalue cls_json;
                cls_json["class_name"] =
                    std::string(bawal::vision::image::fixedStringView(cls.class_name));
                cls_json["detections"] = cls.detections;
                cls_json["pass_count"] = cls.pass_count;
                cls_json["review_count"] = cls.review_count;
                cls_json["reject_count"] = cls.reject_count;
                cls_json["avg_similarity"] = cls.avg_similarity;
                cls_json["avg_quality"] = cls.avg_quality;
                class_rows.emplace_back(std::move(cls_json));
            }
            res["class_breakdown"] = std::move(class_rows);

            crow::json::wvalue grafana_json;
            grafana_json["available"] = summary.grafana.available;
            grafana_json["datasource"] =
                std::string(bawal::vision::image::kGrafanaDatasource);
            grafana_json["dashboard_url"] =
                std::string(bawal::vision::image::kGrafanaDashboardUrl);
            grafana_json["total_inspections"] = summary.grafana.total_inspections;
            grafana_json["inspections_24h"] = summary.grafana.inspections_24h;
            grafana_json["tracked_devices"] = summary.grafana.tracked_devices;
            grafana_json["avg_similarity_pct"] = summary.grafana.avg_similarity_pct;
            grafana_json["fresh_food_rate"] = summary.grafana.fresh_food_rate;
            grafana_json["avg_spoilage_pct"] = summary.grafana.avg_spoilage_pct;

            crow::json::wvalue::list grafana_classes;
            grafana_classes.reserve(summary.grafana.class_stats.size());
            for (const auto& cls : summary.grafana.class_stats) {
                crow::json::wvalue cls_json;
                cls_json["class_name"] =
                    std::string(bawal::vision::image::fixedStringView(cls.class_name));
                cls_json["detections"] = cls.detections;
                cls_json["avg_similarity_pct"] = cls.avg_similarity_pct;
                cls_json["avg_spoilage_pct"] = cls.avg_spoilage_pct;
                cls_json["pass_rate"] = cls.pass_rate;
                grafana_classes.emplace_back(std::move(cls_json));
            }
            grafana_json["class_stats"] = std::move(grafana_classes);

            crow::json::wvalue::list grafana_daily;
            grafana_daily.reserve(summary.grafana.daily.size());
            for (const auto& point : summary.grafana.daily) {
                crow::json::wvalue point_json;
                point_json["day"] =
                    std::string(bawal::vision::image::fixedStringView(point.day));
                point_json["inspections"] = point.inspections;
                point_json["pass_rate"] = point.pass_rate;
                grafana_daily.emplace_back(std::move(point_json));
            }
            grafana_json["daily"] = std::move(grafana_daily);
            res["grafana"] = std::move(grafana_json);

            // Per-tray gallery payload: annotated image URL, verdict and the
            // YOLOv8s INT8 detection breakdown for the dedicated batch tab.
            crow::json::wvalue::list items;
            items.reserve(summary.items.size());
            for (const auto& item : summary.items) {
                crow::json::wvalue item_json;
                item_json["index"] = item.index;
                item_json["filename"] = item.filename;
                item_json["accepted"] = item.accepted;
                item_json["surface_temp"] = item.surface_temp;
                item_json["t_safe_hours"] = item.t_safe_hours;
                item_json["avg_similarity"] = item.avg_similarity;
                item_json["avg_quality"] = item.avg_quality;
                item_json["pass_detections"] = item.pass_detections;
                item_json["review_detections"] = item.review_detections;
                item_json["reject_detections"] = item.reject_detections;
                item_json["annotated_url"] = "/api/v1/inspections/batch/" + summary.batch_id +
                                             "/annotated/" + std::to_string(item.index);

                crow::json::wvalue::list detections;
                detections.reserve(item.detections.size());
                for (const auto& detection : item.detections) {
                    crow::json::wvalue detection_json;
                    detection_json["class_name"] = std::string(detection.class_name);
                    detection_json["confidence"] = detection.confidence;
                    detection_json["freshness"] = bawal::vision::image::freshnessScore(detection) / 100.0f;
                    detection_json["status"] = bawal::vision::image::qualityStatusName(
                        bawal::vision::image::classifyQuality(detection));

                    crow::json::wvalue::list bbox;
                    bbox.emplace_back(detection.bbox.x);
                    bbox.emplace_back(detection.bbox.y);
                    bbox.emplace_back(detection.bbox.width);
                    bbox.emplace_back(detection.bbox.height);
                    detection_json["bbox"] = std::move(bbox);
                    detections.emplace_back(std::move(detection_json));
                }
                item_json["detections"] = std::move(detections);
                items.emplace_back(std::move(item_json));
            }
            res["items"] = std::move(items);

            crow::response response(200, res);
            response.set_header("Content-Type", "application/json");
            return response;
        });

    CROW_ROUTE(m_app, "/api/v1/inspections/batch/<string>/report.pdf")
        ([](const crow::request&, crow::response& res, const std::string& batch_id) {
            // Directory traversal guard: batch ids are flat path components.
            if (batch_id.empty() || batch_id.find("..") != std::string::npos ||
                batch_id.find('/') != std::string::npos ||
                batch_id.find('\\') != std::string::npos) {
                res.code = 400;
                res.end("Bad Request: Invalid batch identifier");
                return;
            }

            namespace fs = std::filesystem;
            const fs::path batch_dir = fs::path("/tmp/bawal_batches") / batch_id;
            const fs::path pdf_file = batch_dir / "master_report.pdf";
            const fs::path tex_file = batch_dir / "master_report.tex";

            std::error_code ec;
            if (fs::is_regular_file(pdf_file, ec)) {
                res.set_static_file_info_unsafe(pdf_file.string(), "application/pdf");
                res.add_header(
                    "Content-Disposition",
                    "attachment; filename=\"BawalVision-Batch-" + batch_id + ".pdf\"");
                res.add_header("X-Report-Format", "pdf");
                res.end();
                return;
            }

            // pdflatex may be absent on the host; serve the generated LaTeX
            // source and tell the client which artifact it actually received.
            if (fs::is_regular_file(tex_file, ec)) {
                res.set_static_file_info_unsafe(tex_file.string(), "text/x-tex");
                res.add_header(
                    "Content-Disposition",
                    "attachment; filename=\"BawalVision-Batch-" + batch_id + ".tex\"");
                res.add_header("X-Report-Format", "tex");
                res.end();
                return;
            }

            res.code = 404;
            res.set_header("Content-Type", "application/json");
            res.write("{\"error\":\"Master batch PDF report not found or compilation pending\"}");
            res.end();
        });

    // Annotated tray snapshot produced by the batch inspector.
    CROW_ROUTE(m_app, "/api/v1/inspections/batch/<string>/annotated/<uint>")
        ([](const crow::request&, crow::response& res, const std::string& batch_id,
            uint32_t index) {
            if (batch_id.empty() || index == 0 ||
                batch_id.find("..") != std::string::npos ||
                batch_id.find('/') != std::string::npos ||
                batch_id.find('\\') != std::string::npos) {
                res.code = 400;
                res.end("Bad Request: Invalid batch identifier");
                return;
            }

            namespace fs = std::filesystem;
            const fs::path image_file = fs::path("/tmp/bawal_batches") / batch_id / "annotated" /
                                        ("annotated-" + std::to_string(index) + ".jpg");

            std::error_code ec;
            if (!fs::is_regular_file(image_file, ec)) {
                res.code = 404;
                res.set_header("Content-Type", "application/json");
                res.write("{\"error\":\"Annotated tray image not found for this batch\"}");
                res.end();
                return;
            }

            res.set_static_file_info_unsafe(image_file.string(), "image/jpeg");
            res.end();
        });
}

void BackendApp::registerImageRoutes() {
    CROW_ROUTE(m_app, "/inspections/<string>")
        ([](const crow::request&, crow::response& res, const std::string& filename) {
            namespace fs = std::filesystem;

            if (filename.empty() ||
                filename.find("..") != std::string::npos ||
                filename.find('/') != std::string::npos ||
                filename.find('\\') != std::string::npos) {
                res.code = 400;
                res.end("Bad Request");
                return;
            }

            const fs::path file_path = fs::path("./data/inspections") / filename;
            std::error_code ec;
            if (!fs::is_regular_file(file_path, ec)) {
                res.code = 404;
                res.end("Image Not Found");
                return;
            }

            res.set_static_file_info(file_path.string(), "image/jpeg");
            res.add_header("Cache-Control", "public, max-age=3600");
            res.end();
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
    const auto camera_mode = [this](const crow::request& req) {
        std::string mode = "live";
        try {
            const auto doc = crow::json::load(req.body);
            if (doc && doc.has("mode") && doc["mode"].t() == crow::json::type::String) {
                mode = static_cast<std::string>(doc["mode"].s());
            }
        } catch (const std::exception&) {
            // Keep the default; the mock frame payload still carries its own mode.
        }

        // Switching between live and mock clears the current snapshot, overlay
        // and tray state, then ignores in-flight frames from the old source for
        // a short grace window so stale bounding boxes never reach the UI.
        m_state.setMockMode(mode == "mock");
        m_state.clearDetectionState();
        m_state.requestSourceClear();

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
        .methods(crow::HTTPMethod::Post)([this]() {
            m_state.requestTrackerReset();
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

        if (m_image_store != nullptr) {
            // Prefer the vision worker's annotated frame (bounding boxes and
            // labels already rendered) so the rolling audit buffer never stores
            // a plain, un-annotated snapshot.
            std::vector<uint8_t> jpeg = m_state.latestAnnotatedJpeg();
            if (jpeg.empty()) {
                jpeg = m_state.latestJpeg();
            }
            if (!jpeg.empty()) {
                (void)m_image_store->save_encoded(
                    inspection.valid ? inspection.frame_id : 0, jpeg);
            }
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
            for (const auto& record : m_database->listInspections(100)) {
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

void BackendApp::registerDeviceRoutes() {
    CROW_ROUTE(m_app, "/api/v1/devices/status")([this]() {
        const CameraView camera = m_state.camera();
        const TelemetryView telemetry = m_state.telemetry();
        const bool telemetry_fresh = telemetry.valid && telemetry.age_ms <= 30'000ULL;
        const bool live_camera = camera.active && !isMockDeviceId(camera.device_id);

        bool connected = false;
        std::string connection_mode = "disconnected";
        std::string device_id;
        std::string firmware_version;
        // UDP discovery learns the ESP32 peer address before any camera POST;
        // expose it so the browser can pull MJPEG directly from the node.
        std::string ip_address = telemetry.ip_address;

        if (telemetry_fresh) {
            connected = true;
            connection_mode = telemetry.connection_mode.empty() ? "wifi_lan"
                                                                : telemetry.connection_mode;
            device_id = telemetry.device_id;
            firmware_version = telemetry.firmware_version.empty() ? "1.0.0"
                                                                  : telemetry.firmware_version;
            ip_address = telemetry.ip_address;
        } else if (live_camera) {
            connected = true;
            connection_mode = "wifi_lan";
            device_id = camera.device_id;
            firmware_version = telemetry.firmware_version.empty() ? "1.0.0"
                                                                  : telemetry.firmware_version;
            ip_address = telemetry.ip_address;
        }

        const std::vector<UsbPortInfo> usb_ports = scanUsbSerialPorts();
        // detectHostLanIp() honours BACKEND_IP/HOST_LAN_IP only while that
        // address is actually bound to one of our interfaces, so a laptop that
        // roamed to another Wi-Fi never advertises the previous network's IP.
        const std::string host_ip = detectHostLanIp();
        const char* ssid = std::getenv("WIFI_SSID");
        const char* wifi_pass = std::getenv("WIFI_PASS");
        // The .env Wi-Fi is a fallback only: once the laptop roams, the live
        // NetworkManager association is the truth and must win.
        const std::string active_ssid = detectActiveWifiSsid();
        const std::string server_ssid =
            !active_ssid.empty() ? active_ssid : (ssid != nullptr ? std::string(ssid)
                                                                  : std::string{});

        crow::json::wvalue body;
        body["connected"] = connected;
        body["connection_mode"] = connection_mode;
        body["device_id"] = device_id;
        body["firmware_version"] = firmware_version;
        body["ip_address"] = ip_address;
        body["stream_url"] = ip_address.empty()
            ? ""
            : "http://" + ip_address + "/stream";
        body["detected_host_ip"] = host_ip;

        crow::json::wvalue::list ports;
        for (const auto& port : usb_ports) {
            crow::json::wvalue item;
            item["port"] = port.port;
            item["name"] = port.name;
            item["vendor_id"] = port.vendor_id;
            item["product_id"] = port.product_id;
            item["manufacturer"] = port.manufacturer;
            item["product"] = port.product;
            item["is_esp32"] = port.is_esp32;
            ports.emplace_back(std::move(item));
        }
        body["usb_ports"] = std::move(ports);

        if (!usb_ports.empty()) {
            EspSerialAck cached;
            long long cache_age_ms = -1;
            const bool has_cached =
                lastEspSerialAck(cached, &cache_age_ms) && cache_age_ms <= 600'000;

            crow::json::wvalue esp_status;
            esp_status["port"] = usb_ports.front().port;
            esp_status["device_id"] = has_cached && !cached.device_id.empty()
                ? cached.device_id
                : device_id;
            // Never invent an SSID from the stale .env: without a recent serial
            // ACK only telemetry is authoritative, and it carries no SSID.
            esp_status["current_ssid"] = has_cached ? cached.ssid : "";
            esp_status["wifi_connected"] = has_cached ? cached.wifi_connected : connected;
            esp_status["temperature"] = has_cached
                ? cached.temperature
                : (telemetry.has_temperature ? telemetry.temperature : 0.0f);
            esp_status["status_source"] = has_cached ? "serial" : "telemetry";
            body["esp_usb_status"] = std::move(esp_status);
        }

        crow::json::wvalue::list networks;
        body["wifi_networks"] = std::move(networks);

        crow::json::wvalue server_wifi;
        server_wifi["ssid"] = server_ssid;
        server_wifi["active_ssid"] = active_ssid;
        server_wifi["has_password"] = wifi_pass != nullptr && *wifi_pass != '\0';
        server_wifi["backend_ip"] = host_ip;
        server_wifi["detected_host_ip"] = host_ip;
        server_wifi["backend_port"] = std::to_string(m_config.port);
        body["server_wifi"] = std::move(server_wifi);

        return crow::response(body);
    });

    auto wifi_networks_handler = []() {
        std::string raw_output;
        std::string executed_command;
        int exit_status = -1;
        const std::vector<WifiNetworkInfo> scanned =
            scanWifiNetworks(&raw_output, &executed_command, &exit_status);

        const char* path_env = std::getenv("PATH");
        const int exit_code =
            (exit_status >= 0 && WIFEXITED(exit_status))
                ? WEXITSTATUS(exit_status)
                : exit_status;
        std::cerr << "[WIFI] PATH=" << (path_env != nullptr ? path_env : "<unset>")
                  << '\n'
                  << "[WIFI] command=" << executed_command << '\n'
                  << "[WIFI] exit=" << exit_code
                  << " raw_bytes=" << raw_output.size() << '\n'
                  << "[WIFI] raw output:\n"
                  << (raw_output.empty() ? "<empty>\n" : raw_output);
        if (!raw_output.empty() && raw_output.back() != '\n') {
            std::cerr << '\n';
        }
        std::cerr << "[WIFI] parsed " << scanned.size() << " network(s)\n"
                  << std::flush;

        // Highlight the network the host is actually associated with; the
        // .env WIFI_SSID only answers when nmcli cannot tell us.
        const std::string active_ssid = detectActiveWifiSsid();
        const char* env_ssid = std::getenv("WIFI_SSID");
        const std::string server_ssid =
            !active_ssid.empty() ? active_ssid : (env_ssid != nullptr ? std::string(env_ssid)
                                                                      : std::string{});

        crow::json::wvalue::list networks;
        for (const auto& network : scanned) {
            crow::json::wvalue item;
            item["ssid"] = network.ssid;
            item["signal"] = network.signal;
            item["security"] = network.security;
            item["is_saved"] = network.is_saved;
            item["is_server"] = !server_ssid.empty() && network.ssid == server_ssid;
            networks.emplace_back(std::move(item));
        }

        crow::json::wvalue body;
        body["networks"] = std::move(networks);
        return crow::response(body);
    };
    CROW_ROUTE(m_app, "/api/v1/devices/wifi/networks")(wifi_networks_handler);
    CROW_ROUTE(m_app, "/api/v1/devices/wifi/scan")(wifi_networks_handler);

    CROW_ROUTE(m_app, "/api/v1/devices/usb/probe")
        .methods(crow::HTTPMethod::Post)([](const crow::request& req) {
            std::string requested;
            const auto doc = crow::json::load(req.body);
            if (doc && doc.has("port") && doc["port"].t() == crow::json::type::String) {
                requested = trimCopy(static_cast<std::string>(doc["port"].s()));
            }

            const std::vector<UsbPortInfo> ports = scanUsbSerialPorts();
            if (ports.empty()) {
                crow::json::wvalue body;
                body["success"] = false;
                body["message"] =
                    "No USB serial port detected (/dev/ttyACM* or /dev/ttyUSB*)";
                return crow::response(400, body);
            }

            std::string port = requested.empty() ? ports.front().port : requested;
            const bool present = std::any_of(
                ports.begin(), ports.end(),
                [&port](const UsbPortInfo& candidate) { return candidate.port == port; });
            if (!present) {
                crow::json::wvalue body;
                body["success"] = false;
                body["port"] = port;
                body["message"] = "USB serial port " + port +
                                  " is not present on the backend host";
                return crow::response(404, body);
            }

            // Real serial transaction: ask the board for its live state so the
            // Settings screen can show the current SSID/backend target without
            // depending on a browser Web Serial session.
            const EspSerialResult probe = probeEspSerial(port, 6000);

            crow::json::wvalue body;
            body["port"] = port;
            if (probe.success && probe.ack.received) {
                body["success"] = true;
                body["status"] = probe.ack.status.empty() ? "ok" : probe.ack.status;
                body["device_id"] = probe.ack.device_id;
                body["firmware_version"] = probe.ack.firmware_version;
                body["current_ssid"] = probe.ack.ssid;
                body["wifi_connected"] = probe.ack.wifi_connected;
                body["backend_ip"] = probe.ack.backend_ip;
                body["backend_port"] = probe.ack.backend_port;
                body["temperature"] = probe.ack.temperature;
                body["cpu_temperature"] = probe.ack.cpu_temperature;
                body["cpu_load"] = probe.ack.cpu_load;
                body["message"] = probe.ack.wifi_connected
                    ? ("ESP32-S3 (" + probe.ack.device_id + ") is connected to \"" +
                       probe.ack.ssid + "\"")
                    : "ESP32-S3 detected over USB serial (Wi-Fi not associated)";
                return crow::response(body);
            }

            body["success"] = false;
            body["message"] = "USB serial port " + port +
                              " detected, but no BAWAL_ACK received: " + probe.error;
            return crow::response(body);
        });

    CROW_ROUTE(m_app, "/api/v1/devices/connect")
        .methods(crow::HTTPMethod::Post)([this](const crow::request& req) {
            std::string requested_id;
            std::string requested_port;
            const auto doc = crow::json::load(req.body);
            if (doc) {
                if (doc.has("device_id") && doc["device_id"].t() == crow::json::type::String) {
                    requested_id = sanitizeDeviceId(
                        static_cast<std::string>(doc["device_id"].s()));
                }
                if (doc.has("usb_port") && doc["usb_port"].t() == crow::json::type::String) {
                    requested_port = trimCopy(static_cast<std::string>(doc["usb_port"].s()));
                }
            }

            const CameraView camera = m_state.camera();
            const TelemetryView telemetry = m_state.telemetry();
            const bool telemetry_fresh = telemetry.valid && telemetry.age_ms <= 30'000ULL;
            const bool live_camera = camera.active && !isMockDeviceId(camera.device_id);
            const auto matches = [&requested_id](const std::string& id) {
                return requested_id.empty() || id == requested_id;
            };

            if (telemetry_fresh && matches(telemetry.device_id)) {
                crow::json::wvalue body;
                body["success"] = true;
                body["status"] = "connected";
                body["device_id"] = telemetry.device_id;
                body["firmware_version"] = telemetry.firmware_version.empty()
                    ? "1.0.0"
                    : telemetry.firmware_version;
                body["ip_address"] = telemetry.ip_address;
                body["connection_mode"] = telemetry.connection_mode.empty()
                    ? "wifi_lan"
                    : telemetry.connection_mode;
                body["latency_ms"] = 8;
                body["message"] = "ESP32-S3 detected on the shared Wi-Fi network";
                return crow::response(body);
            }
            if (live_camera && matches(camera.device_id)) {
                crow::json::wvalue body;
                body["success"] = true;
                body["status"] = "connected";
                body["device_id"] = camera.device_id;
                body["firmware_version"] = telemetry.firmware_version.empty()
                    ? "1.0.0"
                    : telemetry.firmware_version;
                body["ip_address"] = telemetry.ip_address;
                body["connection_mode"] = "wifi_lan";
                body["latency_ms"] = 11;
                body["message"] = "ESP32-S3 camera node active on the shared Wi-Fi network";
                return crow::response(body);
            }

            const std::vector<UsbPortInfo> ports = scanUsbSerialPorts();
            crow::json::wvalue body;
            body["success"] = false;
            body["device_id"] = requested_id;
            body["connection_mode"] = "disconnected";
            if (!ports.empty() || !requested_port.empty()) {
                const std::string port =
                    !requested_port.empty() ? requested_port : ports.front().port;
                body["usb_port"] = port;

                // The board may already be provisioned and associated while the
                // backend has no telemetry yet (fresh boot, stale IP, ...).
                // Ask it directly over USB serial before reporting failure.
                const EspSerialResult probe = probeEspSerial(port, 6000);
                if (probe.success && probe.ack.received && probe.ack.wifi_connected &&
                    matches(probe.ack.device_id)) {
                    body["success"] = true;
                    body["status"] = "connected";
                    body["device_id"] = probe.ack.device_id;
                    body["firmware_version"] = probe.ack.firmware_version.empty()
                        ? "1.0.0"
                        : probe.ack.firmware_version;
                    body["ip_address"] = "USB Serial";
                    body["usb_port"] = port;
                    body["connection_mode"] = "usb_serial";
                    body["latency_ms"] = 4;
                    body["message"] =
                        "ESP32-S3 is associated with \"" + probe.ack.ssid +
                        "\" and targeting " + probe.ack.backend_ip + ":" +
                        std::to_string(probe.ack.backend_port);
                    return crow::response(body);
                }

                body["message"] =
                    "USB serial port " + port +
                    " is present, but the ESP32-S3 did not report an active Wi-Fi "
                    "association over serial: " + probe.error;
            } else {
                body["message"] =
                    "No ESP32-S3 detected on the server Wi-Fi network or USB serial "
                    "slots (/dev/ttyACM*, /dev/ttyUSB*).";
            }
            return crow::response(body);
        });

    CROW_ROUTE(m_app, "/api/v1/devices/usb/provision")
        .methods(crow::HTTPMethod::Post)([](const crow::request& req) {
            std::string port;
            std::string ssid;
            std::string password;
            std::string backend_ip;
            int backend_port = 8080;

            const auto doc = crow::json::load(req.body);
            if (doc) {
                if (doc.has("usb_port") && doc["usb_port"].t() == crow::json::type::String) {
                    port = trimCopy(static_cast<std::string>(doc["usb_port"].s()));
                }
                if (doc.has("ssid") && doc["ssid"].t() == crow::json::type::String) {
                    ssid = trimCopy(static_cast<std::string>(doc["ssid"].s()));
                }
                if (doc.has("password") && doc["password"].t() == crow::json::type::String) {
                    password = static_cast<std::string>(doc["password"].s());
                }
                for (const char* key : {"backend_ip", "gateway_host_ip"}) {
                    if (backend_ip.empty() && doc.has(key) &&
                        doc[key].t() == crow::json::type::String) {
                        backend_ip = trimCopy(static_cast<std::string>(doc[key].s()));
                    }
                }
                if (doc.has("backend_port")) {
                    const auto& value = doc["backend_port"];
                    if (value.t() == crow::json::type::Number && value.i() > 0 &&
                        value.i() <= 65535) {
                        backend_port = static_cast<int>(value.i());
                    } else if (value.t() == crow::json::type::String) {
                        const int parsed = std::atoi(std::string(value.s()).c_str());
                        if (parsed > 0 && parsed <= 65535) {
                            backend_port = parsed;
                        }
                    }
                }
            }

            if (port.empty()) {
                const std::vector<UsbPortInfo> ports = scanUsbSerialPorts();
                if (!ports.empty()) {
                    port = ports.front().port;
                }
            }

            // No target supplied: use the host's current LAN address instead of
            // letting the board drift over UDP discovery. If the address is
            // still unreachable the firmware falls back to discovery anyway.
            if (backend_ip.empty()) {
                backend_ip = detectHostLanIp();
            }

            // Pairing the board to the backend host's own Wi-Fi must not require
            // retyping the password in the browser. Fill it server-side (never
            // sent back over the API), preferring NetworkManager's live secret
            // for the network the host is actually on; .env is only a fallback.
            // An empty password for any other SSID still means "open network".
            if (password.empty()) {
                const std::string active_ssid = detectActiveWifiSsid();
                const char* server_ssid = std::getenv("WIFI_SSID");
                const bool host_network =
                    (!active_ssid.empty() && ssid == active_ssid) ||
                    (server_ssid != nullptr && *server_ssid != '\0' && ssid == server_ssid);
                if (host_network) {
                    password = wifiPasswordForSsid(ssid);
                    if (password.empty()) {
                        const char* server_pass = std::getenv("WIFI_PASS");
                        if (server_pass != nullptr) {
                            password = server_pass;
                        }
                    }
                }
            }

            crow::json::wvalue body;
            body["usb_port"] = port;
            if (port.empty()) {
                body["success"] = false;
                body["message"] =
                    "No USB serial port detected (/dev/ttyACM* or /dev/ttyUSB*) on the "
                    "backend host; connect the ESP32-S3 or use the browser Web Serial flow.";
                return crow::response(400, body);
            }
            if (ssid.empty()) {
                body["success"] = false;
                body["message"] = "Target Wi-Fi SSID is required for provisioning.";
                return crow::response(400, body);
            }

            // The firmware applies the credentials, waits up to 20 s for the
            // association and only then answers with BAWAL_ACK (rolling back on
            // failure), so the serial transaction must be patient.
            const EspSerialResult provision =
                provisionEspSerial(port, ssid, password, backend_ip, backend_port, 30000);

            if (provision.success && provision.ack.received &&
                provision.ack.status != "error") {
                const std::string provisioned_ssid =
                    provision.ack.ssid.empty() ? ssid : provision.ack.ssid;
                const std::string target_ip =
                    provision.ack.backend_ip.empty() ? backend_ip : provision.ack.backend_ip;

                body["success"] = true;
                body["status"] = provision.ack.status.empty() ? "wifi_provisioned"
                                                              : provision.ack.status;
                body["device_id"] = provision.ack.device_id;
                body["firmware_version"] = provision.ack.firmware_version;
                body["provisioned_ssid"] = provisioned_ssid;
                body["backend_ip"] = target_ip;
                body["gateway_host_ip"] = target_ip;
                body["wifi_connected"] = provision.ack.wifi_connected;
                body["temperature"] = provision.ack.temperature;
                body["message"] = provision.ack.wifi_connected
                    ? ("Provisioned Wi-Fi \"" + provisioned_ssid + "\" on " + port +
                       " - board is online")
                    : ("Credentials stored on " + port + "; ESP32-S3 is associating with \"" +
                       provisioned_ssid + "\"...");
                return crow::response(body);
            }

            body["success"] = false;
            if (provision.success && provision.ack.received) {
                body["status"] = "error";
                body["device_id"] = provision.ack.device_id;
                body["firmware_version"] = provision.ack.firmware_version;
                body["message"] =
                    "ESP32-S3 rejected Wi-Fi \"" + ssid +
                    "\" (credentials rolled back). Check the password and AP security, then retry.";
            } else {
                body["message"] = "USB provisioning on " + port +
                                  " failed: " + provision.error +
                                  ". Close idf.py monitor or any other serial tool and retry.";
            }
            return crow::response(body);
        });

    CROW_ROUTE(m_app, "/api/v1/devices/disconnect")
        .methods(crow::HTTPMethod::Post)([this](const crow::request& req) {
            std::string device_id;
            const auto doc = crow::json::load(req.body);
            if (doc && doc.has("device_id") && doc["device_id"].t() == crow::json::type::String) {
                device_id = sanitizeDeviceId(static_cast<std::string>(doc["device_id"].s()));
            }

            m_state.disconnectDevice(device_id);

            crow::json::wvalue body;
            body["success"] = true;
            body["message"] = "Device disconnected";
            return crow::response(body);
        });
}

void BackendApp::registerSystemRoutes() {
    CROW_ROUTE(m_app, "/api/v1/system/threshold")([this]() {
        const double rounded =
            std::round(m_confidence_threshold.load() * 100.0) / 100.0;
        crow::json::wvalue body;
        body["threshold"] = rounded;
        body["min"] = 0.25;
        body["max"] = 0.98;
        body["default"] = 0.35;
        return crow::response(body);
    });

    CROW_ROUTE(m_app, "/api/v1/system/threshold")
        .methods(crow::HTTPMethod::Post)([this](const crow::request& req) {
            const auto doc = crow::json::load(req.body);
            if (!doc || !doc.has("threshold") ||
                doc["threshold"].t() != crow::json::type::Number) {
                return jsonError(400, "Missing numeric threshold");
            }

            const double clamped = std::clamp(doc["threshold"].d(), 0.25, 0.98);
            m_confidence_threshold.store(clamped);

            crow::json::wvalue body;
            body["status"] = "success";
            body["threshold"] = std::round(clamped * 100.0) / 100.0;
            return crow::response(body);
        });
}

void BackendApp::registerLambdaRoutes() {
    CROW_ROUTE(m_app, "/api/v1/lambda/health")([this]() {
        crow::json::wvalue body;
        body["handler_attached"] = m_lambda_handler != nullptr;
        body["ready"] = m_lambda_handler != nullptr && m_lambda_handler->ready();
        body["runtime"] = "bawal::lambda";
        body["invoke_path"] = "/api/v1/lambda/inspect";
        return crow::response(body);
    });

    auto inspect = [this](const crow::request& req) {
        if (m_lambda_handler == nullptr) {
            return jsonError(503, "Lambda inspection handler is not attached");
        }

        const bawal::lambda::LambdaInspectionResponse result =
            m_lambda_handler->handleEvent(req.body);
        crow::response response(result.status_code);
        response.set_header("Content-Type", "application/json");
        response.body = result.json_payload.empty() ? "{}" : result.json_payload;
        return response;
    };

    CROW_ROUTE(m_app, "/api/v1/lambda/inspect")
        .methods(crow::HTTPMethod::Post)(inspect);
    CROW_ROUTE(m_app, "/api/v1/lambda/invoke")
        .methods(crow::HTTPMethod::Post)(inspect);
    CROW_ROUTE(m_app, "/api/v1/lambda/batch")
        .methods(crow::HTTPMethod::Post)(inspect);
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
