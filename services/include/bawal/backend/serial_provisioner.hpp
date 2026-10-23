#pragma once

#include <string>
#include <string_view>

namespace bawal::backend {

/** Decoded `BAWAL_ACK:{...}` line emitted by the ESP32-S3 USB provisioning task. */
struct EspSerialAck {
    bool received{false};
    std::string status; ///< "wifi_provisioned", "ok" or "error".
    std::string device_id;
    std::string firmware_version;
    std::string ssid;       ///< SSID currently stored in the board's NVS.
    std::string backend_ip; ///< Backend IP currently stored in NVS.
    int backend_port{8080};
    bool wifi_connected{false};
    double temperature{0.0};
    double cpu_temperature{0.0};
    double cpu_load{0.0};
};

struct EspSerialResult {
    bool success{false};
    std::string error; ///< Empty on success; human-readable reason otherwise.
    EspSerialAck ack;
};

/**
 * @brief Sends `{"cmd":"status"}` over the USB serial link and decodes the ACK.
 *
 * Read-only query used by the Settings screen to show the board's live SSID /
 * backend target without changing anything.
 */
[[nodiscard]] EspSerialResult probeEspSerial(const std::string& port,
                                             int timeout_ms = 6000);

/**
 * @brief Sends `{"cmd":"provision_wifi",...}` and waits for the ACK.
 *
 * The firmware applies the credentials, waits up to 20 s for association and
 * only then answers (rolling back on failure), so the default timeout is
 * generous. An empty password targets an open network; the backend IP may be
 * empty to keep the board's currently stored target.
 */
[[nodiscard]] EspSerialResult provisionEspSerial(const std::string& port,
                                                 std::string_view ssid,
                                                 std::string_view password,
                                                 std::string_view backend_ip,
                                                 int backend_port = 8080,
                                                 int timeout_ms = 30000);

} // namespace bawal::backend
