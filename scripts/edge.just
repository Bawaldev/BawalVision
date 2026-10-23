# Edge (ESP32-S3 Firmware) Recipes

DEFAULT_PORT := env_var_or_default("PORT", "/dev/ttyACM0")

# Build ESP32-S3 firmware
[group('Edge')]
build-edge:
    @just _idf-exec "build"
    cd {{ justfile_directory() }} && python3 scripts/gen_clangd_headers.py \
        --db edge/build/compile_commands.json \
        --include edge/main/include \
        --representative main/src/wifi.c

# Clean ESP-IDF build directory
[group('Edge')]
clean-edge:
    @just _idf-exec "fullclean"

# Reconfigure ESP-IDF CMake project
[group('Edge')]
configure-edge:
    @just _idf-exec "reconfigure"
    cd {{ justfile_directory() }} && python3 scripts/gen_clangd_headers.py \
        --db edge/build/compile_commands.json \
        --include edge/main/include \
        --representative main/src/wifi.c

# Erase flash / NVS on ESP32-S3
[group('Edge')]
erase-nvs port=DEFAULT_PORT:
    @just _idf-exec "-p {{ port }} erase-flash"

# Flash firmware and open serial monitor
[group('Edge')]
flash-edge port=DEFAULT_PORT:
    @just _idf-exec "-p {{ port }} flash monitor"

# Open ESP-IDF interactive menuconfig
[group('Edge')]
menuconfig-edge:
    @just _idf-exec "menuconfig"

# Open serial monitor on port
[group('Edge')]
monitor-edge port=DEFAULT_PORT:
    @just _idf-exec "-p {{ port }} monitor"

# Build, flash, and monitor ESP32-S3
[group('Edge')]
run-edge port=DEFAULT_PORT:
    just build-edge
    just flash-edge {{ port }}

[private]
_idf-exec cmd:
    #!/usr/bin/env bash
    set -eo pipefail
    if [ ! -f "$IDF_PATH/export.sh" ]; then
        echo "[ERROR] ESP-IDF export.sh not found at: $IDF_PATH" >&2
        echo "[HINT] Set path in .bashrc or via CLI: export IDF_PATH=/path/to/esp-idf" >&2
        exit 1
    fi
    set +u
    . "$IDF_PATH/export.sh" > /dev/null 2>&1
    set -u
    cd "{{ justfile_directory() }}/edge" && idf.py {{ cmd }}
