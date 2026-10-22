#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "bawal/backend/state.hpp"

namespace bawal::backend {

struct InspectionFoodRow {
    std::string name;
    double similarity_rate{0.0};
    double spoilage_score{0.0};
    std::string status;
};

struct InspectionRecordRow {
    std::string id;
    std::string device_id;
    std::string firmware_version;
    std::string created_at;
    std::string image_s3_uri;
    double cpu_temperature{0.0};
    double temperature{0.0};
    int cpu_load{0};
    bool is_spoiled{false};
    double t_safe_hours{0.0};
    long long frame_id{0};
    double lux_luminance{0.0};
    std::string white_balance_status;
    int color_temperature{6500};
    std::string engine_pipeline;
    std::vector<InspectionFoodRow> foods;
};

struct DatabaseConfig {
    std::string host{"postgres"};
    std::string port{"5432"};
    std::string user{"postgres"};
    std::string password{"postgres"};
    std::string dbname{"bawal_vision"};
    std::string sslmode{"disable"};

    [[nodiscard]] static DatabaseConfig fromEnvironment();
};

/**
 * @brief Minimal libpq persistence for inspection snapshots.
 *
 * The Crow HTTP layer and the vision thread stay usable when PostgreSQL is
 * unavailable: every call fails soft and the backend keeps serving in-memory
 * snapshots. Only the vision worker writes rows.
 */
class Database {
public:
    explicit Database(DatabaseConfig config);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool connect();
    void disconnect();

    [[nodiscard]] bool ready() const;
    [[nodiscard]] long long inspectionCount();
    [[nodiscard]] std::vector<InspectionRecordRow> listInspections(int limit = 100);

    bool saveInspection(std::string_view device_id,
                        uint32_t frame_id,
                        const DetectionSoA& detections,
                        const SceneOpticalSoA& optical,
                        std::string_view engine_pipeline);

private:
    bool ensureConnectedLocked();
    bool ensureDeviceLocked(std::string_view device_id);
    bool insertInspectionLocked(std::string_view device_id,
                                uint32_t frame_id,
                                const DetectionSoA& detections,
                                const SceneOpticalSoA& optical,
                                std::string_view engine_pipeline);

    DatabaseConfig m_config;
    mutable std::mutex m_mutex;
    void* m_conn{nullptr};
    bool m_ready{false};
    uint64_t m_last_connect_attempt_ms{0};
};

} // namespace bawal::backend
