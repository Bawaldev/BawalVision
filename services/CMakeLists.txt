cmake_minimum_required(VERSION 3.22)

project(bawal_vision_service
    VERSION 1.0.0
    DESCRIPTION "Vision processing service for MBG inspection"
    LANGUAGES CXX
)

# Standard C++20 runtime configuration
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Strict warnings for systems stability
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# Default to Release build if unspecified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# Compiler-level SIMD vectorization targeted for AWS Graviton (Neoverse-N1)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        add_compile_options(-march=armv8.2-a+simd -mtune=neoverse-n1)
    endif()
endif()

# Core dependencies: concurrency, image processing, and serialization
find_package(Threads REQUIRED)
find_package(OpenMP REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc calib3d)
find_package(nlohmann_json 3.2.0 REQUIRED)

# Network transport for local IPC / Go backend streaming
include(FetchContent)
FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v11.4.4
)
set(USE_TLS OFF CACHE BOOL "" FORCE) # Disabled for local zero-overhead loopback
FetchContent_MakeAvailable(ixwebsocket)

# Service executable definition
add_executable(vision_service
    src/main.cpp
    src/vision/calibration.cpp
    src/vision/dewarp.cpp
    src/vision/pipeline.cpp
    src/vision/segmenter.cpp
    src/vision/types.cpp
)

# Public API include paths (supports umbrella header pattern)
target_include_directories(vision_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
)

# Linker resolution
target_link_libraries(vision_service PRIVATE
    Threads::Threads
    OpenMP::OpenMP_CXX
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
    ixwebsocket
)

install(TARGETS vision_service DESTINATION bin)