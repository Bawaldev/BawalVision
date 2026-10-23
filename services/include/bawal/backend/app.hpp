#pragma once

#include <crow_all.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "bawal/backend/state.hpp"
#include "bawal/ipc/shm_producer.hpp"

namespace bawal::server {
class RollingImageStore;
}

namespace bawal::backend {

class Database;

struct ServerConfig {
    std::string bind_address{"0.0.0.0"};
    uint16_t port{8080};
};

/**
 * @brief Crow HTTP front-end for the unified C++ backend.
 *
 * Only this web boundary uses OOP. Route handlers translate HTTP payloads into
 * POD/SHM operations; the vision pipeline remains a separate thread consuming
 * frames through bawal::ipc::ShmFrameConsumer.
 */
class BackendApp {
public:
    BackendApp(BackendState& state, ipc::ShmProducer& producer, ServerConfig config,
               Database* database = nullptr,
               server::RollingImageStore* image_store = nullptr);

    void configure();
    int run();
    void stop();

    /** Observe the source IP of device telemetry/frame requests. */
    void setPeerCallback(std::function<void(const std::string&)> callback);

    [[nodiscard]] crow::App<crow::CORSHandler>& app() noexcept { return m_app; }

private:
    void registerRoutes();
    void registerCameraRoutes();
    void registerTelemetryRoutes();
    void registerInspectionRoutes();
    void registerImageRoutes();
    void registerExampleRoutes();
    void registerCompatibilityRoutes();
    void registerDeviceRoutes();
    void registerSystemRoutes();

    BackendState& m_state;
    ipc::ShmProducer& m_producer;
    ServerConfig m_config;
    Database* m_database{nullptr};
    server::RollingImageStore* m_image_store{nullptr};
    crow::App<crow::CORSHandler> m_app;
    std::atomic<double> m_confidence_threshold{0.35};
    std::function<void(const std::string&)> m_peer_callback;
};

} // namespace bawal::backend
