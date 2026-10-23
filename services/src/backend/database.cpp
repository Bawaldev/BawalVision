#include "bawal/backend/database.hpp"

#include "bawal/vision/image/batch_inspector.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef BAWAL_HAVE_POSTGRES
#include <libpq-fe.h>
#endif

namespace bawal::backend {
namespace {

std::string envOr(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return value;
}

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string boundedText(const char* data, size_t capacity) {
    return std::string(data, strnlen(data, capacity));
}

const char* statusName(uint8_t status) {
    switch (static_cast<FoodStatus>(status)) {
        case FoodStatus::Pass: return "PASS";
        case FoodStatus::Review: return "REVIEW";
        case FoodStatus::Reject: return "REJECT";
        default: return "UNKNOWN";
    }
}

std::string sanitizeId(std::string_view device_id) {
    std::string id(device_id);
    if (id.size() > 63) {
        id.resize(63);
    }
    return id.empty() ? std::string("DEFAULT_CAMERA") : id;
}

double toDouble(std::string_view value) {
    try {
        return std::stod(std::string(value));
    } catch (const std::exception&) {
        return 0.0;
    }
}

long long toLongLong(std::string_view value) {
    try {
        return std::stoll(std::string(value));
    } catch (const std::exception&) {
        return 0;
    }
}

int toInt(std::string_view value) {
    return static_cast<int>(toLongLong(value));
}

bool toBool(std::string_view value) {
    return !value.empty() && (value.front() == 't' || value.front() == 'T' ||
                              value.front() == '1');
}

#ifdef BAWAL_HAVE_POSTGRES
bool resultOk(const PGresult* result) {
    if (result == nullptr) {
        return false;
    }
    const ExecStatusType status = PQresultStatus(result);
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
}

const char* tupleValue(const PGresult* result, int row, int column) {
    if (result == nullptr || row >= PQntuples(result) || column >= PQnfields(result)) {
        return "";
    }
    return PQgetvalue(result, row, column);
}
#endif

} // namespace

DatabaseConfig DatabaseConfig::fromEnvironment() {
    DatabaseConfig config;
    config.host = envOr("DB_HOST", "postgres");
    config.port = envOr("DB_PORT", "5432");
    config.user = envOr("DB_USER", "postgres");
    config.password = envOr("DB_PASSWORD", "postgres");
    config.dbname = envOr("DB_NAME", "bawal_vision");
    config.sslmode = envOr("DB_SSLMODE", "disable");
    return config;
}

Database::Database(DatabaseConfig config) : m_config(std::move(config)) {}

Database::~Database() {
    disconnect();
}

bool Database::connect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_connect_attempt_ms = 0;
    return ensureConnectedLocked();
}

void Database::disconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
#ifdef BAWAL_HAVE_POSTGRES
    if (m_conn != nullptr) {
        PQfinish(static_cast<PGconn*>(m_conn));
        m_conn = nullptr;
    }
#endif
    m_ready = false;
}

bool Database::ready() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ready;
}

bool Database::ensureConnectedLocked() {
#ifdef BAWAL_HAVE_POSTGRES
    if (m_conn != nullptr) {
        if (PQstatus(static_cast<PGconn*>(m_conn)) == CONNECTION_OK) {
            m_ready = true;
            return true;
        }
        PQfinish(static_cast<PGconn*>(m_conn));
        m_conn = nullptr;
        m_ready = false;
    }

    const uint64_t now = nowMs();
    if (m_last_connect_attempt_ms != 0 && now - m_last_connect_attempt_ms < 5000) {
        return false;
    }
    m_last_connect_attempt_ms = now;

    const char* keywords[] = {
        "host", "port", "user", "password", "dbname", "sslmode", "connect_timeout", nullptr
    };
    const char* values[] = {
        m_config.host.c_str(),
        m_config.port.c_str(),
        m_config.user.c_str(),
        m_config.password.c_str(),
        m_config.dbname.c_str(),
        m_config.sslmode.c_str(),
        "3",
        nullptr
    };

    PGconn* conn = PQconnectdbParams(keywords, values, 0);
    if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
        const char* error = conn != nullptr ? PQerrorMessage(conn) : "out of memory";
        std::cerr << "[DB] PostgreSQL connection failed: " << error;
        if (conn != nullptr) {
            PQfinish(conn);
        }
        m_ready = false;
        return false;
    }

    m_conn = conn;
    m_ready = true;
    std::cout << "[DB] Connected to PostgreSQL " << m_config.host << ':' << m_config.port
              << '/' << m_config.dbname << '\n';
    return true;
