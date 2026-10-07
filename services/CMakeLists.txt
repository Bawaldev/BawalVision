cmake_minimum_required(VERSION 3.22)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(bawal_vision_service
    VERSION 1.0.0
    DESCRIPTION "Vision processing service for MBG inspection"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        add_compile_options(-mavx2 -mfma)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        add_compile_options(-march=armv8.2-a+simd -mtune=neoverse-n1)
    endif()
endif()

find_package(Threads REQUIRED)
find_package(OpenMP REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc calib3d imgcodecs highgui dnn)
find_package(CURL REQUIRED)
find_package(nlohmann_json QUIET)

# Resolusi Header & Library ONNX Runtime C++
find_path(ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATHS
        /opt/onnxruntime/include
        /usr/local/include
        /usr/include
)
if(NOT ONNXRUNTIME_INCLUDE_DIR)
    set(ONNXRUNTIME_INCLUDE_DIR "/opt/onnxruntime/include")
endif()

find_library(ONNXRUNTIME_LIB
    NAMES onnxruntime
    PATHS
        /opt/onnxruntime/lib
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /usr/lib/aarch64-linux-gnu
)
if(NOT ONNXRUNTIME_LIB)
    set(ONNXRUNTIME_LIB "/opt/onnxruntime/lib/libonnxruntime.so")
endif()

file(GLOB_RECURSE SERVICE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_executable(vision_service ${SERVICE_SOURCES})

# Daftarkan include direktori ONNX Runtime & OpenCV ke GCC
target_include_directories(vision_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${ONNXRUNTIME_INCLUDE_DIR}
    /opt/onnxruntime/include
    /usr/local/include
)

target_link_libraries(vision_service PRIVATE
    Threads::Threads
    OpenMP::OpenMP_CXX
    ${OpenCV_LIBS}
    ${ONNXRUNTIME_LIB}
)

if(TARGET CURL::libcurl)
    target_link_libraries(vision_service PRIVATE CURL::libcurl)
else()
    target_link_libraries(vision_service PRIVATE ${CURL_LIBRARIES})
endif()

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(vision_service PRIVATE nlohmann_json::nlohmann_json)
endif()

install(TARGETS vision_service DESTINATION bin)