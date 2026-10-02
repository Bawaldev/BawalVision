COMPOSE_FILE := "deploy/docker-compose.yml"

setup-deps:
    sudo apt update
    sudo apt install -y build-essential cmake ninja-build pkg-config \
        libopencv-dev libomp-dev nlohmann-json3-dev

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
    cd {{ justfile_directory() }}/backend && go run cmd/server/main.go

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
docker-build-backend:
    docker build -f deploy/Dockerfile.backend -t bawal-backend:latest .

docker-build-vision:
    docker build -f deploy/Dockerfile.vision -t bawal-vision:latest .

docker-build-frontend:
    docker build -f deploy/Dockerfile.frontend -t bawal-frontend:latest .

docker-build-ml:
    docker build -f deploy/Dockerfile.ml -t bawal-ml:latest .

docker-build-all: docker-build-backend docker-build-vision docker-build-frontend docker-build-ml

docker-run-vision:
    docker run --rm -it -p 8080:8080 bawal-vision:latest

compose-up service="":
    docker compose -f {{ COMPOSE_FILE }} up --build {{ service }}

compose-up-all:
    docker compose -f {{ COMPOSE_FILE }} --profile ml up --build

compose-up-d service="":
    docker compose -f {{ COMPOSE_FILE }} up -d --build {{ service }}

compose-up-d-all:
    docker compose -f {{ COMPOSE_FILE }} --profile ml up -d --build

compose-backend:
    docker compose -f {{ COMPOSE_FILE }} up --build backend

compose-down:
    docker compose -f {{ COMPOSE_FILE }} down

compose-down-v:
    docker compose -f {{ COMPOSE_FILE }} down -v

compose-logs service="":
    docker compose -f {{ COMPOSE_FILE }} logs -f {{ service }}

compose-ps:
    docker compose -f {{ COMPOSE_FILE }} ps

compose-restart service="":
    docker compose -f {{ COMPOSE_FILE }} restart {{ service }}

allow-firewall port=env_var_or_default("BACKEND_PORT", "8080") proto="tcp":
    #!/usr/bin/env bash
    if command -v ufw >/dev/null 2>&1; then
        echo "[INFO] Configuring Linux UFW Firewall for port {{ port }}/{{ proto }}..."
        sudo ufw allow {{ port }}/{{ proto }} >/dev/null 2>&1 || true
    elif [ "$OS" = "Windows_NT" ] || command -v netsh >/dev/null 2>&1; then
        echo "[INFO] Configuring Windows Firewall for port {{ port }}..."
        netsh advfirewall firewall add rule name="BawalVision_{{ port }}" dir=in action=allow protocol={{ proto }} localport={{ port }} >/dev/null 2>&1 || true
    fi

deny-firewall port=env_var_or_default("BACKEND_PORT", "8080") proto="tcp":
    #!/usr/bin/env bash
    if command -v ufw >/dev/null 2>&1; then
        echo "[INFO] Removing Linux UFW rule for port {{ port }}/{{ proto }}..."
        sudo ufw delete allow {{ port }}/{{ proto }} >/dev/null 2>&1 || true
    elif [ "$OS" = "Windows_NT" ] || command -v netsh >/dev/null 2>&1; then
        echo "[INFO] Removing Windows Firewall rule for port {{ port }}..."
        netsh advfirewall firewall delete rule name="BawalVision_{{ port }}" >/dev/null 2>&1 || true
    fi