#else
    return false;
#endif
}

bool Database::ensureDeviceLocked(std::string_view device_id, std::string_view device_type) {
#ifdef BAWAL_HAVE_POSTGRES
    if (!ensureConnectedLocked()) {
        return false;
    }

    const std::string id = sanitizeId(device_id);
    const std::string type = device_type.empty() ? std::string("ESP32") : std::string(device_type);
    const char* params[] = {id.c_str(), type.c_str(), "crow-cpp-1.0"};
    PGresult* result = PQexecParams(
        static_cast<PGconn*>(m_conn),
        "INSERT INTO devices (id, device_type, firmware_version, status, last_seen_at) "
        "VALUES ($1, $2, $3, 'ACTIVE', NOW()) "
        "ON CONFLICT (id) DO UPDATE "
        "SET device_type = EXCLUDED.device_type, last_seen_at = NOW(), "
        "    updated_at = NOW(), status = 'ACTIVE'",
        3, nullptr, params, nullptr, nullptr, 0);
    const bool ok = resultOk(result);
    if (!ok) {
        std::cerr << "[DB] Device upsert failed: "
                  << PQerrorMessage(static_cast<PGconn*>(m_conn));
    }
    PQclear(result);
    return ok;
#else
    (void)device_id;
    (void)device_type;
    return false;
#endif
}

bool Database::insertInspectionLocked(std::string_view device_id,
                                      uint32_t frame_id,
                                      const DetectionSoA& detections,
                                      const SceneOpticalSoA& optical,
                                      std::string_view engine_pipeline) {
#ifdef BAWAL_HAVE_POSTGRES
    PGconn* conn = static_cast<PGconn*>(m_conn);
    const std::string id = sanitizeId(device_id);

    bool is_spoiled = false;
    const uint32_t count = std::min<uint32_t>(detections.count, DetectionSoA::kMaxDetections);
    for (uint32_t i = 0; i < count; ++i) {
        if (static_cast<FoodStatus>(detections.status[i]) == FoodStatus::Reject) {
            is_spoiled = true;
            break;
        }
    }

    const std::string lux = std::to_string(optical.lux_luminance);
    const std::string white_balance = boundedText(
        optical.white_balance_status, sizeof(optical.white_balance_status));
    const std::string color_temperature = std::to_string(optical.color_temperature);
    const std::string pipeline = engine_pipeline.empty()
        ? std::string("crow-cpp-inline-vision")
        : std::string(engine_pipeline);
    const std::string frame = std::to_string(frame_id);

    const char* params[] = {
        id.c_str(),
        "crow-cpp-1.0",
        frame.c_str(),
        is_spoiled ? "t" : "f",
        lux.c_str(),
        white_balance.c_str(),
        color_temperature.c_str(),
        pipeline.c_str()
    };

    PGresult* result = PQexecParams(
        conn,
        "INSERT INTO inspections "
        "(device_id, firmware_version, frame_id, cpu_temperature, temperature, cpu_load, "
        " is_spoiled, t_safe_hours, lux_luminance, white_balance_status, "
        " color_temperature, engine_pipeline) "
        "VALUES ($1, $2, $3, 35.00, 25.00, 0, $4, 0.00, $5, $6, $7, $8) "
        "RETURNING id",
        8, nullptr, params, nullptr, nullptr, 0);
    if (!resultOk(result)) {
        std::cerr << "[DB] Inspection insert failed: " << PQerrorMessage(conn);
        PQclear(result);
        return false;
    }

    const std::string inspection_id = tupleValue(result, 0, 0);
    PQclear(result);

    for (uint32_t i = 0; i < count; ++i) {
        std::string food_name = boundedText(
            detections.class_name[i], DetectionSoA::kClassNameBytes);
        if (food_name.empty()) {
            food_name = "unknown";
        }

        const char* food_params[] = {food_name.c_str()};
        PGresult* food = PQexecParams(
            conn,
            "INSERT INTO food_items (name) VALUES ($1) "
            "ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.name "
            "RETURNING id",
            1, nullptr, food_params, nullptr, nullptr, 0);
        if (!resultOk(food)) {
            std::cerr << "[DB] Food item upsert failed: " << PQerrorMessage(conn);
            PQclear(food);
            continue;
        }
        const std::string food_id = tupleValue(food, 0, 0);
        PQclear(food);

        const std::string similarity = std::to_string(
            std::clamp(detections.confidence[i], 0.0f, 1.0f));
        const std::string spoilage = std::to_string(
            std::clamp(detections.spoilage_score[i], 0.0f, 100.0f));
        const char* status = statusName(detections.status[i]);

        const char* item_params[] = {
            inspection_id.c_str(),
            food_id.c_str(),
            similarity.c_str(),
            spoilage.c_str(),
            status
        };
        PGresult* item = PQexecParams(
            conn,
            "INSERT INTO inspection_foods "
            "(inspection_id, food_item_id, similarity_rate, spoilage_score, status) "
            "VALUES ($1, $2, $3, $4, $5) "
            "ON CONFLICT (inspection_id, food_item_id) DO UPDATE "
            "SET similarity_rate = GREATEST(inspection_foods.similarity_rate, EXCLUDED.similarity_rate), "
            "    spoilage_score = GREATEST(inspection_foods.spoilage_score, EXCLUDED.spoilage_score), "
            "    status = EXCLUDED.status",
            5, nullptr, item_params, nullptr, nullptr, 0);
        if (!resultOk(item)) {
            std::cerr << "[DB] Inspection food insert failed: " << PQerrorMessage(conn);
        }
        PQclear(item);
    }

    return true;
#else
    (void)device_id;
    (void)frame_id;
    (void)detections;
    (void)optical;
    (void)engine_pipeline;
    return false;
#endif
}

