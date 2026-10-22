#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace bawal::backend {

/**
 * @brief UDP responder that lets ESP32-S3 edge nodes locate the backend after
 * DHCP or Wi-Fi changes.
 *
 * Edge nodes broadcast the `BAWAL_DISCOVER_V1` probe to port 8765 and use the
 * source address of the reply as the backend's real LAN IP. This replaces the
 * discovery goroutine that lived in the retired Go backend.
 */
class DiscoveryResponder {
public:
    using PeerCallback = std::function<void(const std::string&)>;

    DiscoveryResponder() = default;
    ~DiscoveryResponder();

    DiscoveryResponder(const DiscoveryResponder&) = delete;
    DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;

    /**
     * @brief Observe the source address of discovery probes.
     *
     * Docker's userland forwarding rewrites the HTTP client address, so the
     * probe source is the only reliable way to learn the node's LAN IP.
     * Must be set before start(); it is only invoked from the worker thread.
     */
    void setPeerCallback(PeerCallback callback) { m_peer_callback = std::move(callback); }

    bool start(uint16_t discovery_port, uint16_t http_port);
    void stop();

private:
    void run(uint16_t http_port);

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    int m_socket{-1};
    PeerCallback m_peer_callback;
};

} // namespace bawal::backend
