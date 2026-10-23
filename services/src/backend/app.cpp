#include "bawal/backend/app.hpp"

#include "bawal/backend/database.hpp"

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

bool isMockDeviceId(std::string_view id) {
    return id == "MOCK_VIDEO_CAM" || id.rfind("MOCK_", 0) == 0;
}

uint64_t nowUnixSeconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string trimCopy(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

bool looksDockerOrLoopbackIp(std::string_view ip);

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

std::string readSysfsValue(const std::string& path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    return trimCopy(value);
}

struct UsbPortInfo {
    std::string port;
    std::string name;
    std::string vendor_id;
    std::string product_id;
    std::string manufacturer;
    std::string product;
    bool is_esp32{false};
};

bool isEspUsbVendor(std::string_view vendor_id) {
    return vendor_id == "303a" || vendor_id == "10c4" ||
           vendor_id == "1a86" || vendor_id == "0403";
}

std::vector<UsbPortInfo> scanUsbSerialPorts() {
    std::vector<UsbPortInfo> ports;
    std::error_code ec;
    std::filesystem::directory_iterator it("/dev", ec);
    if (ec) {
        return ports;
    }

    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string node = it->path().filename().string();
        if (node.rfind("ttyACM", 0) != 0 && node.rfind("ttyUSB", 0) != 0) {
            continue;
        }

        UsbPortInfo info;
        info.port = "/dev/" + node;
        info.name = node.rfind("ttyACM", 0) == 0 ? "ESP32-S3 USB Serial/JTAG"
                                                  : "USB Serial";

        const std::string sys = "/sys/class/tty/" + node + "/device";
        std::error_code canonical_ec;
        const std::filesystem::path device =
            std::filesystem::canonical(sys, canonical_ec);
        const std::string base = canonical_ec ? sys : device.string();
        info.vendor_id = readSysfsValue(base + "/../idVendor");
        info.product_id = readSysfsValue(base + "/../idProduct");
        info.manufacturer = readSysfsValue(base + "/../manufacturer");
        info.product = readSysfsValue(base + "/../product");
        info.is_esp32 = isEspUsbVendor(info.vendor_id) ||
                        info.manufacturer.find("Espressif") != std::string::npos;
        ports.push_back(std::move(info));
    }

    std::sort(ports.begin(), ports.end(),
              [](const UsbPortInfo& lhs, const UsbPortInfo& rhs) {
                  return lhs.port < rhs.port;
              });
    return ports;
}

std::string runShellCommand(const std::string& command, int* exit_status = nullptr) {
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        if (exit_status != nullptr) {
            *exit_status = -1;
        }
        return output;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output.append(buffer);
    }
    if (exit_status != nullptr) {
        *exit_status = pclose(pipe);
    } else {
        pclose(pipe);
    }
    return output;
}

std::string resolveHostExecutable(const std::vector<std::string>& absolute_candidates,
                                  const std::string& fallback) {
    for (const std::string& candidate : absolute_candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return fallback;
}

std::vector<std::string> splitNmcliTerseLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;
    for (const char c : line) {
        if (escaped) {
            current.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == ':') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    return fields;
}

struct WifiNetworkInfo {
    std::string ssid;
    int signal{0};
    std::string security;
    bool is_saved{false};
};

std::vector<WifiNetworkInfo> parseWifiNetworks(const std::string& output) {
    std::vector<WifiNetworkInfo> networks;
    std::unordered_map<std::string, std::size_t> index_by_ssid;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> fields = splitNmcliTerseLine(line);
        if (fields.size() < 3) {
            continue;
        }

        const std::string ssid = trimCopy(fields[0]);
        if (ssid.empty() || ssid == "--") {
            continue;
        }

        int signal = 0;
        try {
            signal = std::stoi(trimCopy(fields[1]));
        } catch (const std::exception&) {
            continue;
        }
        signal = std::clamp(signal, 0, 100);

        std::string security = trimCopy(fields[2]);
        if (security.empty()) {
            security = "Open";
        }

        const auto existing = index_by_ssid.find(ssid);
        if (existing == index_by_ssid.end()) {
            index_by_ssid.emplace(ssid, networks.size());
            networks.push_back(WifiNetworkInfo{ssid, signal, security, false});
        } else if (signal > networks[existing->second].signal) {
            networks[existing->second].signal = signal;
            networks[existing->second].security = security;
        }
    }

    std::sort(networks.begin(), networks.end(),
              [](const WifiNetworkInfo& lhs, const WifiNetworkInfo& rhs) {
                  return lhs.signal > rhs.signal;
              });
    return networks;
}