bool Database::saveInspection(std::string_view device_id,
                              uint32_t frame_id,
                              const DetectionSoA& detections,
                              const SceneOpticalSoA& optical,
                              std::string_view engine_pipeline) {
#ifdef BAWAL_HAVE_POSTGRES
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!ensureConnectedLocked()) {
        return false;
    }
    if (!ensureDeviceLocked(device_id)) {
        return false;
    }

    PGconn* conn = static_cast<PGconn*>(m_conn);
    PGresult* begin = PQexec(conn, "BEGIN");
    const bool begin_ok = resultOk(begin);
    PQclear(begin);
    if (!begin_ok) {
        return false;
    }

    if (!insertInspectionLocked(device_id, frame_id, detections, optical, engine_pipeline)) {
        PGresult* rollback = PQexec(conn, "ROLLBACK");
        PQclear(rollback);
        return false;
    }

    PGresult* commit = PQexec(conn, "COMMIT");
    const bool commit_ok = resultOk(commit);
    PQclear(commit);
    return commit_ok;
#else
    (void)device_id;
    (void)frame_id;
    (void)detections;
    (void)optical;
    (void)engine_pipeline;
    return false;
#endif
}

long long Database::inspectionCount() {
#ifdef BAWAL_HAVE_POSTGRES
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!ensureConnectedLocked()) {
        return -1;
    }
    PGresult* result = PQexec(static_cast<PGconn*>(m_conn),
                              "SELECT COUNT(*) FROM inspections");
    if (!resultOk(result)) {
        PQclear(result);
        return -1;
    }
    const long long count = std::strtoll(tupleValue(result, 0, 0), nullptr, 10);
    PQclear(result);
    return count;
#else
    return -1;
#endif
}

