#include "bawal/backend/discovery.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char* kProbePrefix = "BAWAL_DISCOVER";

} // namespace

namespace bawal::backend {

DiscoveryResponder::~DiscoveryResponder() {
    stop();
}

bool DiscoveryResponder::start(uint16_t discovery_port, uint16_t http_port) {
    if (m_running.load()) {
        return true;
    }

    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::perror("[DISCOVERY] socket() failed");
        return false;
    }

    int reuse = 1;
    (void)::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int broadcast = 1;
    (void)::setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(discovery_port);

    if (::bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        std::perror("[DISCOVERY] bind() failed");
        ::close(sock);
        return false;
    }

    m_socket = sock;
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread([this, http_port]() { run(http_port); });
    return true;
}

void DiscoveryResponder::run(uint16_t http_port) {
    char hostname[128] = {0};
    if (::gethostname(hostname, sizeof(hostname) - 1) != 0) {
        std::snprintf(hostname, sizeof(hostname), "bawal-backend");
    }

    while (m_running.load(std::memory_order_acquire)) {
        pollfd descriptor{m_socket, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 500);
        if (ready <= 0) {
            continue;
        }

        char buffer[256] = {0};
        sockaddr_in source{};
        socklen_t source_length = sizeof(source);
        const ssize_t received = ::recvfrom(
            m_socket, buffer, sizeof(buffer) - 1, 0,
            reinterpret_cast<sockaddr*>(&source), &source_length);
        if (received <= 0) {
            continue;
        }
        buffer[received] = '\0';

        const std::string probe(buffer, static_cast<std::size_t>(received));
        if (probe.rfind(kProbePrefix, 0) != 0) {
            continue;
        }

        if (m_peer_callback) {
            char peer[INET_ADDRSTRLEN] = {0};
            if (::inet_ntop(AF_INET, &source.sin_addr, peer, sizeof(peer)) != nullptr) {
                m_peer_callback(peer);
            }
        }

        char reply[256] = {0};
        std::snprintf(reply, sizeof(reply),
                      "{\"service\":\"bawal-vision\",\"version\":\"1.0.0\","
                      "\"backend_port\":%u,\"hostname\":\"%s\"}",
                      static_cast<unsigned>(http_port), hostname);

        (void)::sendto(m_socket, reply, std::strlen(reply), 0,
                       reinterpret_cast<const sockaddr*>(&source), source_length);
    }
}

void DiscoveryResponder::stop() {
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }
}

} // namespace bawal::backend
