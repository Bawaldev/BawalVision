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
#include "bawal/lambda/handler.hpp"
#include "bawal/server/rolling_image_store.hpp"
#include "bawal/vision/classifier.hpp"
#include <opencv2/core/version.hpp>

static_assert(CV_VERSION_MAJOR >= 4, "BawalVision backend strictly requires OpenCV 4+.");

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

    std::cout << "[INFO] BawalVision C++ Backend initializing...\n";
    std::cout << "[INFO] OpenCV Version: " << CV_VERSION << " (Verified OpenCV4 Modern Pipeline)\n";

    const bool lan_mode = (envOr("BAWAL_LAN_MODE", "0") == "1");
    bawal::backend::ServerConfig config;
    config.lan_mode = lan_mode;
    const std::string default_bind = lan_mode ? "0.0.0.0" : "127.0.0.1";
    config.bind_address = envOr("CROW_BIND", default_bind.c_str());
    config.port = envPort();
    config.device_token = envOr("BAWAL_DEVICE_TOKEN", "");
    config.admin_token = envOr("BAWAL_ADMIN_TOKEN", "");

    if (lan_mode || config.bind_address == "0.0.0.0") {
        if (config.device_token.empty() || config.admin_token.empty()) {
            std::cerr << "[FATAL] Binding 0.0.0.0 / LAN Mode requires both BAWAL_DEVICE_TOKEN and BAWAL_ADMIN_TOKEN to be set!\n"
                      << "        Run 'just setup' to generate tokens. Refusing to start in unauthenticated fail-open mode.\n";
            return 1;
        }
        std::cout << "[INFO] LAN Mode enabled: binding " << config.bind_address << " with device and admin auth active\n";
    } else {
        std::cout << "[INFO] Localhost Mode: binding " << config.bind_address << " (set BAWAL_LAN_MODE=1 for LAN ingress)\n";
    }
    if (!config.device_token.empty()) {
        std::cout << "[INFO] Device auth enabled: X-Device-Token verification active\n";
    }
    if (!config.admin_token.empty()) {
        std::cout << "[INFO] Admin auth enabled: X-Admin-Token verification active for control endpoints\n";
    }

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

    bawal::server::RollingImageStore image_store("./data/inspections", 50);

    const std::string model_path = bawal::backend::resolveModelPath();
    bawal::backend::VisionWorker vision(state, model_path, &database, &image_store);
    vision.start();

    // Serverless-compatible inspection handler exposed under
    // POST /api/v1/lambda/inspect. It owns a separate pipeline and stays lazy
    // so the always-on backend does not load a second copy of the model until
    // the endpoint is actually used.
    bawal::lambda::VisionLambdaHandler lambda_handler;
    bawal::lambda::LambdaConfig lambda_config;
    lambda_config.model_path = model_path;
    lambda_config.auth_token = envOr("BAWAL_LAMBDA_AUTH_TOKEN", "");
    lambda_config.enforce_http_auth = false;
    lambda_config.enable_prewarm = envOr("BAWAL_LAMBDA_PREWARM", "0") == "1";
    const std::string lambda_enabled = envOr("BAWAL_LAMBDA_ENABLED", "1");
    if (lambda_enabled != "0" && lambda_enabled != "false") {
        if (!lambda_handler.initialize(lambda_config)) {
            std::cerr << "[WARN] Lambda inspection handler initialization failed\n";
        }
    }

    // Dedicated classifier for ZIP batch inspection. A separate instance keeps
    // batch jobs from contending with (or racing) the live inference pipeline.
    bawal::vision::FoodClassifier batch_classifier;
    if (!batch_classifier.init(model_path, 2)) {
        std::cerr << "[WARN] Batch classifier unavailable (model: " << model_path
                  << "); POST /api/v1/inspections/batch-zip will return 503\n";
    }

    // Bind the UDS notifier only after the consumer is listening. This keeps a
    // restarted process from reusing a socket left by a previous run.
    for (int i = 0; i < 40 && !vision.consumerReady(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!vision.consumerReady()) {
        std::cerr << "[WARN] SHM consumer not ready yet; notifier will retry\n";
    }
    producer.startNotify();

    bawal::backend::MjpegIngress mjpeg_ingress(state, producer);
    mjpeg_ingress.start();

    const auto peer_callback = [&state, &mjpeg_ingress](const std::string& ip) {
        state.updatePeerIp(ip);
        mjpeg_ingress.setPeerIp(ip);
    };

    bawal::backend::BackendApp app(state, producer, config, &database, &image_store);
    app.setPeerCallback(peer_callback);
    app.setBatchClassifier(&batch_classifier);
    if (lambda_enabled != "0" && lambda_enabled != "false") {
        app.setLambdaHandler(&lambda_handler);
    }
    app.configure();

    // ESP32-S3 nodes locate the backend through this UDP responder when their
    // cached/compiled IP is stale (DHCP change, Docker bridge IP, etc.).
    bawal::backend::DiscoveryResponder discovery;
    discovery.setPeerCallback(peer_callback);
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
