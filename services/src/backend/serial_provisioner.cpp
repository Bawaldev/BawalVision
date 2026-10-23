#include "bawal/backend/serial_provisioner.hpp"

#include "bawal/backend/text_utils.hpp"

#include <crow_all.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace bawal::backend {
namespace {

/**
 * One USB serial link is shared by probe and provision calls; Crow worker
 * threads must never interleave a transaction mid-ACK.
 */
std::mutex g_serial_mutex;

#ifdef _WIN32

EspSerialResult unsupportedPlatform() {
    EspSerialResult result;
    result.error = "USB serial provisioning is not supported on Windows builds; "
                   "use the browser Web Serial flow instead.";
    return result;
}

#else

long long steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** RAII fd wrapper so every error path closes the tty exactly once. */
class SerialFd {
public:
    SerialFd() = default;
    ~SerialFd() {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }
    void reset(int fd) noexcept { m_fd = fd; }
    [[nodiscard]] int get() const noexcept { return m_fd; }

private:
    int m_fd{-1};
};

/**
 * Opens the CDC-ACM node in raw mode without touching DTR/RTS.
 *
 * HUPCL is cleared so closing the port cannot pulse DTR and reset the board —
 * the ESP32-S3 must keep running its app while the backend talks to it.
 */
bool openRawPort(const std::string& port, SerialFd& serial, std::string& error) {
    const int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + port + " (" + std::strerror(errno) + ")";
        return false;
    }

    termios tio{};
    if (::tcgetattr(fd, &tio) == 0) {
        ::cfmakeraw(&tio);
        (void)::cfsetispeed(&tio, B115200);
        (void)::cfsetospeed(&tio, B115200);
        tio.c_cflag |= (CLOCAL | CREAD);
        tio.c_cflag &= ~HUPCL;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        (void)::tcsetattr(fd, TCSANOW, &tio);
    }
    // Drop stale console output so the ACK scan only sees fresh lines.
    ::tcflush(fd, TCIFLUSH);
    serial.reset(fd);
    return true;
}

bool writeAll(int fd, std::string_view data, int timeout_ms, std::string& error) {
    size_t written = 0;
    const long long deadline = steadyNowMs() + timeout_ms;
    while (written < data.size()) {
        const long long remaining = deadline - steadyNowMs();
        if (remaining <= 0) {
            error = "write timed out";
            return false;
        }
        pollfd pfd{fd, POLLOUT, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(std::min<long long>(remaining, 200)));
        if (rc < 0) {
            if (errno == EINTR) continue;
            error = std::strerror(errno);
            return false;
        }
        if (rc == 0) continue;

        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            error = std::strerror(errno);
            return false;
        }
    }
    return true;
}

double jsonNumber(const crow::json::rvalue& doc, const char* key) {
    if (!doc.has(key)) return 0.0;
    const crow::json::rvalue value = doc[key];
    return value.t() == crow::json::type::Number ? value.d() : 0.0;
}

std::string jsonString(const crow::json::rvalue& doc, const char* key) {
    if (!doc.has(key)) return {};
    const crow::json::rvalue value = doc[key];
    return value.t() == crow::json::type::String ? std::string(value.s()) : std::string{};
}

bool jsonBool(const crow::json::rvalue& doc, const char* key) {
    if (!doc.has(key)) return false;
    const crow::json::rvalue value = doc[key];
    return value.t() == crow::json::type::True;
}

void decodeAck(const std::string& json_text, EspSerialAck& ack, std::string& error) {
    const crow::json::rvalue doc = crow::json::load(json_text);
    if (!doc || doc.t() != crow::json::type::Object) {
        error = "malformed BAWAL_ACK payload: " + json_text;
        return;
    }
    ack.received = true;
    ack.status = jsonString(doc, "status");
    ack.device_id = jsonString(doc, "device_id");
    ack.firmware_version = jsonString(doc, "firmware_version");
    ack.ssid = jsonString(doc, "ssid");
    ack.backend_ip = jsonString(doc, "backend_ip");
    const double port = jsonNumber(doc, "backend_port");
    if (port > 0 && port <= 65535) {
        ack.backend_port = static_cast<int>(port);
    }
    ack.wifi_connected = jsonBool(doc, "wifi_connected");
    ack.temperature = jsonNumber(doc, "temperature");
    ack.cpu_temperature = jsonNumber(doc, "cpu_temperature");
    ack.cpu_load = jsonNumber(doc, "cpu_load");
}

