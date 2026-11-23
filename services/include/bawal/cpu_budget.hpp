#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace bawal {

/**
 * @brief CPUs this process may actually use.
 *
 * std::thread::hardware_concurrency() reports every host core inside a
 * container, so a pool sized from it oversubscribes the cgroup CPU quota
 * (docker compose `cpus:` / `--cpus`). Prefer the quota when one is set.
 */
inline int cpuBudget() {
    static const int budget = [] {
        int cores = static_cast<int>(std::thread::hardware_concurrency());
        if (cores <= 0) {
            cores = 1;
        }

        const auto quota_cores = [](long long quota, long long period) {
            return (period > 0 && quota > 0)
                       ? static_cast<int>((quota + period - 1) / period)
                       : 0;
        };

        // cgroup v2: "<quota|max> <period>".
        if (std::FILE* f = std::fopen("/sys/fs/cgroup/cpu.max", "r")) {
            char quota[32] = {0};
            long long period = 0;
            if (std::fscanf(f, "%31s %lld", quota, &period) == 2) {
                if (const int n = quota_cores(std::atoll(quota), period); n > 0) {
                    cores = std::min(cores, n);
                }
            }
            std::fclose(f);
        } else {
            // cgroup v1: separate quota/period files, -1 means "no limit".
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
            if (const int n = quota_cores(quota, period); n > 0) {
                cores = std::min(cores, n);
            }
        }
        return std::max(1, cores);
    }();
    return budget;
}

} // namespace bawal