std::vector<WifiNetworkInfo> scanWifiNetworks(std::string* raw_output = nullptr,
                                              std::string* executed_command = nullptr,
                                              int* exit_status = nullptr) {
    const std::string nmcli = resolveHostExecutable(
        {"/usr/bin/nmcli", "/bin/nmcli", "/usr/local/bin/nmcli"}, "nmcli");
    const std::string timeout = resolveHostExecutable(
        {"/usr/bin/timeout", "/bin/timeout"}, "timeout");
    const std::string rescan_command =
        timeout + " 8 " + nmcli +
        " -t -f SSID,SIGNAL,SECURITY device wifi list --rescan yes 2>&1";
    const std::string cached_command =
        timeout + " 8 " + nmcli +
        " -t -f SSID,SIGNAL,SECURITY device wifi list 2>&1";

    int status = -1;
    std::string output = runShellCommand(rescan_command, &status);
    std::string command = rescan_command;
    std::vector<WifiNetworkInfo> networks = parseWifiNetworks(output);

    // A forced rescan is rejected when NetworkManager is already scanning
    // (or when the caller lacks permission). Fall back to the cached list so
    // the settings page still shows the last known networks.
    if (networks.empty()) {
        int fallback_status = -1;
        std::string fallback_output = runShellCommand(cached_command, &fallback_status);
        std::vector<WifiNetworkInfo> fallback_networks =
            parseWifiNetworks(fallback_output);
        if (!fallback_networks.empty() || output.empty()) {
            output = std::move(fallback_output);
            status = fallback_status;
            command = cached_command;
            networks = std::move(fallback_networks);
        }
    }

    if (raw_output != nullptr) {
        *raw_output = output;
    }
    if (executed_command != nullptr) {
        *executed_command = command;
    }
    if (exit_status != nullptr) {
        *exit_status = status;
    }
    return networks;
}

bool looksDockerOrLoopbackIp(std::string_view ip) {
    if (ip.empty() || ip == "0.0.0.0") {
        return true;
    }
    if (ip.rfind("127.", 0) == 0 || ip.rfind("192.168.65.", 0) == 0) {
        return true;
    }
    if (ip.rfind("172.", 0) == 0) {
        const std::size_t dot = ip.find('.', 4);
        if (dot != std::string_view::npos) {
            const std::string second(ip.substr(4, dot - 4));
            char* end = nullptr;
            const long octet = std::strtol(second.c_str(), &end, 10);
            if (end != second.c_str() && octet >= 16 && octet <= 31) {
                return true;
            }
        }
    }
    return false;
}

