# 1. Load file .env dan set shell
set dotenv-load := true
set shell := ["bash", "-uc"]

# 2. Export environment variables secara global di level justfile
export WIFI_SSID := env_var_or_default("WIFI_SSID", "")
export WIFI_PASS := env_var_or_default("WIFI_PASS", "")
export BACKEND_IP := env_var_or_default("BACKEND_IP", "")
export BACKEND_PORT := env_var_or_default("BACKEND_PORT", "8080")

# Fallback path for ESP-IDF
export IDF_PATH := env_var_or_default("IDF_PATH", env_var("HOME") + "/esp/esp-idf")

COMPOSE_FILE := "deploy/docker-compose.yml"

default:
    @just --list

# --- Vision & Backend Recipes ---
setup-deps:
    sudo apt update
    sudo apt install -y build-essential cmake ninja-build pkg-config \
        libopencv-dev libomp-dev nlohmann-json3-dev

configure-vision target="Release":
    cmake -B services/vision/build -S services/vision \
        -DCMAKE_BUILD_TYPE={{ target }} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build-vision target="Release":
    @if [ ! -f "services/vision/build/Makefile" ] && [ ! -f "services/vision/build/build.ninja" ]; then just configure-vision {{ target }}; fi
    cmake --build services/vision/build --parallel $(nproc)

run-vision:
    ./services/vision/build/bin/vision_service

build-gateway:
    cd backend && go build -o bin/gateway cmd/server/main.go

run-gateway:
    cd backend && go run cmd/server/main.go

clean:
    rm -rf services/vision/build
    rm -rf backend/bin

docker-build-vision:
    docker build -f deploy/Dockerfile.vision -t bawal-vision:latest .

docker-run-vision:
    docker run --rm -it -p 8080:8080 bawal-vision:latest

compose-up service="":
    docker compose -f {{ COMPOSE_FILE }} up --build {{ service }}

compose-up-d service="":
    docker compose -f {{ COMPOSE_FILE }} up -d --build {{ service }}

compose-backend:
    docker compose -f {{ COMPOSE_FILE }} up --build backend

compose-down:
    docker compose -f {{ COMPOSE_FILE }} down

# --- ESP32-S3 Firmware Recipes ---

# Build standar tanpa override Wi-Fi
build-edge:
    @just _idf-exec "build"

# Build firmware dengan menyuntikkan kredensial Wi-Fi dari .env secara aman
build-edge-wifi:
    @if [ -z "$WIFI_SSID" ]; then echo "[ERROR] WIFI_SSID belum diisi di file .env!" >&2; exit 1; fi
    @if [ -z "$BACKEND_IP" ]; then echo "[ERROR] BACKEND_IP belum diisi di file .env!" >&2; exit 1; fi
    @just _idf-exec 'build -DCONFIG_ESP_WIFI_SSID="$WIFI_SSID" -DCONFIG_ESP_WIFI_PASSWORD="$WIFI_PASS" -DCONFIG_BACKEND_IP="$BACKEND_IP" -DCONFIG_BACKEND_PORT="$BACKEND_PORT"'

# Flash firmware dan jalankan monitor
flash-edge port=env_var_or_default("PORT", "/dev/ttyACM0"):
    @just _idf-exec "-p {{ port }} flash monitor --no-reset"

# Shortcut: Build (dengan Wi-Fi .env), Flash, dan Monitor
run-edge port=env_var_or_default("PORT", "/dev/ttyACM0"):
    just build-edge-wifi
    just flash-edge {{ port }}

# Erase NVS partition saja untuk reset Wi-Fi cache
erase-nvs port=env_var_or_default("PORT", "/dev/ttyACM0"):
    @just _idf-exec "-p {{ port }} erase-flash"

menuconfig-edge:
    @just _idf-exec "menuconfig"

clean-edge:
    @just _idf-exec "fullclean"

# Helper internal: validate & load ESP-IDF toolchain in isolation
[private]
_idf-exec cmd:
    #!/usr/bin/env bash
    set -eo pipefail
    if [ ! -f "$IDF_PATH/export.sh" ]; then
        echo "[ERROR] ESP-IDF export.sh tidak ditemukan di: $IDF_PATH" >&2
        echo "[HINT] Set path di .bashrc atau via CLI: export IDF_PATH=/path/ke/esp-idf" >&2
        exit 1
    fi
    set +u
    . "$IDF_PATH/export.sh" > /dev/null 2>&1
    set -u
    cd edge && idf.py {{ cmd }}