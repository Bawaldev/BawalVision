# Contributing to BawalVision

Thank you for your interest in contributing to **BawalVision**! We welcome contributions across all domains: Embedded Hardware, Computer Vision, Backend Engineering, Machine Learning, and Web Dashboard.

---

## 🏛️ Monorepo Architecture

BawalVision is structured as a polyglot monorepo:

| Directory | Subsystem | Language / Stack |
| --------- | --------- | ---------------- |
| [`edge/`](../edge) | IoT Edge Sensor Node & Display | C, ESP-IDF v5/v6, FreeRTOS, LVGL |
<<<<<<< HEAD
| [`services/`](../services) | Machine Vision & Inference Pipeline | C++20, OpenCV, ONNX Runtime, OpenMP, SIMD |
| [`backend/`](../backend) | Real-time Ingestion Gateway & Orchestrator | Go 1.24, Fiber v2, GORM, PostgreSQL |
=======
| [`services/`](../services) | Crow REST Backend, Machine Vision & Inference Pipeline | C++20, Crow, OpenCV, ONNX Runtime, libpq, OpenMP |
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
| [`frontend/`](../frontend) | SCADA & Operator HMI Dashboard | SvelteKit 5, TypeScript, Vite |
| [`ml/`](../ml) | AI Model Training & Super-Resolution | Python 3.11+, PyTorch, YOLOv8, RealESRGAN |
| [`deploy/`](../deploy) | Container Deployment & Infrastructure | Docker Compose, Multi-stage Dockerfiles |

---

## 🛠️ Local Development & Setup

We use [`just`](https://github.com/casey/just) as our unified task runner:

```bash
# View all available recipes across the monorepo
just --list
```

### 1. C++20 Vision Service
```bash
# Install host dependencies (Ubuntu / Debian / Fedora)
just setup-deps

# Configure and compile Release binary
just build-vision

# Run with ThreadSanitizer race detection
just run-vision-tsan
```

<<<<<<< HEAD
### 2. Go Backend
```bash
# Run unit tests and concurrency race detector
just test-backend

# Launch local gateway server
just run-gateway
=======
### 2. C++20 Crow Backend
```bash
# Compile the unified Crow backend (HTTP + in-process vision)
just build-backend

# Run against the Docker PostgreSQL instance (host process, Wi-Fi scan capable)
just run-backend

# Exercise the HTTP API against a live backend
just test-api
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
```

### 3. ESP32-S3 Firmware
```bash
# Compile edge firmware
just build-edge

# Flash and open serial monitor
just flash-edge PORT=/dev/ttyACM0
```

### 4. SvelteKit Frontend
```bash
# Install npm dependencies
just install-frontend

# Start Vite dev server on http://localhost:5173
just run-frontend

# Run TypeScript type check
just check-frontend
```

### 5. Full Stack via Docker Compose
```bash
# Start backend, vision service, database, and frontend
just compose-up

# Follow live container logs
just compose-logs
```

---

## 📝 Commit Convention

We enforce the [Conventional Commits](https://www.conventionalcommits.org/) standard:

- `feat(scope): ...` for new features or algorithmic improvements.
- `fix(scope): ...` for bug fixes.
- `perf(scope): ...` for SIMD, caching, or throughput optimizations.
- `refactor(scope): ...` for code restructuring without behavioral change.
- `docs(scope): ...` for documentation updates.
- `chore(scope): ...` for dependency updates or CI maintenance.

*Example:* `feat(vision): optimize Retinex illumination using AVX2 SIMD`

---

## 🔒 Pull Request Guidelines

1. **Keep Commits Clean**: Never commit `.env`, Wi-Fi secrets, private keys, or raw image datasets.
2. **Architecture Portability**: Do NOT use `-march=native` in C++ code; use explicit feature detection (`-mavx2`, `__AVX2__`, NEON intrinsics) to ensure portability.
<<<<<<< HEAD
3. **Run CI Checks Locally**: Before opening a PR, ensure `just test-backend` and `just build-vision` pass locally.
=======
3. **Run CI Checks Locally**: Before opening a PR, ensure `just build-backend`, `just build-vision`, `just test-api`, and `just check-frontend` pass locally.
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
4. **Link Issues**: Reference related issues in the PR description (e.g. `Fixes #42`).

