#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <curl/curl.h>

#include "bawal/backend/app.hpp"
#include "bawal/backend/database.hpp"
#include "bawal/backend/discovery.hpp"
#include "bawal/backend/state.hpp"
#include "bawal/backend/mjpeg_ingress.hpp"
#include "bawal/backend/vision_worker.hpp"
#include "bawal/ipc/shm_producer.hpp"

namespace {

std::string envOr(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return value;
}

uint16_t envPort() {
    const std::string value = envOr("CROW_PORT", envOr("BACKEND_PORT", "8080").c_str());
    char* end = nullptr;
    const unsigned long port = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || port == 0 || port > 65535) {
        return 8080;
    }
    return static_cast<uint16_t>(port);
}

uint16_t envPortOrDefault(const char* key, uint16_t fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long port = std::strtoul(value, &end, 10);
    if (end == value || port == 0 || port > 65535) {
        return fallback;
    }
    return static_cast<uint16_t>(port);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    bawal::backend::ServerConfig config;
    config.bind_address = envOr("CROW_BIND", "0.0.0.0");
    config.port = envPort();

    bawal::ipc::ShmProducer producer;
    if (!producer.init()) {
        std::cerr << "[FATAL] Unable to initialize POSIX SHM producer ("
                  << bawal::ipc::SHM_NAME << ")\n";
        return 1;
    }

    bawal::backend::BackendState state;
    curl_global_init(CURL_GLOBAL_DEFAULT);

    bawal::backend::DatabaseConfig db_config =
        bawal::backend::DatabaseConfig::fromEnvironment();
    bawal::backend::Database database(db_config);
    if (!database.connect()) {
        std::cerr << "[WARN] PostgreSQL unavailable at " << db_config.host << ':'
                  << db_config.port << '/' << db_config.dbname
                  << "; continuing with in-memory inspection snapshots only\n";
    }

    bawal::backend::VisionWorker vision(state, bawal::backend::resolveModelPath(),
                                        &database);
    vision.start();

    // Bind the UDS notifier only after the consumer is listening. This keeps a
    // restarted process from reusing a socket left by a previous run.
    for (int i = 0; i < 40 && !vision.consumerReady(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!vision.consumerReady()) {
        std::cerr << "[WARN] SHM consumer not ready yet; notifier will retry\n";
    }
    producer.startNotify();

    bawal::backend::BackendApp app(state, producer, config, &database);
    app.configure();

    // ESP32-S3 nodes locate the backend through this UDP responder when their
    // cached/compiled IP is stale (DHCP change, Docker bridge IP, etc.).
    bawal::backend::DiscoveryResponder discovery;
    bawal::backend::MjpegIngress mjpeg_ingress(state, producer);
    mjpeg_ingress.start();
    discovery.setPeerCallback(
        [&state, &mjpeg_ingress](const std::string& ip) {
            state.updatePeerIp(ip);
            mjpeg_ingress.setPeerIp(ip);
        });
    const uint16_t discovery_port = envPortOrDefault("DISCOVERY_PORT", 8765);
    if (discovery.start(discovery_port, config.port)) {
        std::cout << "[INFO] UDP backend discovery responder on :" << discovery_port
                  << " (HTTP port " << config.port << ")\n";
    } else {
        std::cerr << "[WARN] UDP discovery responder disabled (port " << discovery_port
                  << " unavailable)\n";
    }

    std::cout << "[INFO] bawal::backend C++ Crow server on http://"
              << config.bind_address << ':' << config.port
              << " (SHM: " << bawal::ipc::SHM_NAME << ")\n";

    const int rc = app.run();
    discovery.stop();
    mjpeg_ingress.stop();
    vision.stop();
    producer.stopNotify();
    curl_global_cleanup();
    return rc;
}
