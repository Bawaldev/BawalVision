COMPOSE_FILE := justfile_directory() + "/deploy/docker-compose.yml"

setup-deps:
    sudo apt update
    sudo apt install -y build-essential cmake ninja-build pkg-config \
        libopencv-dev libomp-dev libhwy-dev libfmt-dev libcpp-httplib-dev nlohmann-json3-dev

configure-vision target="Release":
    cd {{ justfile_directory() }} && cmake -B services/build -S services \
        -DCMAKE_BUILD_TYPE={{ target }} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build-vision target="Release":
    cd {{ justfile_directory() }} && if [ ! -f "services/build/Makefile" ] && [ ! -f "services/build/build.ninja" ]; then just configure-vision {{ target }}; fi
    cd {{ justfile_directory() }} && cmake --build services/build --parallel $(nproc)

run-vision device_id=env_var_or_default("DEVICE_ID", ""):
    cd {{ justfile_directory() }} && if [ ! -f "services/build/bin/vision_service" ]; then just build-vision; fi
    cd {{ justfile_directory() }} && DEVICE_ID="{{ device_id }}" ./services/build/bin/vision_service "{{ device_id }}"

# --- RACE CONDITION DETECTION (ThreadSanitizer) ---

configure-vision-tsan:
    cd {{ justfile_directory() }} && cmake -B services/build-tsan -S services \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build-vision-tsan:
    cd {{ justfile_directory() }} && if [ ! -f "services/build-tsan/Makefile" ] && [ ! -f "services/build-tsan/build.ninja" ]; then just configure-vision-tsan; fi
    cd {{ justfile_directory() }} && cmake --build services/build-tsan --parallel $(nproc)

run-vision-tsan device_id=env_var_or_default("DEVICE_ID", ""):
    #!/usr/bin/env bash
    if [ ! -f "services/build-tsan/bin/vision_service" ]; then
        just build-vision-tsan
    fi
    setarch $(uname -m) -R gdb \
        -ex "set environment DEVICE_ID={{ device_id }}" \
        -ex "set environment TSAN_OPTIONS=suppressions={{ justfile_directory() }}/services/tsan_suppressions.txt:ignore_noninstrumented_modules=1:second_deadlock_stack=1" \
        -ex "run" \
        --args ./services/build-tsan/bin/vision_service "{{ device_id }}"
# ------------------------------------------------------

build-gateway:
    cd {{ justfile_directory() }}/backend && go build -o bin/gateway cmd/server/main.go

run-gateway:
    @just allow-firewall
    cd {{ justfile_directory() }}/backend && \
        DB_HOST={{ env_var_or_default("DB_HOST", "localhost") }} \
        DB_PORT={{ env_var_or_default("DB_PORT", env_var_or_default("POSTGRES_PORT", "5433")) }} \
        go run cmd/server/main.go

# ESP32-S3 edge stack: Postgres in compose + backend on the host.
edge-stack:
    # Docker Desktop for Linux cannot forward LAN TCP payloads to published
    # container ports (UDP discovery still works), so edge frames/telemetry
    # must reach a host-side backend.
    @just compose-up postgres
    @docker compose -f {{ COMPOSE_FILE }} stop backend >/dev/null 2>&1 || true
    cd {{ justfile_directory() }}/backend && \
        DB_HOST={{ env_var_or_default("DB_HOST", "localhost") }} \
        DB_PORT={{ env_var_or_default("DB_PORT", env_var_or_default("POSTGRES_PORT", "5433")) }} \
        go run cmd/server/main.go

test-api base_url=("http://localhost:" + env_var_or_default("BACKEND_PORT", "8080") + "/api/v1"):
    cd {{ justfile_directory() }} && BASE_URL="{{ base_url }}" ./backend/scripts/test_api.sh

test-backend:
    cd {{ justfile_directory() }}/backend && go test -v ./...

clean:
    rm -rf {{ justfile_directory() }}/services/build {{ justfile_directory() }}/services/build-tsan {{ justfile_directory() }}/backend/bin {{ justfile_directory() }}/frontend/.svelte-kit/output

# --- FRONTEND (Svelte 5 + TypeScript) ---
install-frontend:
    cd {{ justfile_directory() }}/frontend && npm install

run-frontend:
    cd {{ justfile_directory() }}/frontend && npm run dev

build-frontend:
    cd {{ justfile_directory() }}/frontend && npm run build

check-frontend:
    cd {{ justfile_directory() }}/frontend && npm run check
# ----------------------------------------

# --- CONTAINER & DOCKER ORCHESTRATION ---
compose-up service="":
    cd {{ justfile_directory() }} && docker compose -f {{ COMPOSE_FILE }} up -d {{ service }}

