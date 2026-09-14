set shell := ["bash", "-uc"]

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