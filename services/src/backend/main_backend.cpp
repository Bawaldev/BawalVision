#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "bawal/backend/app.hpp"
#include "bawal/backend/state.hpp"
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

    bawal::backend::VisionWorker vision(state, bawal::backend::resolveModelPath());
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

    bawal::backend::BackendApp app(state, producer, config);
    app.configure();

    std::cout << "[INFO] bawal::backend C++ Crow server on http://"
              << config.bind_address << ':' << config.port
              << " (SHM: " << bawal::ipc::SHM_NAME << ")\n";

    const int rc = app.run();
    vision.stop();
    producer.stopNotify();
    return rc;
}
