#pragma once

#include <string>
#include <string_view>

namespace bawal::backend {

/**
 * @brief True for Docker bridge, Desktop VM, and loopback addresses that can
 * never identify a LAN edge node.
 */
[[nodiscard]] bool looksDockerOrLoopbackIp(std::string_view ip);

/** Resolve the backend host's primary 192.168.x.x / 10.x.x.x LAN address. */
[[nodiscard]] std::string detectHostLanIp();

} // namespace bawal::backend
