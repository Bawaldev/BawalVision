#pragma once

#if defined(BAWAL_USE_SYSTEM_HTTPLIB) && __has_include_next(<httplib.h>)
#include_next <httplib.h>
#else

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace httplib {

namespace detail {

/**
 * @brief Case-insensitive comparator for HTTP header keys.
 */
struct ci {
    /**
     * @brief Compares two strings case-insensitively.
     *
     * @param s1 First header key string.
     * @param s2 Second header key string.
     * @return true if s1 is lexicographically less than s2 ignoring case.
     */
    bool operator()(const std::string& s1, const std::string& s2) const noexcept {
        const size_t n1 = s1.size();
        const size_t n2 = s2.size();
        const size_t min_len = n1 < n2 ? n1 : n2;
        for (size_t i = 0; i < min_len; ++i) {
            const unsigned char c1 = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(s1[i])));
            const unsigned char c2 = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(s2[i])));
            if (c1 != c2) {
                return c1 < c2;
            }
        }
        return n1 < n2;
    }
};

} // namespace detail

using Headers = std::multimap<std::string, std::string, detail::ci>;

/**
 * @brief Enumeration of HTTP client transport errors.
 */
enum class Error {
    Success = 0,
    Unknown,
    Connection,
    BindIPAddress,
    Read,
    Write,
    ExceedRedirectCount,
    Canceled,
    SSLConnection,
    SSLLoadingCerts,
    SSLServerVerification,
    UnsupportedMultipartBoundaryChars,
    Compression,
    ConnectionTimeout
};

/**
 * @brief HTTP response container returned by httplib::Client requests.
 */
struct Response {
    std::string version;
    int status = -1;
    std::string reason;
    Headers headers;
    std::string body;

    /**
     * @brief Checks whether a specific response header exists.
     *
     * @param key Header name (case-insensitive).
     * @return true if the header exists in the response.
     */
    [[nodiscard]] bool has_header(const std::string& key) const {
        return headers.find(key) != headers.end();
    }

    /**
     * @brief Retrieves the value of a specific response header.
     *
     * @param key Header name (case-insensitive).
     * @param def Default value if header is missing.
     * @return std::string Header value or default string.
     */
    [[nodiscard]] std::string get_header_value(const std::string& key, const char* def = "") const {
        auto it = headers.find(key);
        return (it != headers.end()) ? it->second : std::string(def ? def : "");
    }
};

/**
 * @brief Wrapper holding either a valid HTTP Response or a transport Error.
 */
class Result {
public:
    Result() = default;
    Result(std::unique_ptr<Response>&& res, Error err, Headers&& request_headers = Headers{})
        : res_(std::move(res)), err_(err), request_headers_(std::move(request_headers)) {}

    [[nodiscard]] explicit operator bool() const noexcept { return res_ != nullptr; }
    [[nodiscard]] const Response& value() const { return *res_; }
    [[nodiscard]] Response& value() { return *res_; }
    [[nodiscard]] const Response& operator*() const { return *res_; }
    [[nodiscard]] Response& operator*() { return *res_; }
    [[nodiscard]] const Response* operator->() const { return res_.get(); }
    [[nodiscard]] Response* operator->() { return res_.get(); }
    [[nodiscard]] Error error() const noexcept { return err_; }

private:
    std::unique_ptr<Response> res_;
    Error err_ = Error::Unknown;
    Headers request_headers_;
};

/**
 * @brief High-connectivity HTTP/1.1 client supporting persistent Keep-Alive sockets and TCP_NODELAY.
 */
class Client {
public:
    /**
     * @brief Constructs an HTTP client from a base URL (e.g. "http://localhost:8080") or host string.
     *
     * @param scheme_host_port Target endpoint URL or hostname[:port].
     */
    explicit Client(const std::string& scheme_host_port) {
        parseEndpoint(scheme_host_port);
    }