std::vector<InspectionRecordRow> Database::listInspections(int limit) {
    std::vector<InspectionRecordRow> records;
#ifdef BAWAL_HAVE_POSTGRES
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!ensureConnectedLocked()) {
        return records;
    }

    limit = std::clamp(limit, 1, 500);
    const std::string limit_text = std::to_string(limit);
    const char* params[] = {limit_text.c_str()};

    PGresult* result = PQexecParams(
        static_cast<PGconn*>(m_conn),
        "SELECT i.id::text, i.device_id, i.firmware_version, "
        "to_char(i.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), "
        "COALESCE(i.image_s3_uri, ''), i.cpu_temperature::text, i.temperature::text, "
        "i.cpu_load::text, i.is_spoiled::text, i.t_safe_hours::text, i.frame_id::text, "
        "i.lux_luminance::text, i.white_balance_status, i.color_temperature::text, "
        "i.engine_pipeline, COALESCE(fi.name, ''), "
        "COALESCE(f.similarity_rate::text, '0'), COALESCE(f.spoilage_score::text, '0'), "
        "COALESCE(f.status, '') "
        "FROM inspections i "
        "LEFT JOIN inspection_foods f ON f.inspection_id = i.id "
        "LEFT JOIN food_items fi ON fi.id = f.food_item_id "
        "ORDER BY i.created_at DESC, i.id DESC LIMIT $1::int",
        1, nullptr, params, nullptr, nullptr, 0);
    if (!resultOk(result)) {
        std::cerr << "[DB] Inspection list failed: "
                  << PQerrorMessage(static_cast<PGconn*>(m_conn));
        PQclear(result);
        return records;
    }

    const int row_count = PQntuples(result);
    records.reserve(static_cast<std::size_t>(std::max(0, row_count)));

    for (int row = 0; row < row_count; ++row) {
        const std::string id = tupleValue(result, row, 0);
        auto existing = std::find_if(
            records.begin(), records.end(),
            [&id](const InspectionRecordRow& record) { return record.id == id; });

        if (existing == records.end()) {
            InspectionRecordRow record;
            record.id = id;
            record.device_id = tupleValue(result, row, 1);
            record.firmware_version = tupleValue(result, row, 2);
            record.created_at = tupleValue(result, row, 3);
            record.image_s3_uri = tupleValue(result, row, 4);
            record.cpu_temperature = toDouble(tupleValue(result, row, 5));
            record.temperature = toDouble(tupleValue(result, row, 6));
            record.cpu_load = toInt(tupleValue(result, row, 7));
            record.is_spoiled = toBool(tupleValue(result, row, 8));
            record.t_safe_hours = toDouble(tupleValue(result, row, 9));
            record.frame_id = toLongLong(tupleValue(result, row, 10));
            record.lux_luminance = toDouble(tupleValue(result, row, 11));
            record.white_balance_status = tupleValue(result, row, 12);
            record.color_temperature = toInt(tupleValue(result, row, 13));
            record.engine_pipeline = tupleValue(result, row, 14);
            records.push_back(std::move(record));
            existing = records.end() - 1;
        }

        const std::string food_name = tupleValue(result, row, 15);
        if (!food_name.empty()) {
            InspectionFoodRow food;
            food.name = food_name;
            food.similarity_rate = toDouble(tupleValue(result, row, 16));
            food.spoilage_score = toDouble(tupleValue(result, row, 17));
            food.status = tupleValue(result, row, 18);
            existing->foods.push_back(std::move(food));
        }
    }

    PQclear(result);
#else
    (void)limit;
#endif
    return records;
}

