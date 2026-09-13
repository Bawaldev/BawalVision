cmake_minimum_required(VERSION 3.22)

project(bawal_vision
    VERSION 1.0.0
    DESCRIPTION "Real-time edge security sentry with hardware interlock"
    LANGUAGES CXX
)

# ------------------------------------------------------------------------------
# 1. Konfigurasi Standar C++20 & Direktori Output
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Letakkan hasil kompilasi biner ke direktori bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ------------------------------------------------------------------------------
# 2. Flag Kompiler & Optimasi Hardware (ARM NEON)
# ------------------------------------------------------------------------------
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    add_compile_options(-O3)
    
    # Deteksi arsitektur ARM64 untuk aktivasi SIMD NEON langsung ke prosesor
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        message(STATUS "Arsitektur ARM64 terdeteksi: Mengaktifkan optimasi NEON")
        add_compile_options(-march=armv8.2-a+simd -mtune=cortex-a76)
    endif()
endif()

# ------------------------------------------------------------------------------
# 3. Pencarian Dependensi Sistem
# ------------------------------------------------------------------------------
find_package(Threads REQUIRED)

# OpenCV (Minimal modul yang dibutuhkan pipeline visi)
find_package(OpenCV REQUIRED COMPONENTS
    core
    imgproc
    dnn
    videoio
    calib3d # Diperlukan untuk solvePnP
)

# AWS SDK for C++ (Hanya modul yang relevan)
find_package(AWSSDK REQUIRED COMPONENTS
    core
    bedrock-runtime
)

# JSON parser (nlohmann_json)
find_package(nlohmann_json 3.2.0 REQUIRED)

# ------------------------------------------------------------------------------
# 4. FetchContent Dependensi Jaringan (IXWebSocket)
# Digunakan agar WebSocket client/server langsung ter-build otomatis
# ------------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v11.4.4
)
# Nonaktifkan opsi yang tidak diperlukan untuk mempercepat kompilasi
set(USE_TLS OFF CACHE BOOL "" FORCE) # Gunakan OFF jika telemetri Three.js murni lokal ws://
FetchContent_MakeAvailable(ixwebsocket)

# ------------------------------------------------------------------------------
# 5. Definisi Sumber Kode Executable
# ------------------------------------------------------------------------------
set(BAWAL_SOURCES
    src/main.cpp
    
    # Modul Vision
    src/vision/camera_stream.cpp
    src/vision/face_detector.cpp
    src/vision/pose_estimator.cpp
    
    # Modul Hardware Interlock
    src/interlock/gpio_controller.cpp
    src/interlock/watchdog_feeder.cpp
    
    # Modul Network Telemetry
    src/network/ws_server.cpp
    
    # Modul Cloud Dispatcher
    src/cloud/aws_bridge.cpp
    src/cloud/incident_reporter.cpp
)

add_executable(bawal_sentry ${BAWAL_SOURCES})

# ------------------------------------------------------------------------------
# 6. Include Directories & Header Resolution
# ------------------------------------------------------------------------------
target_include_directories(bawal_sentry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${AWSSDK_INCLUDE_DIRS}
)

# ------------------------------------------------------------------------------
# 7. Linking Libraries (Target-Based)
# ------------------------------------------------------------------------------
target_link_libraries(bawal_sentry PRIVATE
    Threads::Threads
    ${OpenCV_LIBS}
    ${AWSSDK_LINK_LIBRARIES}
    nlohmann_json::nlohmann_json
    ixwebsocket
)

# ------------------------------------------------------------------------------
# 8. Instalasi / Deploy Target
# ------------------------------------------------------------------------------
install(TARGETS bawal_sentry DESTINATION bin)
install(DIRECTORY assets/ DESTINATION share/bawal_vision/assets)
install(DIRECTORY config/ DESTINATION share/bawal_vision/config)