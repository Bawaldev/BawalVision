#pragma once

#include <cstdint>
#include <string>
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

} // namespace bawal::backend
