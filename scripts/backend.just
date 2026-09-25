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
    cd {{ justfile_directory() }} && DEVICE_ID="{{ device_id }}" ./services/build/bin/vision_service {{ device_id }}
    
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
    rm -rf {{ justfile_directory() }}/services/build {{ justfile_directory() }}/backend/bin

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