cmake_minimum_required(VERSION 3.22)

# Generate compile_commands.json untuk IntelliSense VS Code
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
find_package(OpenCV REQUIRED COMPONENTS core imgproc calib3d imgcodecs highgui)
find_package(nlohmann_json QUIET)
find_package(CURL REQUIRED)

# ==============================================================================
# OTOMATISASI RECURSIVE SOURCE & INCLUDE PATH
# ==============================================================================

# 1. Otomatis tangkap SEMUA file .cpp di bawah folder src/ (termasuk subfolder server, vision, dll)
file(GLOB_RECURSE SERVICE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

# 2. Build executable dari seluruh sumber C++ yang ditemukan
add_executable(vision_service ${SERVICE_SOURCES})

# 3. Include directory diatur ke folder 'include' agar include path selalu relatif dari <bawal/...>
target_include_directories(vision_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

# Linker resolution
target_link_libraries(vision_service PRIVATE
    Threads::Threads
    OpenMP::OpenMP_CXX
    ${OpenCV_LIBS}
    CURL::libcurl
)

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(vision_service PRIVATE nlohmann_json::nlohmann_json)
endif()

install(TARGETS vision_service DESTINATION bin)