    /**
     * @brief Constructs an HTTP client from explicit host and port.
     *
     * @param host Target hostname or IPv4/IPv6 address.
     * @param port Target TCP port.
     */
    Client(const std::string& host, int port)
        : host_(host), port_(port) {}

    ~Client() {
        closeSocket();
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Client(Client&& other) noexcept
        : host_(std::move(other.host_)),
          port_(other.port_),
          sock_fd_(other.sock_fd_),
          keep_alive_(other.keep_alive_),
          tcp_nodelay_(other.tcp_nodelay_),
          conn_timeout_ms_(other.conn_timeout_ms_),
          read_timeout_ms_(other.read_timeout_ms_),
          write_timeout_ms_(other.write_timeout_ms_),
          default_headers_(std::move(other.default_headers_)) {
        other.sock_fd_ = -1;
    }

    Client& operator=(Client&& other) noexcept {
        if (this != &other) {
            closeSocket();
            host_ = std::move(other.host_);
            port_ = other.port_;
            sock_fd_ = other.sock_fd_;
            keep_alive_ = other.keep_alive_;
            tcp_nodelay_ = other.tcp_nodelay_;
            conn_timeout_ms_ = other.conn_timeout_ms_;
            read_timeout_ms_ = other.read_timeout_ms_;
            write_timeout_ms_ = other.write_timeout_ms_;
            default_headers_ = std::move(other.default_headers_);
            other.sock_fd_ = -1;
        }
        return *this;
    }

    /**
     * @brief Checks whether the client configuration is valid.
     *
     * @return true if host and port are valid.
     */
    [[nodiscard]] bool is_valid() const noexcept {
        return !host_.empty() && port_ > 0 && port_ <= 65535;
    }

    /**
     * @brief Sets TCP connection establishment timeout.
     *
     * @param sec Seconds component.
     * @param usec Microseconds component.
     * @return void
     */
    void set_connection_timeout(time_t sec, time_t usec = 0) noexcept {
        conn_timeout_ms_ = static_cast<int>(sec * 1000 + usec / 1000);
        if (conn_timeout_ms_ <= 0) conn_timeout_ms_ = 1000;
    }

    template <class Rep, class Period>
    void set_connection_timeout(const std::chrono::duration<Rep, Period>& duration) noexcept {
        conn_timeout_ms_ = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
        if (conn_timeout_ms_ <= 0) conn_timeout_ms_ = 1000;
    }

    /**
     * @brief Sets socket read timeout.
     *
     * @param sec Seconds component.
     * @param usec Microseconds component.
     * @return void
     */
    void set_read_timeout(time_t sec, time_t usec = 0) noexcept {
        read_timeout_ms_ = static_cast<int>(sec * 1000 + usec / 1000);
        if (read_timeout_ms_ <= 0) read_timeout_ms_ = 1500;
        applySocketTimeouts();
    }

    template <class Rep, class Period>
    void set_read_timeout(const std::chrono::duration<Rep, Period>& duration) noexcept {
        read_timeout_ms_ = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
        if (read_timeout_ms_ <= 0) read_timeout_ms_ = 1500;
        applySocketTimeouts();
    }

    /**
     * @brief Sets socket write timeout.
     *
     * @param sec Seconds component.
     * @param usec Microseconds component.
     * @return void
     */
    void set_write_timeout(time_t sec, time_t usec = 0) noexcept {
        write_timeout_ms_ = static_cast<int>(sec * 1000 + usec / 1000);
        if (write_timeout_ms_ <= 0) write_timeout_ms_ = 1500;
        applySocketTimeouts();
    }

    template <class Rep, class Period>
    void set_write_timeout(const std::chrono::duration<Rep, Period>& duration) noexcept {
        write_timeout_ms_ = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
        if (write_timeout_ms_ <= 0) write_timeout_ms_ = 1500;
        applySocketTimeouts();
    }

    /**
     * @brief Enables or disables persistent HTTP/1.1 Keep-Alive connection reuse.
     *
     * @param on True to keep TCP socket open across sequential requests.
     * @return void
     */
    void set_keep_alive(bool on) noexcept {
        keep_alive_ = on;
        if (!keep_alive_) {
            closeSocket();
        }
    }

    /**
     * @brief Enables or disables TCP_NODELAY (disables Nagle's algorithm for low-latency telemetry).
     *
     * @param on True to enable immediate packet transmission.
     * @return void
     */
    void set_tcp_nodelay(bool on) noexcept {
        tcp_nodelay_ = on;
        if (sock_fd_ >= 0) {
            int flag = tcp_nodelay_ ? 1 : 0;
            ::setsockopt(sock_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        }
    }

    /**
     * @brief Sets default HTTP headers attached to every request from this client.
     *
     * @param headers Multimap of header key-value pairs.
     * @return void
     */
    void set_default_headers(Headers headers) {
        default_headers_ = std::move(headers);
    }

    /**
     * @brief Performs an HTTP GET request to the specified path.
     *
     * @param path Request path (e.g. "/api/v1/camera/active").
     * @return Result Response wrapper.
     */
    [[nodiscard]] Result Get(const std::string& path) {
        return Get(path, Headers{});
    }

    /**
     * @brief Performs an HTTP GET request with custom headers.
     *
     * @param path Request path.
     * @param headers Custom HTTP headers.
     * @return Result Response wrapper.
     */
    [[nodiscard]] Result Get(const std::string& path, const Headers& headers) {
        return sendRequest("GET", path, headers, nullptr, 0, "");
    }

    /**
     * @brief Performs an HTTP POST request with string payload and content type.
     *
     * @param path Request path.
     * @param body Payload string.
     * @param content_type MIME content type (e.g. "application/json").
     * @return Result Response wrapper.
     */
    [[nodiscard]] Result Post(const std::string& path, const std::string& body, const std::string& content_type) {
        return Post(path, Headers{}, body.data(), body.size(), content_type);
    }

    /**
     * @brief Performs an HTTP POST request with custom headers and string payload.
     *
     * @param path Request path.
     * @param headers Custom HTTP headers.
     * @param body Payload string.
     * @param content_type MIME content type.
     * @return Result Response wrapper.
     */
    [[nodiscard]] Result Post(const std::string& path, const Headers& headers, const std::string& body, const std::string& content_type) {
        return Post(path, headers, body.data(), body.size(), content_type);
    }

    /**
     * @brief Performs an HTTP POST request with raw binary buffer and content length.
     *
     * @param path Request path.
     * @param body Pointer to raw payload bytes.
     * @param content_length Number of bytes in payload.
     * @param content_type MIME content type.
     * @return Result Response wrapper.
     */
    [[nodiscard]] Result Post(const std::string& path, const char* body, size_t content_length, const std::string& content_type) {
        return Post(path, Headers{}, body, content_length, content_type);
    }

    /**
     * @brief Performs an HTTP POST request with custom headers, raw binary buffer, and content length.
     *
     * @param path Request path.
     * @param headers Custom HTTP headers.
     * @param body Pointer to raw payload bytes.
     * @param content_length Number of bytes in payload.
     * @param content_type MIME content type.
     * @return Result Response wrapper.
     */
    [[nodiscard]] Result Post(const std::string& path, const Headers& headers, const char* body, size_t content_length, const std::string& content_type) {
        return sendRequest("POST", path, headers, body, content_length, content_type);
    }

private:
    std::string host_ = "localhost";
    int port_ = 80;
    int sock_fd_ = -1;
    bool keep_alive_ = true;
    bool tcp_nodelay_ = true;
    int conn_timeout_ms_ = 1000;
    int read_timeout_ms_ = 1500;
    int write_timeout_ms_ = 1500;
    Headers default_headers_;

    void parseEndpoint(std::string_view input) {
        std::string_view s = input;
        if (s.rfind("http://", 0) == 0) {
            s.remove_prefix(7);
        } else if (s.rfind("https://", 0) == 0) {
            s.remove_prefix(8);
        }
        const size_t slash_pos = s.find('/');
        if (slash_pos != std::string_view::npos) {
            s = s.substr(0, slash_pos);
        }
        const size_t colon_pos = s.rfind(':');
        if (colon_pos != std::string_view::npos) {
            host_ = std::string(s.substr(0, colon_pos));
            const std::string port_str(s.substr(colon_pos + 1));
            const int parsed_port = std::atoi(port_str.c_str());
            port_ = (parsed_port > 0 && parsed_port <= 65535) ? parsed_port : 80;
        } else {
            host_ = std::string(s);
            port_ = 80;
        }
    }

    void closeSocket() noexcept {
        if (sock_fd_ >= 0) {
            ::close(sock_fd_);
            sock_fd_ = -1;
        }
    }

    void applySocketTimeouts() noexcept {
        if (sock_fd_ < 0) return;
        struct timeval rtv {};
        rtv.tv_sec = read_timeout_ms_ / 1000;
        rtv.tv_usec = (read_timeout_ms_ % 1000) * 1000;
        ::setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

        struct timeval wtv {};
        wtv.tv_sec = write_timeout_ms_ / 1000;
        wtv.tv_usec = (write_timeout_ms_ % 1000) * 1000;
        ::setsockopt(sock_fd_, SOL_SOCKET, SO_SNDTIMEO, &wtv, sizeof(wtv));
    }

    [[nodiscard]] bool isSocketHealthy() const noexcept {
        if (sock_fd_ < 0) return false;
        struct pollfd pfd {};
        pfd.fd = sock_fd_;
        pfd.events = POLLIN;
        const int r = ::poll(&pfd, 1, 0);
        if (r < 0) return false;
        if (r > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            return false;
        }
        if (r > 0 && (pfd.revents & POLLIN)) {
            char peek_byte = 0;
            const ssize_t n = ::recv(sock_fd_, &peek_byte, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n == 0) {
                // Remote peer gracefully closed the keep-alive socket
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool ensureConnected() {
        if (keep_alive_ && isSocketHealthy()) {
            return true;
        }
        closeSocket();

        struct addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        struct addrinfo* res_list = nullptr;
        const std::string port_str = std::to_string(port_);
        if (::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res_list) != 0 || !res_list) {
            return false;
        }

        int connected_fd = -1;
        for (struct addrinfo* rp = res_list; rp != nullptr; rp = rp->ai_next) {
            int fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd < 0) continue;

            // Set non-blocking for bounded connect timeout
            const int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0) {
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }

            if (tcp_nodelay_) {
                int one = 1;
                ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            }
            if (keep_alive_) {
                int one = 1;
                ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            }

            int rc = ::connect(fd, rp->ai_addr, rp->ai_addrlen);
            if (rc == 0) {
                if (flags >= 0) ::fcntl(fd, F_SETFL, flags);
                connected_fd = fd;
                break;
            } else if (errno == EINPROGRESS) {
                struct pollfd pfd {};
                pfd.fd = fd;
                pfd.events = POLLOUT;
                const int pr = ::poll(&pfd, 1, conn_timeout_ms_);
                if (pr > 0 && (pfd.revents & POLLOUT)) {
                    int so_error = 0;
                    socklen_t len = sizeof(so_error);
                    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                        if (flags >= 0) ::fcntl(fd, F_SETFL, flags);
                        connected_fd = fd;
                        break;
                    }
                }
            }
            ::close(fd);
        }

        ::freeaddrinfo(res_list);
        if (connected_fd < 0) {
            return false;
        }

        sock_fd_ = connected_fd;
        applySocketTimeouts();
        return true;
    }

    [[nodiscard]] bool writeAll(const char* data, size_t len) {
        size_t total_sent = 0;
        while (total_sent < len) {
            const ssize_t n = ::send(sock_fd_, data + total_sent, len - total_sent, MSG_NOSIGNAL);
            if (n <= 0) {
                if (errno == EINTR) continue;
                return false;
            }
            total_sent += static_cast<size_t>(n);
        }
        return true;
    }

    [[nodiscard]] Result sendRequest(
        const char* method,
        const std::string& path,
        const Headers& headers,
        const char* body,
        size_t content_length,
        const std::string& content_type
    ) {
        if (!is_valid()) {
            return Result(nullptr, Error::Connection);
        }

        // Allow up to 2 attempts in case a pooled keep-alive socket was closed by server just before send
        for (int attempt = 0; attempt < 2; ++attempt) {
            const bool reusing_socket = (sock_fd_ >= 0);
            if (!ensureConnected()) {
                return Result(nullptr, Error::ConnectionTimeout);
            }

            std::string req_target = path.empty() ? "/" : path;
            if (req_target[0] != '/') {
                const size_t scheme_pos = req_target.find("://");
                if (scheme_pos != std::string::npos) {
                    const size_t slash = req_target.find('/', scheme_pos + 3);
                    req_target = (slash != std::string::npos) ? req_target.substr(slash) : "/";
                } else {
                    req_target = "/" + req_target;
                }
            }

            Headers merged = default_headers_;
            for (const auto& kv : headers) {
                merged.erase(kv.first);
                merged.insert(kv);
            }
            if (!content_type.empty()) {
                merged.erase("Content-Type");
                merged.emplace("Content-Type", content_type);
            }

            std::ostringstream req_ss;
            req_ss << method << " " << req_target << " HTTP/1.1\r\n";
            req_ss << "Host: " << host_ << ":" << port_ << "\r\n";
            req_ss << "Connection: " << (keep_alive_ ? "keep-alive" : "close") << "\r\n";
            req_ss << "User-Agent: BawalVision-httplib/1.0\r\n";
            if (body != nullptr || std::strcmp(method, "POST") == 0 || std::strcmp(method, "PUT") == 0) {
                req_ss << "Content-Length: " << content_length << "\r\n";
            }
            for (const auto& [k, v] : merged) {
                if (detail::ci{}(k, "Host") == detail::ci{}("Host", k) &&
                    detail::ci{}(k, "Connection") == detail::ci{}("Connection", k) &&
                    detail::ci{}(k, "Content-Length") == detail::ci{}("Content-Length", k)) {
                    continue;
                }
                req_ss << k << ": " << v << "\r\n";
            }
            req_ss << "\r\n";

            const std::string header_str = req_ss.str();
            if (!writeAll(header_str.data(), header_str.size()) ||
                (body && content_length > 0 && !writeAll(body, content_length))) {
                closeSocket();
                if (reusing_socket && attempt == 0) {
                    continue;
                }
                return Result(nullptr, Error::Write);
            }

            auto response = std::make_unique<Response>();
            bool server_closed = false;
            if (!readResponse(*response, server_closed)) {
                closeSocket();
                if (reusing_socket && attempt == 0) {
                    continue;
                }
                return Result(nullptr, Error::Read);
            }

            if (!keep_alive_ || server_closed) {
                closeSocket();
            }

            return Result(std::move(response), Error::Success, std::move(merged));
        }

        return Result(nullptr, Error::Connection);
    }

    [[nodiscard]] bool readResponse(Response& res, bool& server_closed) {
        std::string raw_buf;
        raw_buf.reserve(8192);
        char chunk[4096];
        size_t header_end_pos = std::string::npos;

        while (header_end_pos == std::string::npos) {
            const ssize_t n = ::recv(sock_fd_, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                return false;
            }
            raw_buf.append(chunk, static_cast<size_t>(n));
            header_end_pos = raw_buf.find("\r\n\r\n");
            if (raw_buf.size() > 65536 && header_end_pos == std::string::npos) {
                return false;
            }
        }

        const std::string header_block = raw_buf.substr(0, header_end_pos);
        std::string remaining = raw_buf.substr(header_end_pos + 4);

        std::istringstream hstream(header_block);
        std::string status_line;
        if (!std::getline(hstream, status_line)) {
            return false;
        }
        if (!status_line.empty() && status_line.back() == '\r') {
            status_line.pop_back();
        }

        {
            std::istringstream sls(status_line);
            sls >> res.version >> res.status;
            std::getline(sls, res.reason);
            if (!res.reason.empty() && res.reason.front() == ' ') {
                res.reason.erase(0, 1);
            }
        }
        if (res.status <= 0) {
            return false;
        }

        long content_length = -1;
        bool is_chunked = false;
        server_closed = false;

        std::string line;
        while (std::getline(hstream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) continue;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
                val.erase(0, 1);
            }
            res.headers.emplace(key, val);

            if (!detail::ci{}(key, "Content-Length") && !detail::ci{}("Content-Length", key)) {
                content_length = std::atol(val.c_str());
            } else if (!detail::ci{}(key, "Transfer-Encoding") && !detail::ci{}("Transfer-Encoding", key)) {
                if (val.find("chunked") != std::string::npos || val.find("Chunked") != std::string::npos) {
                    is_chunked = true;
                }
            } else if (!detail::ci{}(key, "Connection") && !detail::ci{}("Connection", key)) {
                if (val.find("close") != std::string::npos || val.find("Close") != std::string::npos) {
                    server_closed = true;
                }
            }
        }

        if (is_chunked) {
            return readChunkedBody(remaining, res.body);
        } else if (content_length >= 0) {
            const size_t target_len = static_cast<size_t>(content_length);
            res.body = std::move(remaining);
            if (res.body.size() < target_len) {
                res.body.reserve(target_len);
                while (res.body.size() < target_len) {
                    const size_t to_read = std::min(sizeof(chunk), target_len - res.body.size());
                    const ssize_t n = ::recv(sock_fd_, chunk, to_read, 0);
                    if (n <= 0) {
                        if (n < 0 && errno == EINTR) continue;
                        return false;
                    }
                    res.body.append(chunk, static_cast<size_t>(n));
                }
            } else if (res.body.size() > target_len) {
                res.body.resize(target_len);
            }
            return true;
        } else {
            // Read until EOF when neither Content-Length nor chunked encoding is specified
            server_closed = true;
            res.body = std::move(remaining);
            while (true) {
                const ssize_t n = ::recv(sock_fd_, chunk, sizeof(chunk), 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (n == 0) break;
                res.body.append(chunk, static_cast<size_t>(n));
            }
            return true;
        }
    }

    [[nodiscard]] bool readChunkedBody(std::string& buffer, std::string& out_body) {
        char chunk[4096];
        size_t pos = 0;
        while (true) {
            size_t line_end = buffer.find("\r\n", pos);
            while (line_end == std::string::npos) {
                const ssize_t n = ::recv(sock_fd_, chunk, sizeof(chunk), 0);
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    return false;
                }
                buffer.append(chunk, static_cast<size_t>(n));
                line_end = buffer.find("\r\n", pos);
            }

            const std::string size_str = buffer.substr(pos, line_end - pos);
            const size_t chunk_size = std::strtoul(size_str.c_str(), nullptr, 16);
            pos = line_end + 2;

            if (chunk_size == 0) {
                return true;
            }

            while (buffer.size() < pos + chunk_size + 2) {
                const ssize_t n = ::recv(sock_fd_, chunk, sizeof(chunk), 0);
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    return false;
                }
                buffer.append(chunk, static_cast<size_t>(n));
            }

            out_body.append(buffer.data() + pos, chunk_size);
            pos += chunk_size + 2;
        }
    }
};

} // namespace httplib

#endif