compose-down:
    cd {{ justfile_directory() }} && docker compose -f {{ COMPOSE_FILE }} down

compose-build service="":
    cd {{ justfile_directory() }} && docker compose -f {{ COMPOSE_FILE }} build {{ service }}

compose-logs service="":
    cd {{ justfile_directory() }} && docker compose -f {{ COMPOSE_FILE }} logs -f {{ service }}

# Host-side TCP bridge for ESP32-S3 -> Docker backend on Docker Desktop.
edge-proxy host_port=env_var_or_default("BACKEND_PORT", "8080") container_port=env_var_or_default("BACKEND_HOST_PORT", "18080"):
    #!/usr/bin/env bash
    set -euo pipefail
    if ! command -v socat >/dev/null 2>&1; then
        echo "[ERROR] socat is required for the Docker Desktop LAN bridge." >&2
        echo "[HINT] Install it with: sudo apt install socat  (or sudo dnf install socat)" >&2
        exit 1
    fi
    echo "[INFO] Bridging 0.0.0.0:{{ host_port }} -> 127.0.0.1:{{ container_port }} (Docker Desktop workaround)"
    exec socat TCP-LISTEN:{{ host_port }},reuseaddr,fork TCP:127.0.0.1:{{ container_port }}

stop-edge-proxy:
    #!/usr/bin/env bash
    set -euo pipefail
    pids="$(pgrep -f 'socat TCP-LISTEN:.*TCP:127\.0\.0\.1:18080' || true)"
    if [ -z "$pids" ]; then
        echo "[INFO] No edge-proxy process found."
        exit 0
    fi
    kill $pids 2>/dev/null || true
    echo "[INFO] Stopped edge-proxy ($pids)."

allow-firewall port=env_var_or_default("BACKEND_PORT", "8080") proto="tcp":
    #!/usr/bin/env bash
    set -eo pipefail
    discovery_port="${DISCOVERY_PORT:-8765}"
    if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
        echo "[INFO] Configuring firewalld for {{ port }}/{{ proto }} and ${discovery_port}/udp..."
        sudo firewall-cmd --permanent --add-port={{ port }}/{{ proto }} >/dev/null 2>&1 || true
        sudo firewall-cmd --permanent --add-port=${discovery_port}/udp >/dev/null 2>&1 || true
        sudo firewall-cmd --reload >/dev/null 2>&1 || true
    elif command -v ufw >/dev/null 2>&1; then
        echo "[INFO] Configuring Linux UFW Firewall for port {{ port }}/{{ proto }} and ${discovery_port}/udp..."
        sudo ufw allow {{ port }}/{{ proto }} >/dev/null 2>&1 || true
        sudo ufw allow ${discovery_port}/udp >/dev/null 2>&1 || true
    elif [ "$OS" = "Windows_NT" ] || command -v netsh >/dev/null 2>&1; then
        echo "[INFO] Configuring Windows Firewall for port {{ port }}..."
        netsh advfirewall firewall add rule name="BawalVision_{{ port }}" dir=in action=allow protocol={{ proto }} localport={{ port }} >/dev/null 2>&1 || true
        netsh advfirewall firewall add rule name="BawalVision_discovery" dir=in action=allow protocol=udp localport=${discovery_port} >/dev/null 2>&1 || true
    fi

deny-firewall port=env_var_or_default("BACKEND_PORT", "8080") proto="tcp":
    #!/usr/bin/env bash
    set -eo pipefail
    discovery_port="${DISCOVERY_PORT:-8765}"
    if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
        echo "[INFO] Removing firewalld rules for {{ port }}/{{ proto }} and ${discovery_port}/udp..."
        sudo firewall-cmd --permanent --remove-port={{ port }}/{{ proto }} >/dev/null 2>&1 || true
        sudo firewall-cmd --permanent --remove-port=${discovery_port}/udp >/dev/null 2>&1 || true
        sudo firewall-cmd --reload >/dev/null 2>&1 || true
    elif command -v ufw >/dev/null 2>&1; then
        echo "[INFO] Removing Linux UFW rules for {{ port }}/{{ proto }} and ${discovery_port}/udp..."
        sudo ufw delete allow {{ port }}/{{ proto }} >/dev/null 2>&1 || true
        sudo ufw delete allow ${discovery_port}/udp >/dev/null 2>&1 || true
    elif [ "$OS" = "Windows_NT" ] || command -v netsh >/dev/null 2>&1; then
        echo "[INFO] Removing Windows Firewall rules for port {{ port }}..."
        netsh advfirewall firewall delete rule name="BawalVision_{{ port }}" >/dev/null 2>&1 || true
        netsh advfirewall firewall delete rule name="BawalVision_discovery" >/dev/null 2>&1 || true
    fi