# BawalVision 🐟🔍
> **Smart Food Quality & Tray Inspection System**  
> *Developed for OpenCV AI Competition 2026*

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=c%2B%2B)](services/)
[![Go](https://img.shields.io/badge/Go-1.24-00ADD8?logo=go)](backend/)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5%2Fv6-E7352C?logo=espressif)](edge/)
[![SvelteKit](https://img.shields.io/badge/SvelteKit-5-FF3E00?logo=svelte)](frontend/)

BawalVision is an edge-to-cloud automated computer vision and telemetry system designed for quality control, freshness classification, portion detection, and thermal inspection of food trays (e.g., Makan Bergizi Gratis / MBG programs).

---

## 🏛️ System Architecture

```
                      +-----------------------------+
                      |   ESP32-S3 Edge Node        |
                      | - OV5640 DVP Camera (VGA)   |
                      | - ST7789 128x160 LVGL TFT   |
                      | - GY-906 IR Temp Sensor     |
                      | - FreeRTOS Static Tasks     |
                      +--------------+--------------+
                                     | HTTP POST (Keep-Alive) / Wi-Fi
                                     v
                      +-----------------------------+
                      |   Go API Gateway (:8080)    |
                      | - Ingestion & Routing       |
                      | - JWT & Cookie Auth         |
                      | - WebSockets & Telemetry    |
                      +--------------+--------------+
                                     |
             +-----------------------+-----------------------+
             |                                               |
             v                                               v
+---------------------------+                 +---------------------------+
| C++ Vision Service (:9001)|                 | SvelteKit 5 HMI (:3001)   |
| - ONNX Runtime (YOLOv8n)  |                 | - Real-time Live Streams  |
| - OpenCV & OpenMP (AVX2)  |                 | - Visual Inspection Cards |
| - Color/Hue & Quality QC  |                 | - Telemetry & Analytics   |
+---------------------------+                 +---------------------------+
             |
             v
+---------------------------+
| PostgreSQL Local Database |
| - Inspection Records      |
| - Audit & Telemetry Logs  |
+---------------------------+
```

---

## 📦 Subsystems Overview

### 1. Edge Node (`edge/`)
- **Microcontroller:** ESP32-S3 Dual-Core Xtensa LX7 @ 240 MHz (FreeRTOS ESP-IDF).
- **Camera:** OV5640 DVP CSP camera delivering MJPEG/RAW frames over HTTP Keep-Alive.
- **Display:** ST7789/ST7735 $128 \times 160$ SPI TFT with **LVGL 8 RGB Dashboard** showing top HTTP status, Wi-Fi IP, food/core temperatures, and system inspection state.
- **FreeRTOS Static Allocation:** All tasks (`xTaskCreateStaticPinnedToCore`) and queues (`xQueueCreateStatic`) use static memory buffers for zero heap overhead and deterministic 24/7 reliability.

### 2. Vision Engine (`services/`)
- **Core:** C++20 with OpenCV 4, ONNX Runtime, OpenMP multi-threading, and AVX2/SIMD acceleration.
- **Model:** YOLOv8n object detection backbone exporting static fixed-dimension tensors (`[1, 3, 640, 640]`, Opset 17).
- **Safety:** ThreadSanitizer (TSan) build configurations to detect data races and deadlocks.

### 3. Backend Gateway (`backend/`)
- **Engine:** Go 1.24 with Fiber v2, PostgreSQL driver, and WebSocket real-time telemetry broadcaster.
- **Security:** CSRF protection, HttpOnly cookie authentication, rate limiting, and parameter validation.

### 4. Frontend HMI (`frontend/`)
- **Stack:** SvelteKit 5 (Runes), TypeScript, Vite, Tailwind CSS, DaisyUI, Chart.js.
- **Features:** Responsive dark mode dashboard, live camera streams, real-time telemetry metrics, and food classification cards.

### 5. Machine Learning & YOLO (`ml/`)
- **Dataset Management:** Multi-engine scrapers with 64-bit perceptual dHash & MD5 deduplication.
- **Model Optimization:** Automated INT8 Post-Training Quantization and static ONNX tensor export with numerical parity verification.

---

## 🔌 Hardware Pinout (BawalVision v3 Schematic)

| Function | Signal Name | ESP32-S3 GPIO | Pin Header / Notes |
|---|---|---|---|
| **TFT Display (SPI)** | `LCD_MOSI` | `GPIO 14` | Pin 19 (`IO14`) |
| | `LCD_SCK` | `GPIO 21` | Pin 37 (`IO21`) |
| | `LCD_DC` | `GPIO 45` | Pin 34 (`IO45`) |
| | `LCD_CS` | Tied to `GND` | Direct Ground (CS-less mode) |
| | `LCD_RST` | Tied to `RES/EN` | Hardware Reset Line |
| | `LCD_BCKL` | Tied to `3V3` | Always On |
| **Thermal Sensor (I2C)** | `I2C_SDA` | `GPIO 1` | Pin 23 (`IO1`) |
| | `I2C_SCL` | `GPIO 47` | Pin 36 (`IO47`) |
| **OV5640 Camera (DVP)** | `CAM_XCLK` | `GPIO 15` | Pin 7 |
| | `CAM_PCLK` | `GPIO 13` | Pin 18 |
| | `CAM_VSYNC` | `GPIO 6` | Pin 5 |
| | `CAM_HREF` | `GPIO 7` | Pin 6 |
| | `CAM_SIOD` | `GPIO 4` | Pin 3 |
| | `CAM_SIOC` | `GPIO 5` | Pin 4 |
| | `CAM_D0..D7` (Y2..Y9) | `11, 9, 8, 10, 12, 18, 17, 16` | 8-bit Parallel DVP Data Bus |

---

## 🛠️ Unified Task Runner (`just`) Cheatsheet

This project uses [just](https://github.com/casey/just) as a centralized, cross-platform task runner.

### 1. Machine Learning & YOLO (`scripts/ml.just`)

| Command | Description |
|---|---|
| `just setup-ml` | Create `.venv` and install PyTorch (CUDA), Ultralytics, and ONNX dependencies |
| `just scrap-dataset [cat] [n] [engine]` | Multi-engine web scraper with dHash deduplication (Default: `cat="all"` `n="1000"`) |
| `just split-dataset [src] [out] [w]` | Multi-threaded stratified dataset splitter (70/15/15) with perceptual hashing |
| `just augment-train [target] [dir] [w]` | Multi-threaded domain-specific augmentation applied strictly to train split |
| `just prepare-yolo-ai1 [batch] [w]` | Batched zero-shot YOLO annotation generator using YOLO-World foundation model |
| `just train-ai1 [epochs] [batch] [w]` | Train YOLOv8n object detector, export static ONNX, and verify numerical parity |
| `just pipeline-ai1 [w] [batch] [epochs]` | **End-to-end autonomous ML pipeline** (gather $\to$ split $\to$ augment $\to$ annotate $\to$ train $\to$ deploy) |
| `just yolo-train [model] [data] ...` | Flexible YOLO training CLI via Ultralytics module |
| `just yolo-val [model] [data] ...` | Evaluate and compute mAP50 / mAP50-95 metrics on validation set |
| `just yolo-export [model] [format]` | Export PyTorch weights to static ONNX binary (`[1, 3, 640, 640]`, Opset 17) |
| `just yolo-predict [model] [source]` | Run object detection inference on test images or directory |
| `just yolo-tune [model] [data]` | Automated hyperparameter tuning / genetic search |
| `just test-model-ai1 [model]` | Inspect ONNX model input/output tensor shapes and test CPU inference |
| `just quantize-ai1` | Run INT8 Post-Training Quantization for YOLOv8 |
| `just generate-ncnn-cpp [model] [scale]` | High-speed C++ NCNN model generator (`.param` and `.bin`) |
| `just mlflow-ui` | Launch local MLflow experiment tracking dashboard |
| `just clean-ml` | Clean intermediate training run caches and temporary files |

---

### 2. Backend & Vision Services (`scripts/backend.just`)

| Command | Description |
|---|---|
| `just setup-deps` | Install native C++ dependencies (OpenCV, CMake, Ninja, OpenMP) on Ubuntu |
| `just configure-vision [target]` | Generate CMake build configuration for C++ Vision service (Default: `Release`) |
| `just build-vision [target]` | Compile C++ Vision service binary with parallel build jobs (`-j$(nproc)`) |
| `just run-vision [device_id]` | Execute C++ Vision service locally |
| `just configure-vision-tsan` | Configure CMake with ThreadSanitizer for data race detection |
| `just build-vision-tsan` | Compile C++ Vision binary with ThreadSanitizer instrumentation |
| `just run-vision-tsan` | Run Vision service with ThreadSanitizer deadlock / race checking |
| `just build-gateway` | Compile Go Gateway API binary |
| `just run-gateway` | Run Go Gateway REST API server locally |
| `just test-api [base_url]` | Run automated REST API & session verification test suite |
| `just test-backend` | Run Go unit/package tests (`go test -v ./...`) |
| `just install-frontend` | Install SvelteKit npm dependencies (`npm install`) |
| `just build-frontend` | Build production bundle for SvelteKit HMI |
| `just check-frontend` | Run Svelte TypeScript diagnostic check (`npm run check`) |
| `just run-frontend` | Run SvelteKit development server with live reload |
| `just clean` | Remove all build artifacts (`services/build`, `backend/bin`, etc.) |

---

### 3. Docker & Infrastructure (`scripts/backend.just`)

| Command | Description |
|---|---|
| `just compose-up [service]` | Start containers in background (`postgres`, `backend`, `vision`, `frontend`) |
| `just compose-down` | Stop and remove running containers |
| `just compose-build [service]` | Build container images via Docker Compose |
| `just compose-logs [service]` | View live aggregated container logs |
| `just allow-firewall [port]` | Configure firewall rules for gateway port (UFW / Windows) |
| `just deny-firewall [port]` | Remove firewall rule for gateway port |

---

### 4. ESP32-S3 Firmware & Edge (`scripts/edge.just`)

| Command | Description |
|---|---|
| `just build-edge` | Build standard ESP32-S3 firmware via ESP-IDF |
| `just configure-edge` | Reconfigure ESP-IDF build system |
| `just flash-edge [port]` | Flash compiled binary to ESP32-S3 & launch serial monitor (Default: `/dev/ttyACM0`) |
| `just run-edge [port]` | **Shortcut:** Build firmware + Flash + Serial Monitor |
| `just monitor-edge [port]` | Open ESP-IDF serial monitor |
| `just erase-nvs [port]` | Erase NVS flash memory partition |
| `just menuconfig-edge` | Open interactive ESP-IDF `menuconfig` |
| `just clean-edge` | Full clean of ESP-IDF build directory |

---

## 🚀 Quickstart Guide

### 1. Clone & Configure Environment
```bash
git clone https://github.com/fariswirakusuma/BawalVision.git
cd BawalVision

# Copy environment template
cp .env.example .env
# Edit .env with your WIFI_SSID, WIFI_PASS, and BACKEND_IP
```

### 2. Launch Stack with Docker
```bash
# Build all container images via Docker Compose
just compose-build

# Start entire infrastructure in detached mode
just compose-up
```
- **Frontend HMI:** `http://localhost:3001`
- **Go Gateway API:** `http://localhost:8080/api/v1`
- **PostgreSQL:** `localhost:5432`

### 3. Train & Export Object Detection Model
```bash
# Setup Python ML Virtual Environment
just setup-ml

# Run end-to-end parallel training pipeline
just pipeline-ai1
```
The trained model will be automatically validated and saved as `ml/AI1/model/yolov8n/ai1_vision.onnx`.

### 4. Build & Flash Edge Firmware
```bash
# Compile and flash ESP32-S3 firmware over USB-C
just run-edge /dev/ttyACM0
```

---

## 👥 Core Contributors & Maintainers

| Contributor | GitHub | Role & Subsystems |
|:---|:---|:---|
| **Faris Wirakusuma** | [@fariswirakusuma](https://github.com/fariswirakusuma) | Project Lead, ESP32-S3 Firmware, C++20 Vision Engine, Go Backend Gateway, SvelteKit Frontend |
| **Rasyakhairi (OurangIlang)** | [@OurangIlang](https://github.com/OurangIlang) | Machine Learning & YOLOv8 Pipeline, Database Migrations, SvelteKit Frontend |
| **Daffa Rizqy (pondwatr)** | [@pondwatr](https://github.com/pondwatr) | Embedded Hardware Engineering, Electronics Schematics, ESP32-S3 Edge Node |

> Subsystem review responsibilities and code ownership are mapped in [`.github/CODEOWNERS`](.github/CODEOWNERS).

---

## 🤝 Contributing to BawalVision

We welcome community contributions across all engineering domains: Embedded Hardware, Computer Vision, Backend Services, Machine Learning, and Web UI!

### 📋 Contribution Workflow

1. **Read Guidelines:** Please review our [Contribution Guide](.github/CONTRIBUTING.md) and [Code of Conduct](.github/CODE_OF_CONDUCT.md).
2. **Issue Selection:** Check the [Issue Tracker](https://github.com/fariswirakusuma/BawalVision/issues) for open tasks or submit a structured bug report / feature request using our [Issue Templates](.github/ISSUE_TEMPLATE/).
3. **Branching & Commits:**
   - Create a feature branch: `git checkout -b feat/your-feature-name`
   - Follow [Conventional Commits](https://www.conventionalcommits.org/):
     - `feat(scope): ...` for new features or algorithmic improvements.
     - `fix(scope): ...` for bug fixes.
     - `perf(scope): ...` for SIMD or memory optimizations.
4. **Local Verification Checklist:**
   - **Go Backend:** `just test-backend` (passes unit tests with concurrency race detector)
   - **C++ Vision:** `just build-vision` (compiles without non-portable `-march=native`)
   - **Frontend:** `just check-frontend` (passes SvelteKit type checks)
5. **Open a Pull Request:** Submit your PR against `main` using our [Pull Request Template](.github/PULL_REQUEST_TEMPLATE.md).

> [!IMPORTANT]
> **Security & Secret Safeguards:** Never commit `.env`, Wi-Fi secrets, private keys, or raw image datasets. All PRs are automatically audited by our [Security Scanner](.github/workflows/security.yml).
>
> If you discover a security vulnerability, please follow our [Security Policy](.github/SECURITY.md).

---

## 📜 Competition Compliance & Ethical Safeguards

1. **Agentic Disclosure & Transparency:**
   - Developed for the **OpenCV AI Competition 2026**. All automated pipelines, container recipes, and AI-assisted engineering modifications are disclosed and audited.
2. **Privacy & Sensitive Data Protection:**
   - No private credentials, cloud tokens, or confidential datasets are stored in Git commits. Configuration templates are provided via [`.env.example`](.env.example).
3. **Reproducibility & Portability:**
   - Standardized CMake and Docker configurations ensure portable compilation across x86-64 and ARM64 architectures without hardware lock-in.

---

## 📄 License
This project is licensed under the Apache 2.0 License - see the [LICENSE](LICENSE) file for details.