bawal::vision::image::GrafanaAnalytics Database::fetchGrafanaAnalytics() {
    bawal::vision::image::GrafanaAnalytics analytics;
#ifdef BAWAL_HAVE_POSTGRES
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!ensureConnectedLocked()) {
        return analytics;
    }

    PGconn* conn = static_cast<PGconn*>(m_conn);

    // Headline KPIs: identical aggregates to the Grafana stat panels, so the
    // PDF and the dashboard can never drift apart.
    PGresult* headline = PQexec(
        conn,
        "SELECT (SELECT COUNT(*) FROM inspections), "
        "       (SELECT COUNT(*) FROM inspections WHERE created_at >= NOW() - INTERVAL '24 hours'), "
        "       (SELECT COUNT(*) FROM devices), "
        "       COALESCE(ROUND(AVG(similarity_rate) * 100, 1), 0), "
        "       COALESCE(ROUND(COUNT(*) FILTER (WHERE status = 'PASS')::numeric "
        "                      / NULLIF(COUNT(*), 0) * 100, 1), 0), "
        "       COALESCE(ROUND(AVG(spoilage_score), 1), 0) "
        "FROM inspection_foods");
    if (!resultOk(headline)) {
        std::cerr << "[DB] Grafana analytics headline query failed: "
                  << PQerrorMessage(conn);
        PQclear(headline);
        return analytics;
    }

    analytics.available = true;
    analytics.total_inspections = static_cast<uint64_t>(toLongLong(tupleValue(headline, 0, 0)));
    analytics.inspections_24h = static_cast<uint64_t>(toLongLong(tupleValue(headline, 0, 1)));
    analytics.tracked_devices = static_cast<uint64_t>(toLongLong(tupleValue(headline, 0, 2)));
    analytics.avg_similarity_pct = toDouble(tupleValue(headline, 0, 3));
    analytics.fresh_food_rate = toDouble(tupleValue(headline, 0, 4));
    analytics.avg_spoilage_pct = toDouble(tupleValue(headline, 0, 5));
    PQclear(headline);

    // Per-class fleet quality (top 8 classes by detection volume).
    PGresult* classes = PQexec(
        conn,
        "SELECT fi.name, COUNT(*), "
        "       COALESCE(ROUND(AVG(ifo.similarity_rate) * 100, 1), 0), "
        "       COALESCE(ROUND(AVG(ifo.spoilage_score), 1), 0), "
        "       COALESCE(ROUND(COUNT(*) FILTER (WHERE ifo.status = 'PASS')::numeric "
        "                      / NULLIF(COUNT(*), 0) * 100, 1), 0) "
        "FROM inspection_foods ifo "
        "JOIN food_items fi ON fi.id = ifo.food_item_id "
        "GROUP BY fi.name ORDER BY COUNT(*) DESC LIMIT 8");
    if (resultOk(classes)) {
        const int rows = PQntuples(classes);
        analytics.class_stats.reserve(static_cast<size_t>(std::max(0, rows)));
        for (int row = 0; row < rows; ++row) {
            bawal::vision::image::GrafanaClassStat stat;
            bawal::vision::image::assignFixedString(
                stat.class_name, tupleValue(classes, row, 0));
            stat.detections = static_cast<uint64_t>(toLongLong(tupleValue(classes, row, 1)));
            stat.avg_similarity_pct = toDouble(tupleValue(classes, row, 2));
            stat.avg_spoilage_pct = toDouble(tupleValue(classes, row, 3));
            stat.pass_rate = toDouble(tupleValue(classes, row, 4));
            analytics.class_stats.push_back(std::move(stat));
        }
    } else {
        std::cerr << "[DB] Grafana analytics class query failed: "
                  << PQerrorMessage(conn);
    }
    PQclear(classes);

    // 7-day inspection volume / pass-rate trend.
    PGresult* daily = PQexec(
        conn,
        "SELECT to_char(date_trunc('day', i.created_at), 'MM-DD') AS day, "
        "       COUNT(DISTINCT i.id), "
        "       COALESCE(ROUND(COUNT(*) FILTER (WHERE ifo.status = 'PASS')::numeric "
        "                      / NULLIF(COUNT(*), 0) * 100, 1), 0) "
        "FROM inspections i "
        "JOIN inspection_foods ifo ON ifo.inspection_id = i.id "
        "WHERE i.created_at >= NOW() - INTERVAL '7 days' "
        "GROUP BY 1 ORDER BY 1");
    if (resultOk(daily)) {
        const int rows = PQntuples(daily);
        analytics.daily.reserve(static_cast<size_t>(std::max(0, rows)));
        for (int row = 0; row < rows; ++row) {
            bawal::vision::image::GrafanaDailyPoint point;
            bawal::vision::image::assignFixedString(point.day, tupleValue(daily, row, 0));
            point.inspections = static_cast<uint64_t>(toLongLong(tupleValue(daily, row, 1)));
            point.pass_rate = toDouble(tupleValue(daily, row, 2));
            analytics.daily.push_back(std::move(point));
        }
    } else {
        std::cerr << "[DB] Grafana analytics daily query failed: "
                  << PQerrorMessage(conn);
    }
    PQclear(daily);
#endif
    return analytics;
}

