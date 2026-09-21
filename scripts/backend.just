COMPOSE_FILE := "deploy/docker-compose.yml"

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

test-api base_url=("http://localhost:" + BACKEND_PORT + "/api/v1"):
    BASE_URL="{{ base_url }}" ./backend/scripts/test_api.sh

test-backend:
    cd backend && go test -v ./...

clean:
    rm -rf services/vision/build backend/bin

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
    sudo ufw allow {{ port }}/{{ proto }}

deny-firewall port=env_var_or_default("BACKEND_PORT", "8080") proto="tcp":
    sudo ufw delete allow {{ port }}/{{ proto }}