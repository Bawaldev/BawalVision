set shell := ["bash", "-uc"]

# Fallback path for ESP-IDF if the environment variable is not set on the host
export IDF_PATH := env_var_or_default("IDF_PATH", env_var("HOME") + "/esp/esp-idf")

# Display available recipes
default:
    @just --list

# Install native dependencies for Linux / ARM64 Graviton host
setup-deps:
    sudo apt update
    sudo apt install -y build-essential cmake ninja-build pkg-config \
        libopencv-dev libomp-dev nlohmann-json3-dev

# Generate CMake build files and export compile_commands.json for clangd/LSP
configure-vision target="Release":
    cmake -B services/vision/build -S services/vision \
        -DCMAKE_BUILD_TYPE={{ target }} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Compile C++20 vision service in parallel (triggers configure if build tree is missing)
build-vision target="Release":
    @if [ ! -f "services/vision/build/Makefile" ] && [ ! -f "services/vision/build/build.ninja" ]; then just configure-vision {{ target }}; fi
    cmake --build services/vision/build --parallel $(nproc)

# Run the compiled vision service binary
run-vision:
    ./services/vision/build/bin/vision_service

# Build Golang backend orchestrator
build-gateway:
    cd backend && go build -o bin/gateway cmd/server/main.go

# Run Golang backend orchestrator
run-gateway:
    cd backend && go run cmd/server/main.go

# Remove compilation artifacts for both C++ and Go
clean:
    rm -rf services/vision/build
    rm -rf backend/bin

# Build ARM64 Docker container for vision service
docker-build-vision:
    docker build -f deploy/Dockerfile.vision -t bawal-vision:latest .

# Run containerized vision service with exposed network port
docker-run-vision:
    docker run --rm -it \
        -p 8080:8080 \
        bawal-vision:latest
# Build ESP32-S3 firmware via native CMake + Ninja
build-edge:
    @just _idf-exec "build"

# Flash firmware and open serial UART monitor
flash-edge port="/dev/ttyUSB0":
    @just _idf-exec "-p {{ port }} flash monitor"

# Launch interactive Kconfig system configuration
menuconfig-edge:
    @just _idf-exec "menuconfig"

# Clean firmware build tree artifacts
clean-edge:
    @just _idf-exec "fullclean"
# Helper internal: validate & load ESP-IDF toolchain in isolation
[private]
_idf-exec cmd:
    #!/usr/bin/env bash
    set -eo pipefail
    if [ ! -f "$IDF_PATH/export.sh" ]; then
        echo "[ERROR] ESP-IDF export.sh not found at: $IDF_PATH" >&2
        echo "[HINT] Set path manually: export IDF_PATH=/path/to/esp-idf" >&2
        exit 1
    fi
    # Disable unbound variable verification specifically while sourcing Espressif's setup script
    set +u
    . "$IDF_PATH/export.sh" > /dev/null 2>&1
    set -u
    cd edge && idf.py {{ cmd }}