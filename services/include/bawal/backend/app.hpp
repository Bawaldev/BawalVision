#pragma once

#include <crow_all.h>

#include <cstdint>
#include <string>

#include "bawal/backend/state.hpp"
#include "bawal/ipc/shm_producer.hpp"

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
               Database* database = nullptr);

    void configure();
    int run();
    void stop();

    [[nodiscard]] crow::App<crow::CORSHandler>& app() noexcept { return m_app; }

private:
    void registerRoutes();
    void registerCameraRoutes();
    void registerTelemetryRoutes();
    void registerInspectionRoutes();
    void registerExampleRoutes();
    void registerCompatibilityRoutes();

    BackendState& m_state;
    ipc::ShmProducer& m_producer;
    ServerConfig m_config;
    Database* m_database{nullptr};
    crow::App<crow::CORSHandler> m_app;
};

} // namespace bawal::backend
