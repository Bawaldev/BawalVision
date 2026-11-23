#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace bawal::detail {

/**
 * @brief cgroup v2 quota of this process, rounded up to whole CPUs (0 = none).
 *
 * Walks from our own cgroup up to the root: a quota can be set on any ancestor
 * (container root, systemd slice, a `systemd-run -p CPUQuota=...` scope), which
 * is exactly how a host run can be capped to match a deployment.
 */
inline int cgroupV2QuotaCores() {
    std::string relative = "/";
    if (std::FILE* f = std::fopen("/proc/self/cgroup", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f) != nullptr) {
            if (line[0] == '0' && line[1] == ':' && line[2] == ':') {
                relative.assign(line + 3);
                while (!relative.empty() &&
                       (relative.back() == '\n' || relative.back() == '\r')) {
                    relative.pop_back();
                }
                break;
            }
        }
        std::fclose(f);
    }
    if (relative.empty() || relative.front() != '/') {
        relative = "/";
    }

    for (std::string path = relative;;) {
        const std::string file =
            "/sys/fs/cgroup" + (path == "/" ? std::string{} : path) + "/cpu.max";
        if (std::FILE* f = std::fopen(file.c_str(), "r")) {
            char quota[32] = {0};
            long long period = 0;
            const bool parsed = std::fscanf(f, "%31s %lld", quota, &period) == 2;
            std::fclose(f);
            if (parsed && period > 0 && std::strcmp(quota, "max") != 0) {
                const long long q = std::atoll(quota);
                if (q > 0) {
                    return static_cast<int>((q + period - 1) / period);
                }
            }
        }
        if (path == "/") {
            break;
        }
        const auto slash = path.find_last_of('/');
        path = (slash == std::string::npos || slash == 0) ? "/" : path.substr(0, slash);
    }
    return 0;
}

/** @brief cgroup v1 quota as whole CPUs (0 = none); -1 quota means unlimited. */
inline int cgroupV1QuotaCores() {
    long long quota = -1;
    long long period = 0;
    if (std::FILE* q = std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r")) {
        if (std::fscanf(q, "%lld", &quota) != 1) {
            quota = -1;
        }
        std::fclose(q);
    }
    if (std::FILE* p = std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r")) {
        if (std::fscanf(p, "%lld", &period) != 1) {
            period = 0;
        }
        std::fclose(p);
    }
    if (quota <= 0 || period <= 0) {
        return 0;
    }
    return static_cast<int>((quota + period - 1) / period);
}

} // namespace bawal::detail

namespace bawal {

/**
 * @brief CPUs this process may actually use.
 *
 * std::thread::hardware_concurrency() reports every host core inside a
 * container, so a pool sized from it oversubscribes the cgroup CPU quota
 * (docker compose `cpus:` / `--cpus`) and queues work instead of running it.
 */
inline int cpuBudget() {
    static const int budget = [] {
        int cores = static_cast<int>(std::thread::hardware_concurrency());
        if (cores <= 0) {
            cores = 1;
        }
        if (const int v2 = detail::cgroupV2QuotaCores(); v2 > 0) {
            cores = std::min(cores, v2);
        } else if (const int v1 = detail::cgroupV1QuotaCores(); v1 > 0) {
            cores = std::min(cores, v1);
        }
        return std::max(1, cores);
    }();
    return budget;
}

} // namespace bawal
