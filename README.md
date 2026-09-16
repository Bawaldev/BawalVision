# BawalVision

## 🛠 Task Runner & Command Cheatsheet

This project uses [just](https://github.com/casey/just) as a unified task runner. All commands are modularized inside the `scripts/` directory (`scripts/backend.just` and `scripts/edge.just`).

### Quick Start
List all available commands:
```bash
just
```

---

### 1. Backend & Vision Services (`scripts/backend.just`)

| Command | Description |
| --- | --- |
| `just setup-deps` | Install C++ dependencies (OpenCV, CMake, Ninja, etc.) on Ubuntu |
| `just configure-vision [target]` | Generate CMake build system for C++ Vision service (Default: `Release`) |
| `just build-vision [target]` | Compile C++ Vision service binary in parallel |
| `just run-vision` | Execute C++ Vision service locally |
| `just build-gateway` | Build Go Gateway REST API binary |
| `just run-gateway` | Run Go Gateway REST API locally |
| `just compose-up [service]` | Start infrastructure via Docker Compose (Foreground) |
| `just compose-up-d [service]` | Start infrastructure via Docker Compose (Background/Detached) |
| `just compose-backend` | Shortcut to build & start backend service via Docker Compose |
| `just compose-down` | Stop and clean up all Docker Compose containers |
| `just clean` | Remove all build artifacts (C++ Vision & Go binaries) |

---

### 2. ESP32-S3 Firmware & Edge (`scripts/edge.just`)

> **Note:** Ensure your `.env` file is configured with `WIFI_SSID`, `WIFI_PASS`, and `BACKEND_IP` before compiling the firmware.

| Command | Description |
| --- | --- |
| `just build-edge` | Build standard ESP32-S3 firmware |
| `just build-edge-wifi` | Build firmware while injecting Wi-Fi credentials & Backend IP from `.env` |
| `just flash-edge [port]` | Flash firmware binary to ESP32-S3 & open serial monitor (Default port: `/dev/ttyACM0`) |
| `just run-edge [port]` | **Shortcut:** Build (with `.env` Wi-Fi) + Flash + Serial Monitor |
| `just erase-nvs [port]` | Erase NVS partition to reset local Wi-Fi/configuration cache |
| `just menuconfig-edge` | Launch ESP-IDF interactive configuration menu |
| `just clean-edge` | Clean ESP-IDF build artifacts (`fullclean`) |



### Edge Node Compilation (ESP32-S3)
Ensure ESP-IDF (v5.x+) is installed on your machine.
- Default path: `$HOME/esp/esp-idf`
- Build firmware: `just build-edge`
- Custom toolchain path: `IDF_PATH=/path/to/esp-idf just build-edge`