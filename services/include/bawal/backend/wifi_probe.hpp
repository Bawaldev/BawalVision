#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bawal::backend {

/** One Wi-Fi network reported by `nmcli device wifi list`. */
struct alignas(8) WifiNetworkInfo {
    std::string ssid;
    int32_t signal{0};
    std::string security;
    bool is_saved{false};
    uint8_t reserved[3]{0}; ///< Explicit tail padding keeps the layout 8-byte aligned
};

/**
 * @brief Scan visible Wi-Fi networks through nmcli.
 *
 * Falls back to the cached NetworkManager list when a forced rescan is
 * rejected (already scanning or missing permissions).
 *
 * @param raw_output       Optional raw nmcli output for diagnostics.
 * @param executed_command Optional command line that produced the result.
 * @param exit_status      Optional process exit status.
 */
[[nodiscard]] std::vector<WifiNetworkInfo> scanWifiNetworks(
    std::string* raw_output = nullptr,
    std::string* executed_command = nullptr,
    int* exit_status = nullptr);

/**
 * @brief SSID of the Wi-Fi network the host is currently associated with.
 *
 * Empty when the host is offline/wired or nmcli is unavailable. Used instead
 * of the static WIFI_SSID env guess once the laptop roams to another network.
 */
[[nodiscard]] std::string detectActiveWifiSsid();

/**
 * @brief Reads the WPA PSK NetworkManager stored for `ssid` (best effort).
 *
 * Connection names normally equal the SSID. Returns empty when the secret is
 * agent-owned or NetworkManager is unavailable. The value stays server-side
 * and is never returned over the API.
 */
[[nodiscard]] std::string wifiPasswordForSsid(std::string_view ssid);

} // namespace bawal::backend