bool Database::saveBatchInspection(
    const bawal::vision::image::BatchReportSummary& summary) {
#ifdef BAWAL_HAVE_POSTGRES
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!ensureConnectedLocked()) {
        return false;
    }
    if (!ensureDeviceLocked("BATCH_IMG_UPLOAD", "BATCH_UPLOADER")) {
        return false;
    }

    PGconn* conn = static_cast<PGconn*>(m_conn);
    PGresult* begin = PQexec(conn, "BEGIN");
    const bool begin_ok = resultOk(begin);
    PQclear(begin);
    if (!begin_ok) {
        return false;
    }

    bool ok = true;
    for (const auto& item : summary.items) {
        const std::string frame = std::to_string(item.index);
        const std::string image_uri = item.annotated_image_path;
        const char* inspection_params[] = {
            "BATCH_IMG_UPLOAD",
            "img-batch-1.0",
            frame.c_str(),
            item.accepted ? "f" : "t",
            "0.00",
            image_uri.c_str(),
            "img-batch"
        };

        PGresult* inspection = PQexecParams(
            conn,
            "INSERT INTO inspections "
            "(device_id, firmware_version, frame_id, cpu_temperature, temperature, cpu_load, "
            " is_spoiled, t_safe_hours, image_s3_uri, engine_pipeline) "
            "VALUES ($1, $2, $3, 35.00, 25.00, 0, $4, $5, $6, $7) RETURNING id",
            7, nullptr, inspection_params, nullptr, nullptr, 0);
        if (!resultOk(inspection)) {
            std::cerr << "[DB] Batch inspection insert failed: " << PQerrorMessage(conn);
            PQclear(inspection);
            ok = false;
            break;
        }
        const std::string inspection_id = tupleValue(inspection, 0, 0);
        PQclear(inspection);

        for (const auto& detection : item.detections) {
            std::string food_name = std::string(detection.class_name);
            if (food_name.empty()) {
                food_name = "unknown";
            }

            const char* food_params[] = {food_name.c_str()};
            PGresult* food = PQexecParams(
                conn,
                "INSERT INTO food_items (name) VALUES ($1) "
                "ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.name RETURNING id",
                1, nullptr, food_params, nullptr, nullptr, 0);
            if (!resultOk(food)) {
                std::cerr << "[DB] Batch food item upsert failed: " << PQerrorMessage(conn);
                PQclear(food);
                continue;
            }
            const std::string food_id = tupleValue(food, 0, 0);
            PQclear(food);

            const std::string similarity = std::to_string(
                std::clamp(detection.confidence, 0.0f, 1.0f));
            const std::string spoilage = std::to_string(
                std::clamp(detection.spoilage.discoloration_ratio * 100.0f, 0.0f, 100.0f));
            const char* status = bawal::vision::image::qualityStatusName(
                bawal::vision::image::classifyQuality(detection));

            const char* item_params[] = {
                inspection_id.c_str(),
                food_id.c_str(),
                similarity.c_str(),
                spoilage.c_str(),
                status
            };
            PGresult* food_row = PQexecParams(
                conn,
                "INSERT INTO inspection_foods "
                "(inspection_id, food_item_id, similarity_rate, spoilage_score, status) "
                "VALUES ($1, $2, $3, $4, $5) "
                "ON CONFLICT (inspection_id, food_item_id) DO UPDATE "
                "SET similarity_rate = GREATEST(inspection_foods.similarity_rate, EXCLUDED.similarity_rate), "
                "    spoilage_score = GREATEST(inspection_foods.spoilage_score, EXCLUDED.spoilage_score), "
                "    status = EXCLUDED.status",
                5, nullptr, item_params, nullptr, nullptr, 0);
            if (!resultOk(food_row)) {
                std::cerr << "[DB] Batch inspection food insert failed: "
                          << PQerrorMessage(conn);
            }
            PQclear(food_row);
        }
    }

    PGresult* finish = PQexec(conn, ok ? "COMMIT" : "ROLLBACK");
    const bool finish_ok = resultOk(finish);
    PQclear(finish);
    return ok && finish_ok;
#else
    (void)summary;
    return false;
#endif
}

} // namespace bawal::backend
