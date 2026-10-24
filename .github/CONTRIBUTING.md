# Contributing to BawalVision 🤝🚀

Thank you for your interest in contributing to **BawalVision**! We welcome contributions across all domains: Embedded Hardware, Computer Vision, Backend Engineering, Machine Learning, and Web Dashboard.

---

## 🏛️ Monorepo Architecture & Subsystem Guides

BawalVision is structured as a polyglot monorepo. Each subsystem includes its own detailed engineering handbook:

| Directory | Subsystem | Stack | Engineering Guide |
| :--- | :--- | :--- | :--- |
| [`services/`](../services) | C++20 Vision Engine & Crow Backend | C++20, OpenCV, ONNX Runtime, libpq, OpenMP, mimalloc | 📖 [`services/README.md`](../services/README.md) |
| [`edge/`](../edge) | ESP32-S3 IoT Node & LVGL Display | C, ESP-IDF v5/v6, FreeRTOS, LVGL | 📖 [`edge/README.md`](../edge/README.md) |
| [`frontend/`](../frontend) | SvelteKit HMI Operator Portal | SvelteKit 5 (Runes), TypeScript, Vite, Tailwind CSS | 📖 [`frontend/README.md`](../frontend/README.md) |
| [`ml/`](../ml) | Machine Learning, Training & INT8 PTQ | Python 3.11+, PyTorch, YOLOv8, Real-ESRGAN | 📖 [`ml/README.md`](../ml/README.md) |
| [`deploy/`](../deploy) | Container Deployment & AWS Lambda | Docker Compose, PostgreSQL 16, Grafana, Lambda C++ | 📖 [`deploy/README.md`](../deploy/README.md) |

---

## 🛠️ Local Development & Setup

We use [`just`](https://github.com/casey/just) as our unified task runner:

```bash
# View all available recipes across the monorepo
just --list
```

### 1. C++20 Vision Service & Backend
```bash
# Install host dependencies (Ubuntu / Debian / Fedora)
just setup-deps

# Configure and compile Release binary
just build-backend

# Run against the Docker PostgreSQL instance (host process, Wi-Fi scan capable)
just run-backend

# Run full integration API test suite
just test-api

# Run with ThreadSanitizer race detection
just run-vision-tsan
```

### 2. ESP32-S3 Firmware
```bash
# Compile edge firmware
just build-edge

# Flash and open serial monitor
just run-edge /dev/ttyACM0
```

### 3. SvelteKit Frontend
```bash
# Install npm dependencies
just install-frontend

# Run TypeScript type check (strict CI gate)
just check-frontend

# Start Vite dev server on http://localhost:5173
just run-frontend
```

### 4. Full Stack via Docker Compose
```bash
# Start backend, database, and frontend in Docker
just compose-up

# Follow live container logs
just compose-logs
```

---

## 📝 Commit Conventions

We enforce the [Conventional Commits](https://www.conventionalcommits.org/) standard with subsystem scopes:

* `feat(scope): ...` for new features or algorithmic improvements.
* `fix(scope): ...` for bug fixes.
* `perf(scope): ...` for SIMD vectorization, caching, or throughput optimizations.
* `refactor(scope): ...` for code restructuring without behavioral changes.
* `docs(scope): ...` for documentation updates.
* `chore(scope): ...` for dependency updates or CI maintenance.

**Valid Scopes**: `vision`, `backend`, `edge`, `ml`, `frontend`, `deploy`, `ci`, `security`, `tooling`.

*Example:* `perf(vision): optimize food freshness evaluator using 8-lane AVX2 SIMD`

---

## 🔒 Pull Request & Verification Checklist

Before submitting a Pull Request, verify that your changes adhere to our quality gates:

- [ ] **No Machine-Specific Architecture Flags**: Never commit `-march=native`. Use explicit feature flags (`-mavx2 -mfma`, ARM NEON) with dynamic capability checks so binaries remain portable.
- [ ] **Clean Git Index**: Never commit `.env*`, private keys, Wi-Fi credentials, weights files (`*.pt`, `weights/`), or raw image datasets. Temporary files belong in `.cache_trash/` (gitignored).
- [ ] **Pass Local Verification**:
  - `just build-vision` compiles with zero warnings or errors.
  - `just test-api` passes against a live backend.
  - `just check-frontend` passes without diagnostic errors.
- [ ] **Competition Compliance (OpenCV AI Competition 2026)**: If any AI-assisted code generation or agentic program was used, document the scope and rationale in [`OUTPUT.md`](../OUTPUT.md).
- [ ] **Link Issues**: Reference related tracking issues in the PR body (e.g., `Fixes #42`).
