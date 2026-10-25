#pragma once

#include <crow_all.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "bawal/backend/state.hpp"
#include "bawal/ipc/shm_producer.hpp"

namespace bawal::server {
class RollingImageStore;
}

namespace bawal::vision {
class FoodClassifier;
}

namespace bawal::lambda {
class LambdaRuntimeHandler;
}

namespace bawal::backend {

class Database;

struct ServerConfig {
    std::string bind_address{"127.0.0.1"};
    uint16_t port{8080};
    bool lan_mode{false};
    std::string device_token;
    std::string admin_token;
};

/**
 * @brief Parsed frame ingress request (raw or JSON/base64 body plus headers).
 *
 * Fields are grouped 8-byte first and 1-byte last so the heavy string/vector
 * members never leave padding holes between the scalar metadata.
 */
struct alignas(8) FrameIngressRequest {
    std::string device_id;
    std::string camera_model;
    std::vector<uint8_t> image;
    uint32_t frame_id{0};
    uint32_t free_heap_internal{0};
    uint32_t free_heap_psram{0};
    float temperature{0.0f};
    uint16_t width{0};
    uint16_t height{0};
    uint8_t cpu_load{0};
    bool has_temperature{false};
    bool has_cpu_load{false};
    bool has_free_heap_internal{false};
    bool has_free_heap_psram{false};
    uint8_t reserved[4]{0}; ///< Explicit tail padding after the 1-byte flags
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

    /**
     * @brief Attach a dedicated classifier for ZIP batch inspection.
     *
     * Must be a separate FoodClassifier instance from the live VisionWorker
     * pipeline: batch jobs run on Crow worker threads and must never contend
     * with or race the 30 FPS live inference path.
     */
    void setBatchClassifier(bawal::vision::FoodClassifier* classifier) noexcept;

    /**
     * @brief Attach the serverless inspection handler exposed under
     *        `POST /api/v1/lambda/inspect`.
     *
     * The pointer must outlive the app. When null the Lambda routes report
     * 503 so the regular backend can still run without a serverless handler.
     */
    void setLambdaHandler(bawal::lambda::LambdaRuntimeHandler* handler) noexcept;

    /** Verify device token for frame and telemetry ingress. */
    [[nodiscard]] bool verifyDeviceAuth(const crow::request& req) const noexcept;

    /** Verify admin token for hardware actuation, config changes, and device resets. */
    [[nodiscard]] bool verifyAdminAuth(const crow::request& req) const noexcept;

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
    void registerLambdaRoutes();

    BackendState& m_state;
    ipc::ShmProducer& m_producer;
    ServerConfig m_config;
    Database* m_database{nullptr};
    server::RollingImageStore* m_image_store{nullptr};
    crow::App<crow::CORSHandler> m_app;
    std::atomic<double> m_confidence_threshold{0.35};
    std::function<void(const std::string&)> m_peer_callback;
    bawal::vision::FoodClassifier* m_batch_classifier{nullptr};
    bawal::lambda::LambdaRuntimeHandler* m_lambda_handler{nullptr};
    std::mutex m_batch_mutex; ///< Serializes ZIP batch jobs sharing one classifier.
};

} // namespace bawal::backend