/** Reads console output until a complete `BAWAL_ACK:{json}` line arrives. */
bool waitForAck(int fd, int timeout_ms, EspSerialAck& ack, std::string& error) {
    std::string buffer;
    buffer.reserve(4096);
    const long long deadline = steadyNowMs() + timeout_ms;

    static constexpr std::string_view kAckPrefix = "BAWAL_ACK:";

    while (steadyNowMs() < deadline) {
        const long long remaining = deadline - steadyNowMs();
        pollfd pfd{fd, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(std::min<long long>(remaining, 200)));
        if (rc < 0) {
            if (errno == EINTR) continue;
            error = std::strerror(errno);
            return false;
        }
        if (rc == 0) continue;

        char chunk[512];
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            buffer.append(chunk, static_cast<size_t>(n));
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            error = std::strerror(errno);
            return false;
        }

        const std::size_t ack_pos = buffer.find(kAckPrefix);
        if (ack_pos != std::string::npos) {
            const std::size_t line_end = buffer.find('\n', ack_pos);
            if (line_end != std::string::npos) {
                const std::string json_text = trimCopy(buffer.substr(
                    ack_pos + kAckPrefix.size(), line_end - ack_pos - kAckPrefix.size()));
                decodeAck(json_text, ack, error);
                return ack.received;
            }
        }
        if (buffer.size() > 16384) {
            buffer.erase(0, buffer.size() - 8192);
        }
    }

    error = "timed out waiting for BAWAL_ACK (close idf.py monitor or any other "
            "serial tool holding the port)";
    return false;
}

EspSerialResult runCommand(const std::string& port,
                           const std::string& command,
                           int timeout_ms) {
    EspSerialResult result;
    std::lock_guard<std::mutex> lock(g_serial_mutex);

    SerialFd serial;
    if (!openRawPort(port, serial, result.error)) {
        return result;
    }
    if (!writeAll(serial.get(), command, 2000, result.error)) {
        result.error = "failed to write to " + port + ": " + result.error;
        return result;
    }
    if (!waitForAck(serial.get(), timeout_ms, result.ack, result.error)) {
        return result;
    }
    result.success = true;
    return result;
}

#endif // _WIN32

} // namespace

EspSerialResult probeEspSerial(const std::string& port, int timeout_ms) {
#ifdef _WIN32
    (void)port;
    (void)timeout_ms;
    return unsupportedPlatform();
#else
    crow::json::wvalue payload;
    payload["cmd"] = "status";
    return runCommand(port, payload.dump() + "\n", timeout_ms);
#endif
}

EspSerialResult provisionEspSerial(const std::string& port,
                                   std::string_view ssid,
                                   std::string_view password,
                                   std::string_view backend_ip,
                                   int backend_port,
                                   int timeout_ms) {
#ifdef _WIN32
    (void)port;
    (void)ssid;
    (void)password;
    (void)backend_ip;
    (void)backend_port;
    (void)timeout_ms;
    return unsupportedPlatform();
#else
    crow::json::wvalue payload;
    payload["cmd"] = "provision_wifi";
    payload["ssid"] = std::string(ssid);
    payload["pass"] = std::string(password);
    if (!backend_ip.empty()) {
        payload["backend_ip"] = std::string(backend_ip);
    }
    payload["backend_port"] = backend_port;
    return runCommand(port, payload.dump() + "\n", timeout_ms);
#endif
}

} // namespace bawal::backend
