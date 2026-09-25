build-edge:
    @just _idf-exec "build"

configure-edge:
    @just _idf-exec "reconfigure"

build-edge-wifi:
    @just _idf-exec "build"

flash-edge port=env_var_or_default("PORT", "/dev/ttyACM0"):
    @just _idf-exec "-p {{ port }} flash monitor"

run-edge port=env_var_or_default("PORT", "/dev/ttyACM0"):
    just build-edge-wifi
    just flash-edge {{ port }}

erase-nvs port=env_var_or_default("PORT", "/dev/ttyACM0"):
    @just _idf-exec "-p {{ port }} erase-flash"

menuconfig-edge:
    @just _idf-exec "menuconfig"

clean-edge:
    @just _idf-exec "fullclean"

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
    cd "{{ justfile_directory() }}/edge" && idf.py {{ cmd }}