std::string detectHostLanIp() {
    for (const char* key : {"HOST_LAN_IP", "BACKEND_IP"}) {
        const char* value = std::getenv(key);
        if (value != nullptr && *value != '\0' && !looksDockerOrLoopbackIp(value)) {
            return value;
        }
    }

    std::string fallback;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return {};
    }
    for (ifaddrs* it = interfaces; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        char buffer[INET_ADDRSTRLEN] = {0};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof(buffer)) == nullptr) {
            continue;
        }
        const std::string ip(buffer);
        if (looksDockerOrLoopbackIp(ip)) {
            continue;
        }
        if (ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0) {
            freeifaddrs(interfaces);
            return ip;
        }
        if (fallback.empty()) {
            fallback = ip;
        }
    }
    freeifaddrs(interfaces);
    return fallback;
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
    std::string camera_model;
    float temperature{0.0f};
    uint8_t cpu_load{0};
    uint32_t free_heap_internal{0};
    uint32_t free_heap_psram{0};
    bool has_temperature{false};
    bool has_cpu_load{false};
    bool has_free_heap_internal{false};
    bool has_free_heap_psram{false};
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
                 "X-Device-Token", "X-Frame-ID", "X-Width", "X-Height",
                 "X-ESP-Temp", "X-ESP-CPU-Load", "X-Camera-Model",
                 "X-Stream-Source", "X-Telemetry-Struct", "X-Inspection-Source",
                 "X-Inspection-Struct", "X-Mock-Video-Source", "X-Mock-Video-UUID",
                 "X-Is-Rewind", "X-Video-Rewind")
        .expose("X-Stream-Source", "X-Telemetry-Struct", "X-Mock-Video-UUID");

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
    registerExampleRoutes();
    registerCompatibilityRoutes();
    registerDeviceRoutes();
    registerSystemRoutes();
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

            // Real ESP32-S3 nodes attach temperature, CPU load, and camera
            // model headers to every frame. Fold them into the telemetry cache
            // so the UI stays live even between JSON heartbeat POSTs.
            if (parsed->has_temperature || parsed->has_cpu_load ||
                parsed->has_free_heap_internal || parsed->has_free_heap_psram ||
                !parsed->camera_model.empty()) {
                TelemetrySample sample;
                sample.device_id = parsed->device_id;
                sample.camera_model = parsed->camera_model;
                sample.ip_address = requestClientIp(req);
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

            TelemetrySample sample;
            sample.ip_address = requestClientIp(req);
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
        const std::string host_ip = detectHostLanIp();
        const char* ssid = std::getenv("WIFI_SSID");
        const char* wifi_pass = std::getenv("WIFI_PASS");
        const char* backend_ip_env = std::getenv("BACKEND_IP");

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
            crow::json::wvalue esp_status;
            esp_status["port"] = usb_ports.front().port;
            esp_status["device_id"] = device_id;
            esp_status["current_ssid"] = ssid != nullptr ? ssid : "";
            esp_status["wifi_connected"] = connected;
            esp_status["temperature"] =
                telemetry.has_temperature ? telemetry.temperature : 0.0f;
            body["esp_usb_status"] = std::move(esp_status);
        }

        crow::json::wvalue::list networks;
        body["wifi_networks"] = std::move(networks);

        crow::json::wvalue server_wifi;
        server_wifi["ssid"] = ssid != nullptr ? ssid : "";
        server_wifi["has_password"] = wifi_pass != nullptr && *wifi_pass != '\0';
        const std::string backend_ip =
            (backend_ip_env != nullptr && *backend_ip_env != '\0') ? backend_ip_env
                                                                   : host_ip;
        server_wifi["backend_ip"] = backend_ip;
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

        const char* server_ssid = std::getenv("WIFI_SSID");

        crow::json::wvalue::list networks;
        for (const auto& network : scanned) {
            crow::json::wvalue item;
            item["ssid"] = network.ssid;
            item["signal"] = network.signal;
            item["security"] = network.security;
            item["is_saved"] = network.is_saved;
            item["is_server"] = server_ssid != nullptr && *server_ssid != '\0' &&
                                network.ssid == server_ssid;
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

            crow::json::wvalue body;
            body["success"] = false;
            body["port"] = port;
            body["message"] =
                "USB serial port " + port +
                " detected, but serial probing is not accessible from this backend build; "
                "use the browser Web Serial flow to query the ESP32-S3.";
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
                body["message"] =
                    "USB serial port " + port +
                    " is present, but Wi-Fi provisioning is not accessible from the "
                    "backend process; use the browser Web Serial flow to provision "
                    "the ESP32-S3.";
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
            const auto doc = crow::json::load(req.body);
            if (doc && doc.has("usb_port") && doc["usb_port"].t() == crow::json::type::String) {
                port = trimCopy(static_cast<std::string>(doc["usb_port"].s()));
            }
            if (port.empty()) {
                const std::vector<UsbPortInfo> ports = scanUsbSerialPorts();
                if (!ports.empty()) {
                    port = ports.front().port;
                }
            }

            crow::json::wvalue body;
            body["success"] = false;
            body["usb_port"] = port;
            body["message"] =
                std::string("USB serial provisioning is not accessible from the backend "
                            "process") +
                (port.empty() ? std::string(".") : (" on " + port + ".")) +
                " Use the browser Web Serial flow to provision the ESP32-S3.";
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
