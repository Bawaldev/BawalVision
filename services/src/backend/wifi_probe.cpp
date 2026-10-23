#include "bawal/backend/wifi_probe.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <unordered_map>

#include "bawal/backend/text_utils.hpp"

namespace bawal::backend {
namespace {

std::string runShellCommand(const std::string& command, int* exit_status = nullptr) {
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        if (exit_status != nullptr) {
            *exit_status = -1;
        }
        return output;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output.append(buffer);
    }
    if (exit_status != nullptr) {
        *exit_status = pclose(pipe);
    } else {
        pclose(pipe);
    }
    return output;
}

std::string resolveHostExecutable(const std::vector<std::string>& absolute_candidates,
                                  const std::string& fallback) {
    for (const std::string& candidate : absolute_candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return fallback;
}

std::vector<std::string> splitNmcliTerseLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;
    for (const char c : line) {
        if (escaped) {
            current.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == ':') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    return fields;
}

std::vector<WifiNetworkInfo> parseWifiNetworks(const std::string& output) {
    std::vector<WifiNetworkInfo> networks;
    std::unordered_map<std::string, std::size_t> index_by_ssid;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> fields = splitNmcliTerseLine(line);
        if (fields.size() < 3) {
            continue;
        }

        const std::string ssid = trimCopy(fields[0]);
        if (ssid.empty() || ssid == "--") {
            continue;
        }

        int signal = 0;
        try {
            signal = std::stoi(trimCopy(fields[1]));
        } catch (const std::exception&) {
            continue;
        }
        signal = std::clamp(signal, 0, 100);

        std::string security = trimCopy(fields[2]);
        if (security.empty()) {
            security = "Open";
        }

        const auto existing = index_by_ssid.find(ssid);
        if (existing == index_by_ssid.end()) {
            index_by_ssid.emplace(ssid, networks.size());
            networks.push_back(WifiNetworkInfo{ssid, signal, security, false});
        } else if (signal > networks[existing->second].signal) {
            networks[existing->second].signal = signal;
            networks[existing->second].security = security;
        }
    }

    std::sort(networks.begin(), networks.end(),
              [](const WifiNetworkInfo& lhs, const WifiNetworkInfo& rhs) {
                  return lhs.signal > rhs.signal;
              });
    return networks;
}

} // namespace

std::vector<WifiNetworkInfo> scanWifiNetworks(std::string* raw_output,
                                              std::string* executed_command,
                                              int* exit_status) {
    const std::string nmcli = resolveHostExecutable(
        {"/usr/bin/nmcli", "/bin/nmcli", "/usr/local/bin/nmcli"}, "nmcli");
    const std::string timeout = resolveHostExecutable(
        {"/usr/bin/timeout", "/bin/timeout"}, "timeout");
    const std::string rescan_command =
        timeout + " 8 " + nmcli +
        " -t -f SSID,SIGNAL,SECURITY device wifi list --rescan yes 2>&1";
    const std::string cached_command =
        timeout + " 8 " + nmcli +
        " -t -f SSID,SIGNAL,SECURITY device wifi list 2>&1";

    int status = -1;
    std::string output = runShellCommand(rescan_command, &status);
    std::string command = rescan_command;
    std::vector<WifiNetworkInfo> networks = parseWifiNetworks(output);

    // A forced rescan is rejected when NetworkManager is already scanning
    // (or when the caller lacks permission). Fall back to the cached list so
    // the settings page still shows the last known networks.
    if (networks.empty()) {
        int fallback_status = -1;
        std::string fallback_output = runShellCommand(cached_command, &fallback_status);
        std::vector<WifiNetworkInfo> fallback_networks =
            parseWifiNetworks(fallback_output);
        if (!fallback_networks.empty() || output.empty()) {
            output = std::move(fallback_output);
            status = fallback_status;
            command = cached_command;
            networks = std::move(fallback_networks);
        }
    }

    if (raw_output != nullptr) {
        *raw_output = output;
    }
    if (executed_command != nullptr) {
        *executed_command = command;
    }
    if (exit_status != nullptr) {
        *exit_status = status;
    }
    return networks;
}

} // namespace bawal::backend
