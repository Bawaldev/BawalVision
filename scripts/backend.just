COMPOSE_FILE := justfile_directory() + "/deploy/docker-compose.yml"
BACKEND_PORT := env_var_or_default("BACKEND_PORT", "8080")
BACKEND_HOST_PORT := env_var_or_default("BACKEND_HOST_PORT", "18080")
POSTGRES_PORT := env_var_or_default("DB_PORT", env_var_or_default("POSTGRES_PORT", "5433"))
API_URL := "http://localhost:" + BACKEND_PORT + "/api/v1"
DEV_ID := env_var_or_default("DEVICE_ID", "")

# --- VISION (C++20 & OpenCV) ---

# Install native C++ and OpenCV apt dependencies
[group('Vision')]
setup-deps:
    sudo apt update
    sudo apt install -y build-essential cmake ninja-build pkg-config \
        libopencv-dev libomp-dev libfmt-dev libcpp-httplib-dev nlohmann-json3-dev \
        libcurl4-openssl-dev libssl-dev libpq-dev

# Configure C++ vision service with CMake
[group('Vision')]
configure-vision target="Release":
    cd {{ justfile_directory() }} && cmake -B services/build -S services \
        -DCMAKE_BUILD_TYPE={{ target }} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cd {{ justfile_directory() }} && python3 scripts/gen_clangd_headers.py \
        --db services/build/compile_commands.json \
        --include services/include \
        --include ml/include \
        --representative src/backend/app.cpp

# Append header entries to compile_commands.json so clangd resolves their flags
[group('Vision')]
clangd-db:
    cd {{ justfile_directory() }} && python3 scripts/gen_clangd_headers.py \
        --db services/build/compile_commands.json \
        --include services/include \
        --include ml/include \
        --representative src/backend/app.cpp

# Build C++ vision service
[group('Vision')]
build-vision target="Release":
    cd {{ justfile_directory() }} && if [ ! -f "services/build/Makefile" ] && [ ! -f "services/build/build.ninja" ]; then just configure-vision {{ target }}; fi
    cd {{ justfile_directory() }} && cmake --build services/build --parallel $(nproc)

# Run C++ vision inspection service
[group('Vision')]
run-vision device_id=DEV_ID:
    cd {{ justfile_directory() }} && if [ ! -f "services/build/bin/vision_service" ]; then just build-vision; fi
    cd {{ justfile_directory() }} && DEVICE_ID="{{ device_id }}" ./services/build/bin/vision_service "{{ device_id }}"

# Configure ThreadSanitizer build for vision service
[group('Vision')]
configure-vision-tsan:
    cd {{ justfile_directory() }} && cmake -B services/build-tsan -S services \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Build ThreadSanitizer binary for vision service
[group('Vision')]
build-vision-tsan:
    cd {{ justfile_directory() }} && if [ ! -f "services/build-tsan/Makefile" ] && [ ! -f "services/build-tsan/build.ninja" ]; then just configure-vision-tsan; fi
    cd {{ justfile_directory() }} && cmake --build services/build-tsan --parallel $(nproc)

# Run vision service under gdb with ThreadSanitizer
[group('Vision')]
run-vision-tsan device_id=DEV_ID:
    #!/usr/bin/env bash
    if [ ! -f "services/build-tsan/bin/vision_service" ]; then
        just build-vision-tsan
    fi
    setarch $(uname -m) -R gdb \
        -ex "set environment DEVICE_ID={{ device_id }}" \
        -ex "set environment TSAN_OPTIONS=suppressions={{ justfile_directory() }}/services/tsan_suppressions.txt:ignore_noninstrumented_modules=1:second_deadlock_stack=1" \
        -ex "run" \
        --args ./services/build-tsan/bin/vision_service "{{ device_id }}"

# --- BACKEND (C++20 Crow + PostgreSQL) ---

# Build the unified C++ backend (Crow HTTP + in-process ONNX vision)
[group('Backend')]
build-backend:
    cd {{ justfile_directory() }} && if [ ! -f "services/build/Makefile" ] && [ ! -f "services/build/build.ninja" ]; then just configure-vision; fi
    cd {{ justfile_directory() }} && cmake --build services/build --target bawal_backend --parallel $(nproc)

