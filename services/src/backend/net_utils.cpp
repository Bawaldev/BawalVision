#include "bawal/backend/net_utils.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace bawal::backend {

bool looksDockerOrLoopbackIp(std::string_view ip) {
    if (ip.empty() || ip == "0.0.0.0") {
        return true;
    }
    if (ip.rfind("127.", 0) == 0 || ip.rfind("192.168.65.", 0) == 0) {
        return true;
    }
    if (ip.rfind("172.", 0) == 0) {
        const std::size_t dot = ip.find('.', 4);
        if (dot != std::string_view::npos) {
            const std::string second(ip.substr(4, dot - 4));
            char* end = nullptr;
            const long octet = std::strtol(second.c_str(), &end, 10);
            if (end != second.c_str() && octet >= 16 && octet <= 31) {
                return true;
            }
        }
    }
    return false;
}

std::string detectHostLanIp() {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return {};
    }

    std::vector<std::string> local_ips;
    std::string primary;
    std::string fallback;
    for (ifaddrs* it = interfaces; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        char buffer[INET_ADDRSTRLEN] = {0};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof(buffer)) == nullptr) {
            continue;
        }
        const std::string ip(buffer);
        if (looksDockerOrLoopbackIp(ip)) {
            continue;
        }
        local_ips.push_back(ip);
        if (primary.empty() && (ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0)) {
            primary = ip;
        } else if (fallback.empty()) {
            fallback = ip;
        }
    }
    freeifaddrs(interfaces);

    // Honour HOST_LAN_IP/BACKEND_IP only while that address is really bound to
    // one of our interfaces right now. Roaming the laptop to another Wi-Fi
    // must not keep advertising the previous network's address (which would
    // strand the ESP32 on UDP discovery until it re-finds the backend).
    for (const char* key : {"HOST_LAN_IP", "BACKEND_IP"}) {
        const char* value = std::getenv(key);
        if (value == nullptr || *value == '\0' || looksDockerOrLoopbackIp(value)) {
            continue;
        }
        const std::string configured(value);
        if (std::find(local_ips.begin(), local_ips.end(), configured) != local_ips.end()) {
            return configured;
        }
    }

    if (!primary.empty()) {
        return primary;
    }
    return fallback;
}

} // namespace bawal::backend
