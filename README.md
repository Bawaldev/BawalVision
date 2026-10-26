# BawalVision 🐟🔍
> **Smart Food Quality & Tray Inspection System**  
> *Developed for OpenCV AI Competition 2026*

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=c%2B%2B)](services/)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5%2Fv6-E7352C?logo=espressif)](edge/)
[![SvelteKit](https://img.shields.io/badge/SvelteKit-5-FF3E00?logo=svelte)](frontend/)

BawalVision is an edge-to-cloud automated computer vision and telemetry system designed for quality control, freshness classification, portion detection, and thermal inspection of food trays (e.g., Makan Bergizi Gratis / MBG programs).

---

## 🏛️ System Architecture

```
                      +-----------------------------+
                      |   ESP32-S3 Edge Node        |
                      | - OV5640 DVP Camera (VGA)   |
                      | - st7735 128x160 LVGL TFT   |
                      | - FreeRTOS Static Tasks     |
                      +--------------+--------------+
                                     | HTTP POST (Keep-Alive) / Wi-Fi
                                     v
                      +-----------------------------+
                      |   C++ Crow Backend (:8080)  |
                      | - SHM Frame Ingress/Routing |
                      | - In-process YOLOv8/OpenCV  |
                      | - PostgreSQL telemetry      |
                      +--------------+--------------+
                                     |
             +-----------------------+-----------------------+
             |                                               |
             v                                               v
+---------------------------+                 +---------------------------+
| In-Process Vision (SHM)   |                 | SvelteKit 5 HMI (:3001)   |
| - ONNX Runtime (YOLOv8s)  |                 | - Real-time Live Streams  |
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

| Subsystem | Primary Technologies | Handbook & Documentation | Key Responsibilities |
| :--- | :--- | :--- | :--- |
| **Vision & Backend Engine** | C++20, OpenCV 4, ONNX Runtime, OpenMP, Crow, mimalloc | 📖 [`services/README.md`](services/README.md) | High-speed POSIX SHM frame ingress, in-process YOLOv8s INT8 inference, 8-lane AVX2 spoilage evaluation, Crow REST/WebSocket API, and LaTeX PDF audit generation. |
| **Edge Hardware Node** | ESP32-S3, FreeRTOS, LVGL 8, OV5640 DVP| 📖 [`edge/README.md`](edge/README.md) | Deterministic static-allocated FreeRTOS firmware, 20 FPS video streaming over HTTP Keep-Alive, UDP auto-discovery, and local SPI TFT RGB dashboard. |
| **Frontend Web HMI** | SvelteKit 5 (Runes), TypeScript, Vite, Tailwind CSS, DaisyUI | 📖 [`frontend/README.md`](frontend/README.md) | Real-time supervision dashboard, 30+ FPS live inspection stream, high-throughput batch ZIP uploader, interactive PDF report viewer, and USB Wi-Fi provisioning. |
| **Machine Learning & Pipeline**| PyTorch, Ultralytics YOLOv8, ONNX, Real-ESRGAN | 📖 [`ml/README.md`](ml/README.md) | Autonomous data gathering, dHash deduplication, zero-shot YOLO-World relabeling, YOLOv8s training, layer-frozen incremental fine-tuning, and INT8 Post-Training Quantization (PTQ). |
| **Deployment & Orchestration** | Docker Compose, PostgreSQL 16, Grafana OSS, AWS Lambda | 📖 [`deploy/README.md`](deploy/README.md) | Production Docker stack, automated PostgreSQL telemetry persistence, pre-provisioned Grafana analytics, and serverless AWS Lambda C++ container packaging. |

---

## ⚡ Vision & System Benchmark Highlights

*(Evaluated on host **Intel® Xeon® W-11855M @ 3.20 GHz (6C/12T, Fedora Linux)**, containerized via **Debian/Ubuntu Docker**, and cloud-verified on **AWS Lambda C++ Runtime**. Full methodology, mathematical vectorization proofs, and scaling charts available in [`services/README.md`](services/README.md#benchmarks--performance-engineering))*

| Performance Metric | Baseline / Previous | BawalVision Optimized | Measurement & Performance Gain |
| :--- | :---: | :---: | :--- |
| **Live Vision Stream Ingress** | $112.4\text{ ms}$ (Python FP32) | **$57.5\text{ ms}$** (C++20 INT8) | **Sustains solid $15.7\text{ FPS}$ continuous stream** |
| **YOLOv8s Model Disk Footprint**| $44.8\text{ MB}$ (FP32) | **$25.0\text{ MB}$** (INT8) | **$1.79\times\text{ weights reduction}$ ($44.2\%\text{ smaller}$)** |
| **SIMD Spoilage & Freshness** | $0.121\text{ ms}$ (Scalar C++) | **$0.0058\text{ ms}$** (AVX2 Bitmask)| **$20.97\times\text{ speedup}$ (Branchless bitmask vectorization)** |
| **Batch Inspection Throughput** | $0.78\text{ trays/s}$ (1 Thread) | **$4.41\text{ trays/s}$** (12 Cores OpenMP)| **$5.65\times\text{ scaling}$ (30 trays evaluated in 6.81 s)** |
| **Memory Allocator Churn** | $48.2\text{ MB/s}$ (`glibc malloc`) | **$4.1\text{ MB/s}$** (`mimalloc 2.1`)| **$11.7\times\text{ reduction in heap contention}$** |
| **ESP32-S3 Stream Latency** | $145\text{ ms}$ (JSON poll) | **$57.5\text{ ms}$** (Keep-Alive Header) | **Zero-overhead in-header telemetry streaming** |

---

## 🔌 Hardware Pinout (BawalVision v3 Schematic)

| Function | Signal Name | ESP32-S3 GPIO | Pin Header / Notes |
|---|---|---|---|
| **ST7735 TFT Display (SPI)** | `LCD_MOSI` | `GPIO 14` | Pin 19 (`IO14` / Master Out Slave In) |
| | `LCD_SCK` | `GPIO 21` | Pin 37 (`IO21` / SPI Clock) |
| | `LCD_DC` | `GPIO 45` | Pin 34 (`IO45` / Data-Command Select) |
| | `LCD_RST` | `GPIO 39` | Pin 39 (`IO39` / Panel Hardware Reset) |
| | `LCD_BCKL` | `GPIO 38` | Pin 38 (`IO38` / Display Backlight Control) |
| | `LCD_CS` | `-1` | Tied to `GND` (Direct Ground, CS-less mode) |
| **System Control & I/O** | `PIN_BTN_TRIGGER` | `GPIO 48` | Inspection trigger switch (Active LOW with internal pull-up) |
| | `PIN_LED_FLASH` | `GPIO 2` | Pin 24 (`IO2` / Illumination LED trigger) |
| | `PIN_BTN_BOOT` | `GPIO 0` | Pin 33 (`IO0` / Bootloader mode switch) |
| **OV3660 / OV5640 Camera (DVP)** | `CAM_XCLK` | `GPIO 15` | Pin 7 (`IO15` / Master Clock 20 MHz) |
| | `CAM_PCLK` | `GPIO 13` | Pin 18 (`IO13` / Pixel Clock) |
| | `CAM_VSYNC` | `GPIO 6` | Pin 5 (`IO6` / Vertical Sync) |
| | `CAM_HREF` | `GPIO 7` | Pin 6 (`IO7` / Horizontal Reference) |
| | `CAM_SIOD` | `GPIO 4` | Pin 3 (`IO4` / SCCB I2C SDA) |
| | `CAM_SIOC` | `GPIO 5` | Pin 4 (`IO5` / SCCB I2C SCL) |
| | `CAM_D0..D7` (Y2..Y9) | `11, 9, 8, 10, 12, 18, 17, 16` | 8-bit Parallel DVP Data Bus |
| | `CAM_PWDN` | `-1` | Tied to `GND` |
| | `CAM_RESET` | `-1` | Tied to `3V3` |

---

## 🧾 Hardware Bill of Materials (BOM) — BawalVision v7

The complete engineering Bill of Materials sourced from [`hardware/bom/BawalVision v7.xlsx`](hardware/bom/BawalVision%20v7.xlsx):

| Reference Designator | Model / Part | Quantity | Description / Application |
|---|---|:---:|---|
| **Microcontroller** | ESP32-S3-WROOM CAM Dev Board | 1 | Dual-core Xtensa LX7 @ 240 MHz, 8 MB PSRAM, 16 MB Flash |
| **CAM1** | OmniVision OV3660 | 1 | 3-Megapixel CMOS image sensor with 24-pin 0.5 mm pitch FPC interface |
| **TFT Screen** | 1.8" SPI TFT (ST7735) | 1 | 128×160 RGB display for on-device LVGL telemetry HUD |
| **IC1** | LM7805 | 1 | 5V linear voltage regulator with aluminum heatsink |
| **TP5100** | TP5100 | 1 | 2A lithium battery charge management controller |
| **FDC-2S-200** | FDC-2S-200 | 1 | 2S Lithium-Ion Battery Management System (BMS) protection board |
| **BT1** | 2S Battery Pack | 1 | Portable DC battery power pack |
| **PPTC** | Resettable PolySwitch | 2 | 0.7A in series for overcurrent & thermal cutoff protection |
| **9mm-DC-JACK** | Female DC Jack (9 mm) | 1 | External DC power input port |
| **S3** | Rocker Switch | 1 | Master power ON/OFF switch |
| **S1** | Pushbutton Switch | 1 | Inspection illumination trigger switch |
| **D1** | White LED | 1 | Food tray target area lighting |
| **C** | Ceramic Capacitor | 2 | Decoupling capacitors |
| **Thermal Putty** | Thermal interface compound | 1 tub | PPTC thermal coupling & heat management |
| **Pin Header** | 2.54 mm Female Header | 1 set | Socket mount for ESP32-S3 CAM development board |
| **Terminal Block** | 2-pin Screw Terminal (1×2) | 3 | Modular wire interconnects |
| **Perfboard** | Prototyping Board | 2 | FR2 for power assembly, FR4 for motherboard |
| **Hardware** | Nuts, bolts, standoffs, cabling | Assorted | Chassis standoffs, mechanical fasteners, and hookup wires |
| **Enclosure** | Generic Electronic Box | 1 | Enclosure with cutouts for LCD, camera, switches, and DC jack |

---

### 📸 Physical Device Gallery
Detailed teardown, schematics, and internal layout photos are documented in [**`hardware/README.md`**](hardware/README.md).

| Enclosure & Display HUD | Motherboard & ESP32-S3 | Power Assembly & BMS |
|:---:|:---:|:---:|
| <img src="hardware/docs/assets/Screen.jpeg" width="240" alt="Screen View" /> | <img src="hardware/docs/assets/motherboard.jpeg" width="240" alt="Motherboard" /> | <img src="hardware/docs/assets/power_assemvly.jpeg" width="240" alt="Power Assembly" /> |
| **Top Chassis View** | **Internal Exploded Assembly** | |
| <img src="hardware/docs/assets/topview.jpeg" width="240" alt="Top View" /> | <img src="hardware/docs/assets/exploded_view.jpeg" width="240" alt="Exploded View" /> | |

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
| `just train-ai1 [epochs] [batch] [w]` | Train YOLOv8s object detector, export static ONNX, and verify numerical parity |
| `just finetune-ai1 [w] [e] [b] [lr] [fz]` | **Incremental transfer learning with backbone layer freezing (0-9)** to prevent catastrophic forgetting |
| `just scrap-dish [cat] [target]` | Hard-negative tableware scraper (clean plates) with automatic 0-byte negative label injection |
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
| `just build-backend` | Compile the C++ Crow backend (`bawal_backend`) |
| `just run-backend` | Run PostgreSQL in Docker and the C++ backend on the host |
| `just test-api [base_url]` | Run the Crow C++ REST API verification suite |
| `just install-frontend` | Install SvelteKit npm dependencies (`npm install`) |
| `just build-frontend` | Build production bundle for SvelteKit HMI |
| `just check-frontend` | Run Svelte TypeScript diagnostic check (`npm run check`) |
| `just run-frontend` | Run SvelteKit development server with live reload |
| `just clean` | Remove all build artifacts (`services/build`, frontend build output) |

---

### 3. Docker & Infrastructure (`scripts/backend.just`)

| Command | Description |
|---|---|
| `just compose-up [service]` | Start containers in background (`postgres`, `bawal_backend`) |
| `just compose-down` | Stop and remove running containers |
| `just compose-build [service]` | Build container images via Docker Compose |
| `just compose-logs [service]` | View live aggregated container logs |
| `just edge-proxy` | Optional host TCP bridge for Docker Desktop LAN workarounds |
| `just edge-stack` | Start PostgreSQL + C++ backend in Docker |
| `just allow-firewall [port]` | Configure firewall rules for gateway port (firewalld / UFW / Windows) |
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

### 2. Launch Stack
```bash
# Terminal 1: PostgreSQL + frontend + Grafana in Docker
just compose-up

# Terminal 2: backend on the host (required for Wi-Fi scanning)
just run-backend
```
- **Frontend HMI:** `http://localhost:3001`
- **C++ Crow API:** `http://localhost:8080/api/v1`
- **PostgreSQL:** `localhost:5433`

> **Wi-Fi scan:** `nmcli` needs the host's NetworkManager. On Docker Desktop the
> backend runs in a VM that cannot see the host Wi-Fi interface or D-Bus socket,
> so the compose backend is opt-in (`docker-backend` profile) and the default
> stack leaves it to `just run-backend`.
>
> ```bash
> curl -s http://127.0.0.1:8080/api/v1/devices/wifi/networks | jq .
> ```
>
> A native Docker Engine (not Desktop) can still run the backend container:
> `just compose-up "postgres bawal_backend"` (compose mounts
> `/run/dbus/system_bus_socket` into it).

### 3. Train & Export Object Detection Model
```bash
# Setup Python ML Virtual Environment
just setup-ml

# Run end-to-end parallel training pipeline
just pipeline-ai1

# Or incrementally fine-tune an existing checkpoint with layer freezing
just finetune-ai1
```
The trained model will be automatically validated and saved as `ml/AI1/model/yolov8s/ai1_vision.onnx` (and `ml/AI1/model/yolov8s_int8/ai1_vision_int8.onnx`).

### 4. Build & Flash Edge Firmware
```bash
# Compile and flash ESP32-S3 firmware over USB-C
just run-edge /dev/ttyACM0
```

---

## 👥 Core Engineering Team & Contributors

| Member | Role & Ownership | Primary Focus Areas |
|:---|:---|:---|
| **Faris Wirakusuma** ([@fariswirakusuma](https://github.com/fariswirakusuma)) | Lead Architect & Backend Lead | C++20 Vision Engine, AVX2 SIMD, POSIX SHM Ingress, Crow REST Backend, AWS Lambda C++ |
| **Rasya Khairi** ([@OurangIlang](https://github.com/OurangIlang)) | ML & Frontend Lead | YOLO-World Relabeling, YOLOv8 Training & Layer-Frozen Fine-Tuning, INT8 PTQ Quantization, SvelteKit 5 HMI |
| **Daffa Rizqy Andika** ([@pondwatr](https://github.com/pondwatr)) | Embedded & Hardware Lead | ESP32-S3 Static FreeRTOS Firmware, OV5640 DVP Camera, st7735 LVGL Display|

> See full team profiles, acknowledgments, and code ownership in [**`CONTRIBUTORS.md`**](CONTRIBUTORS.md) and [`.github/CODEOWNERS`](.github/CODEOWNERS).

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
   - **C++ Backend:** `just compose-up` then `just test-api` (all endpoints must pass)
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