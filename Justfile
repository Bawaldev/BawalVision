set shell := ["bash", "-uc"]

# Menampilkan daftar perintah yang tersedia
default:
    @just --list

# Pasang dependensi sistem (Debian/Ubuntu)
setup-deps:
    sudo apt update
    sudo apt install -y build-essential cmake ninja-build pkg-config \
        libopencv-dev libssl-dev libcurl4-openssl-dev \
        nlohmann-json3-dev

# Konfigurasi generator CMake & ekspor compile_commands.json untuk clangd/LSP
configure target="Release":
    cmake -B build -S . \
        -DCMAKE_BUILD_TYPE={{ target }} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Kompilasi C++20 secara paralel (otomatis panggil configure jika folder build belum ada)
build target="Release":
    @if [ ! -f "build/Makefile" ] && [ ! -f "build/build.ninja" ]; then just configure {{ target }}; fi
    cmake --build build --parallel $(nproc)

# Bersihkan direktori build lalu kompilasi ulang dari nol
rebuild target="Release": clean (build target)

# Jalankan biner bawal_sentry dengan argumen konfigurasi default
run config="config/sentry_config.json":
    ./build/bin/bawal_sentry --config {{ config }}

# Bersihkan seluruh artefak kompilasi
clean:
    rm -rf build

# Build Docker image multi-stage untuk target ARM64
docker-build:
    docker build -t bawal-vision:latest .

# Jalankan container dengan akses USB Camera, GPIO, dan host network
docker-run:
    docker run --rm -it \
        --net=host \
        --device=/dev/video0:/dev/video0 \
        --device=/dev/gpiochip0:/dev/gpiochip0 \
        bawal-vision:latest


install-aws-sdk:
    sudo apt install -y uuid-dev zlib1g-dev libpulse-dev
    git clone --depth 1 --recurse-submodules https://github.com/aws/aws-sdk-cpp.git /tmp/aws-sdk-cpp
    cmake -B /tmp/aws-sdk-cpp/build -S /tmp/aws-sdk-cpp \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DENABLE_TESTING=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_ONLY="core;bedrock-runtime"
    cmake --build /tmp/aws-sdk-cpp/build --parallel $(nproc)
    sudo cmake --install /tmp/aws-sdk-cpp/build
    rm -rf /tmp/aws-sdk-cpp