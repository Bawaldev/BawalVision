import math
import os
from pathlib import Path


def get_optimal_workers(reserve_cores: int = 0, max_ratio: float = 0.75) -> int:
    """
    Detects available CPU threads/cores safely across:
    - Host Laptops / Desktops (x86_64, ARM64 Apple Silicon)
    - AWS Cloud EC2 (Intel Xeon, AMD EPYC, AWS Graviton ARM)
    - Docker Containers / Kubernetes / AWS ECS (cgroups v1 & cgroups v2 quotas)
    - Windows (cmd/PowerShell), Linux, macOS

    Applies the 75% thermal headroom rule (max_ratio=0.75) by default to prevent
    severe thermal throttling, fan noise, and OS starvation on multi-core workstations.

    Args:
        reserve_cores (int): Explicit number of CPU cores to leave free for OS/system.
        max_ratio (float): Maximum fraction of total threads to allocate (default: 0.75 = 75%).

    Returns:
        int: Number of optimal parallel worker processes/threads (minimum: 1).
    """
    cores = None

    # 1. Python 3.13+ process CPU count (affinity & cgroup aware)
    if hasattr(os, "process_cpu_count"):
        try:
            cores = os.process_cpu_count()
        except Exception:
            pass

    # 2. Linux / AWS / Docker CPU affinity
    if cores is None and hasattr(os, "sched_getaffinity"):
        try:
            affinity = os.sched_getaffinity(0)
            if affinity:
                cores = len(affinity)
        except Exception:
            pass

    # 3. Cgroups v2 quota check (Docker / AWS ECS / Kubernetes modern)
    cgroup_v2 = Path("/sys/fs/cgroup/cpu.max")
    if cores is None and cgroup_v2.exists():
        try:
            parts = cgroup_v2.read_text().strip().split()
            if len(parts) >= 2 and parts[0] != "max":
                quota = int(parts[0])
                period = int(parts[1])
                if quota > 0 and period > 0:
                    cores = math.ceil(quota / period)
        except Exception:
            pass

    # 4. Cgroups v1 quota check (Docker / AWS ECS legacy)
    cfs_quota = Path("/sys/fs/cgroup/cpu/cpu.cfs_quota_us")
    cfs_period = Path("/sys/fs/cgroup/cpu/cpu.cfs_period_us")
    if cores is None and cfs_quota.exists() and cfs_period.exists():
        try:
            q = int(cfs_quota.read_text().strip())
            p = int(cfs_period.read_text().strip())
            if q > 0 and p > 0:
                cores = math.ceil(q / p)
        except Exception:
            pass

    # 5. Fallback to os.cpu_count()
    if cores is None or cores <= 0:
        cores = os.cpu_count() or 4

    # Apply 75% ratio ceiling to prevent thermal throttling on 6C/12T and 8C/16T laptops
    bounded_cores = max(1, int(round(cores * max_ratio)))
    if reserve_cores > 0:
        bounded_cores = max(1, bounded_cores - reserve_cores)

    return bounded_cores


if __name__ == "__main__":
    workers = get_optimal_workers()
    print(f"Optimal Workers Detected: {workers}")