# Run the C++ backend on the host against the Docker PostgreSQL instance.
# The host process is the only way to scan the host's Wi-Fi through nmcli on
# Docker Desktop, which runs containers in a VM without access to wlp*/NM.
[group('Backend')]
run-backend:
    @just compose-up postgres
    -cd {{ justfile_directory() }} && docker compose -f {{ COMPOSE_FILE }} stop bawal_backend >/dev/null 2>&1 || true
    cd {{ justfile_directory() }} && \
        CROW_PORT={{ BACKEND_PORT }} \
        DB_HOST={{ env_var_or_default("DB_HOST", "localhost") }} \
        DB_PORT={{ POSTGRES_PORT }} \
        ./services/build/bin/bawal_backend

# Start PostgreSQL and the C++ backend in Docker (native Docker Engine only;
# Docker Desktop cannot scan host Wi-Fi from a container)
[group('Backend')]
edge-stack:
    @just compose-up "postgres bawal_backend"

# Run the Crow C++ API integration suite against a live backend
[group('Backend')]
test-api base_url=API_URL:
    cd {{ justfile_directory() }} && BASE_URL="{{ base_url }}" ./deploy/test_api.sh

# Legacy alias: the Go test suite was removed with backend/
[group('Backend')]
test-backend:
    @echo "Go backend removed. Run 'just test-api' against a live C++ backend instead."

# --- FRONTEND (Svelte 5 + TypeScript) ---

# Install npm dependencies for SvelteKit frontend
[group('Frontend')]
install-frontend:
    cd {{ justfile_directory() }}/frontend && npm install

# Run SvelteKit frontend in development mode
[group('Frontend')]
run-frontend:
    cd {{ justfile_directory() }}/frontend && npm run dev

# Build production assets for frontend
[group('Frontend')]
build-frontend:
    cd {{ justfile_directory() }}/frontend && npm run build

# Run svelte-check TypeScript validation
[group('Frontend')]
check-frontend:
    cd {{ justfile_directory() }}/frontend && npm run check

# --- CONTAINER & DOCKER ORCHESTRATION ---

# Start docker-compose service(s) in background
[group('Docker')]
compose-up service="":
    cd {{ justfile_directory() }} && docker compose -f {{ COMPOSE_FILE }} up -d {{ service }}

# Stop all docker-compose services
[group('Docker')]
compose-down:
    cd {{ justfile_directory() }} && docker compose -f {{ COMPOSE_FILE }} down

# Build or rebuild docker-compose service images (includes opt-in backend)
[group('Docker')]
compose-build service="":
    cd {{ justfile_directory() }} && docker compose -f {{ COMPOSE_FILE }} --profile docker-backend build {{ service }}

# Follow docker-compose logs for service(s)
[group('Docker')]
compose-logs service="":
    cd {{ justfile_directory() }} && docker compose -f {{ COMPOSE_FILE }} logs -f {{ service }}

# --- NETWORK & FIREWALL UTILITIES ---

# Bridge LAN :8080 to Docker :18080
[group('Network')]
edge-proxy host_port=BACKEND_PORT container_port=BACKEND_HOST_PORT:
    #!/usr/bin/env bash
    set -euo pipefail
    if ! command -v socat >/dev/null 2>&1; then
        echo "[ERROR] socat is required for the Docker Desktop LAN bridge." >&2
        echo "[HINT] Install it with: sudo apt install socat  (or sudo dnf install socat)" >&2
        exit 1
    fi
    echo "[INFO] Bridging 0.0.0.0:{{ host_port }} -> 127.0.0.1:{{ container_port }} (Docker Desktop workaround)"
    exec socat TCP-LISTEN:{{ host_port }},reuseaddr,fork TCP:127.0.0.1:{{ container_port }}

# Terminate running edge-proxy bridge processes
[group('Network')]
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

# Allow TCP backend and UDP discovery ports through firewall
[group('Network')]
allow-firewall port=BACKEND_PORT proto="tcp":
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

# Remove firewall rules for backend and UDP discovery
[group('Network')]
deny-firewall port=BACKEND_PORT proto="tcp":
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

# --- GENERAL UTILITIES ---

# Clean build artifacts across all services
[group('General')]
clean:
    rm -rf {{ justfile_directory() }}/services/build {{ justfile_directory() }}/services/build-tsan {{ justfile_directory() }}/frontend/.svelte-kit/output
