cmake_minimum_required(VERSION 3.22)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(bawal_vision_service
    VERSION 1.0.0
    DESCRIPTION "Vision processing service for MBG inspection with Real-ESRGAN NCNN"
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

    # ============================================================
    # WARNING: -ffast-math
    # ------------------------------------------------------------
    # This flag can BREAK the following:
    #   - std::isnan / std::isinf / std::isfinite (may always return false)
    #   - Softmax / normalization / log-sum-exp (reorders operations)
    #   - Division by small epsilon (may be treated as zero)
    #   - ML model accuracy (ONNX / NCNN inference)
    #
    # ONLY apply to files with simple image arithmetic.
    # DO NOT apply to ML inference, pipeline, or control logic files.
    # ============================================================

    # Files SAFE for -ffast-math (simple image arithmetic only)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vision/enchanter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vision/simd_food_evaluator.cpp
        PROPERTIES COMPILE_OPTIONS "-ffast-math"
    )

    # Files requiring HIGH PRECISION (ML inference, pipeline control)
    # Explicitly disable fast-math to prevent override from global flags
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vision/classifier.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vision/pipeline.cpp
        PROPERTIES COMPILE_OPTIONS "-fno-fast-math"
    )
endif()

# Core dependencies
find_package(Threads REQUIRED)
find_package(OpenMP REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc calib3d imgcodecs highgui dnn)
find_package(CURL REQUIRED)
find_package(nlohmann_json QUIET)
find_package(ncnn QUIET)

if(NOT TARGET ncnn)
    find_path(NCNN_INCLUDE_DIR
        NAMES net.h gpu.h
        PATHS
            /opt/ncnn/include/ncnn
            /usr/local/include/ncnn
            /usr/include/ncnn
    )

    find_library(NCNN_LIB
        NAMES ncnn
        PATHS
            /opt/ncnn/lib
            /usr/local/lib
            /usr/lib
            /usr/lib/x86_64-linux-gnu
            /usr/lib/aarch64-linux-gnu
    )

    if(NOT NCNN_INCLUDE_DIR OR NOT NCNN_LIB)
        message(STATUS "NCNN library not found. Operating with native multi-architecture AVX-512 / AVX2 / OpenMP SIMD super-resolution engine.")
    endif()
endif()

find_package(Vulkan QUIET)

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

set(ML_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ml/include)
set(ML_GENERATOR_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../ml/src/ncnn_generator.cpp)

file(GLOB_RECURSE SERVICE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
if(EXISTS ${ML_GENERATOR_SRC})
    list(APPEND SERVICE_SOURCES ${ML_GENERATOR_SRC})
endif()

add_executable(vision_service ${SERVICE_SOURCES})

target_include_directories(vision_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ML_INCLUDE_DIR}
    ${OpenCV_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${ONNXRUNTIME_INCLUDE_DIR}
    /opt/onnxruntime/include
    /usr/local/include
)

# Add NCNN include directory if not using imported target
if(NCNN_INCLUDE_DIR)
    target_include_directories(vision_service PRIVATE ${NCNN_INCLUDE_DIR})
endif()

target_link_libraries(vision_service PRIVATE
    Threads::Threads
    OpenMP::OpenMP_CXX
    ${OpenCV_LIBS}
    ${ONNXRUNTIME_LIB}
)

# Link NCNN library if available
if(TARGET ncnn)
    target_link_libraries(vision_service PRIVATE ncnn)
    target_compile_definitions(vision_service PRIVATE HAVE_NCNN=1 BAWAL_HAVE_NCNN=1)
elseif(NCNN_LIB)
    target_link_libraries(vision_service PRIVATE ${NCNN_LIB})
    target_compile_definitions(vision_service PRIVATE HAVE_NCNN=1 BAWAL_HAVE_NCNN=1)
endif()

# Link Vulkan if NCNN GPU acceleration is enabled
if(Vulkan_FOUND AND (TARGET ncnn OR NCNN_LIB))
    target_link_libraries(vision_service PRIVATE Vulkan::Vulkan)
    target_compile_definitions(vision_service PRIVATE USE_NCNN_VULKAN=1)
endif()

if(TARGET CURL::libcurl)
    target_link_libraries(vision_service PRIVATE CURL::libcurl)
else()
    target_link_libraries(vision_service PRIVATE ${CURL_LIBRARIES})
endif()

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(vision_service PRIVATE nlohmann_json::nlohmann_json)
endif()

install(TARGETS vision_service DESTINATION bin)