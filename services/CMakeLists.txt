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

if(CMAKE_CXX_COMPILER_ID MATCHES "IntelLLVM")
    message(STATUS "Detected Intel oneAPI DPC++/C++ Compiler (icpx) - Activating Xeon silicon optimizations")
    add_compile_options(-O3 -mavx2 -mfma -fiopenmp -qopt-zmm-usage=low)
    add_compile_definitions(BAWAL_USE_INTEL_ICPX=1)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3)
    
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        add_compile_options(-mavx2 -mfma)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        add_compile_options(-march=armv8.2-a+simd -mtune=neoverse-n1)
    endif()
endif()

# Optional Intel oneDNN (DNNL) execution provider support for Intel Xeon acceleration
option(ENABLE_ONEDNN "Enable oneDNN (DNNL) execution provider in ONNX Runtime" OFF)
if(ENABLE_ONEDNN OR CMAKE_CXX_COMPILER_ID MATCHES "IntelLLVM")
    find_path(DNNL_INCLUDE_DIR
        NAMES dnnl.h dnnl.hpp dnnl_provider_factory.h
        PATHS
            /opt/intel/oneapi/dnnl/latest/include
            /opt/onnxruntime/include
            /usr/local/include
            /usr/include
    )
    if(DNNL_INCLUDE_DIR)
        message(STATUS "Found oneDNN (DNNL) at: ${DNNL_INCLUDE_DIR}")
        add_compile_definitions(USE_DNNL=1 HAVE_DNNL=1)
        include_directories(${DNNL_INCLUDE_DIR})
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")

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
<<<<<<< HEAD
find_package(hwy QUIET)
if(NOT TARGET hwy::hwy)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(HWY QUIET IMPORTED_TARGET libhwy)
    endif()
    if(NOT TARGET PkgConfig::HWY)
        find_path(HWY_INCLUDE_DIR
            NAMES hwy/highway.h
            PATHS
                /usr/local/include
                /usr/include
        )
        find_library(HWY_LIB
            NAMES hwy
            PATHS
                /usr/local/lib
                /usr/lib
                /usr/lib64
                /usr/lib/x86_64-linux-gnu
                /usr/lib/aarch64-linux-gnu
        )
    endif()
endif()
find_package(fmt QUIET)
find_package(httplib QUIET)
find_package(nlohmann_json QUIET)
=======
find_package(PostgreSQL QUIET)
find_package(fmt CONFIG QUIET)
find_package(httplib QUIET)
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
find_package(ncnn QUIET)

include(FetchContent)

if(NOT TARGET fmt::fmt)
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 10.2.1
        GIT_SHALLOW TRUE
    )
    set(FMT_DOC OFF CACHE BOOL "" FORCE)
    set(FMT_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(fmt)
endif()

if(NOT TARGET glaze::glaze)
    FetchContent_Declare(glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v4.2.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glaze)
endif()

find_package(mimalloc CONFIG QUIET)
if(NOT TARGET mimalloc-static AND NOT TARGET mimalloc)
    FetchContent_Declare(mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG v2.1.7
        GIT_SHALLOW TRUE
    )
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(mimalloc)
endif()

option(ENABLE_AWS_LAMBDA "Build vision_service as an AWS Lambda custom runtime" OFF)
if(ENABLE_AWS_LAMBDA)
    find_package(aws-lambda-runtime CONFIG REQUIRED)
endif()

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

# The upstream generator uses std::format, which requires GCC 13+ / Clang 17+
# with a matching libstdc++. Debian bookworm ships GCC 12, so container builds
# substitute a no-op stub and rely on pre-converted NCNN weights instead.
option(BAWAL_ENABLE_NCNN_GENERATOR
    "Compile the std::format-based NCNN generator (requires GCC 13+)" ON)

# ---------------------------------------------------------------------------
# Third-party web stack (Crow + standalone Asio)
# ---------------------------------------------------------------------------
# Both are vendored so the Docker image can build without network access.
set(BAWAL_THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(bawal_crow INTERFACE)
target_include_directories(bawal_crow SYSTEM INTERFACE
    ${BAWAL_THIRD_PARTY_DIR}/crow
    ${BAWAL_THIRD_PARTY_DIR}/asio/include
)
target_compile_definitions(bawal_crow INTERFACE ASIO_STANDALONE)
target_link_libraries(bawal_crow INTERFACE Threads::Threads)

# ---------------------------------------------------------------------------
# Shared core: vision pipeline + IPC + Crow routes
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SERVICE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER SERVICE_SOURCES EXCLUDE REGEX "/src/main\\.cpp$")
list(FILTER SERVICE_SOURCES EXCLUDE REGEX "/src/backend/main_backend\\.cpp$")
if(BAWAL_ENABLE_NCNN_GENERATOR AND EXISTS ${ML_GENERATOR_SRC})
    list(APPEND SERVICE_SOURCES ${ML_GENERATOR_SRC})
else()
    list(APPEND SERVICE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vision/ncnn_generator_stub.cpp)
endif()

add_library(bawal_core STATIC ${SERVICE_SOURCES})

target_include_directories(bawal_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ML_INCLUDE_DIR}
    ${OpenCV_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${ONNXRUNTIME_INCLUDE_DIR}
    /opt/onnxruntime/include
    /usr/local/include
)

# Add NCNN include directory if not using imported target
if(NCNN_INCLUDE_DIR)
    target_include_directories(bawal_core PRIVATE ${NCNN_INCLUDE_DIR})
endif()

target_link_libraries(bawal_core PUBLIC
    Threads::Threads
    OpenMP::OpenMP_CXX
    ${OpenCV_LIBS}
    ${ONNXRUNTIME_LIB}
    bawal_crow
)

# Link NCNN library if available
if(TARGET ncnn)
    target_link_libraries(bawal_core PUBLIC ncnn)
    target_compile_definitions(bawal_core PUBLIC HAVE_NCNN=1 BAWAL_HAVE_NCNN=1)
elseif(NCNN_LIB)
    target_link_libraries(bawal_core PUBLIC ${NCNN_LIB})
    target_compile_definitions(bawal_core PUBLIC HAVE_NCNN=1 BAWAL_HAVE_NCNN=1)
endif()

# Link Vulkan if NCNN GPU acceleration is enabled
if(Vulkan_FOUND AND (TARGET ncnn OR NCNN_LIB))
    target_link_libraries(bawal_core PUBLIC Vulkan::Vulkan)
    target_compile_definitions(bawal_core PUBLIC USE_NCNN_VULKAN=1)
endif()

if(TARGET CURL::libcurl)
    target_link_libraries(bawal_core PUBLIC CURL::libcurl)
else()
    target_link_libraries(bawal_core PUBLIC ${CURL_LIBRARIES})
endif()

if(PostgreSQL_FOUND)
    target_link_libraries(bawal_core PUBLIC PostgreSQL::PostgreSQL)
    target_compile_definitions(bawal_core PUBLIC BAWAL_HAVE_POSTGRES=1)
    message(STATUS "PostgreSQL/libpq found - inspection persistence enabled")
else()
    message(STATUS "PostgreSQL/libpq not found - inspection persistence disabled")
endif()

<<<<<<< HEAD
if(TARGET fmt::fmt)
    target_link_libraries(vision_service PRIVATE fmt::fmt)
    target_compile_definitions(vision_service PRIVATE BAWAL_USE_SYSTEM_FMT=1)
endif()

if(TARGET httplib::httplib)
    target_link_libraries(vision_service PRIVATE httplib::httplib)
    target_compile_definitions(vision_service PRIVATE BAWAL_USE_SYSTEM_HTTPLIB=1)
endif()

if (TARGET hwy::hwy)
    target_link_libraries(vision_service PRIVATE hwy::hwy)
    target_compile_definitions(vision_service PRIVATE BAWAL_HAVE_HWY=1)
elseif (TARGET PkgConfig::HWY)
    target_link_libraries(vision_service PRIVATE PkgConfig::HWY)
    target_compile_definitions(vision_service PRIVATE BAWAL_HAVE_HWY=1)
elseif (HWY_INCLUDE_DIR AND HWY_LIB)
    target_include_directories(vision_service PRIVATE ${HWY_INCLUDE_DIR})
    target_link_libraries(vision_service PRIVATE ${HWY_LIB})
    target_compile_definitions(vision_service PRIVATE BAWAL_HAVE_HWY=1)
else()
    message(STATUS "Google Highway (hwy) not found via CMake/PkgConfig; using built-in AVX2 / scalar dark detector fallback.")
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "IntelLLVM")
    target_compile_definitions(vision_service PRIVATE BAWAL_USE_INTEL_ICPX=1)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(vision_service PRIVATE BAWAL_RELEASE_BUILD=1)
endif()


install(TARGETS vision_service DESTINATION bin)
=======
target_link_libraries(bawal_core PUBLIC fmt::fmt glaze::glaze)
target_compile_definitions(bawal_core PUBLIC BAWAL_USE_FMT=1 BAWAL_USE_GLAZE=1)

if(TARGET mimalloc-static)
    target_link_libraries(bawal_core PUBLIC mimalloc-static)
    target_compile_definitions(bawal_core PUBLIC BAWAL_USE_MIMALLOC=1)
elseif(TARGET mimalloc)
    target_link_libraries(bawal_core PUBLIC mimalloc)
    target_compile_definitions(bawal_core PUBLIC BAWAL_USE_MIMALLOC=1)
else()
    message(FATAL_ERROR "mimalloc package did not provide mimalloc-static or mimalloc target")
endif()

if(TARGET httplib::httplib)
    target_link_libraries(bawal_core PUBLIC httplib::httplib)
    target_compile_definitions(bawal_core PUBLIC BAWAL_USE_SYSTEM_HTTPLIB=1)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "IntelLLVM")
    target_compile_definitions(bawal_core PUBLIC BAWAL_USE_INTEL_ICPX=1)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(bawal_core PUBLIC BAWAL_RELEASE_BUILD=1)
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
option(BAWAL_BUILD_BACKEND "Build the unified Crow C++ web service" ON)
option(BAWAL_BUILD_VISION_VIEWER "Build the legacy standalone vision viewer" ON)

# Unified C++ web service: Crow HTTP + in-process SHM vision consumer.
if(BAWAL_BUILD_BACKEND)
    add_executable(bawal_backend src/backend/main_backend.cpp)
    target_link_libraries(bawal_backend PRIVATE bawal_core OpenMP::OpenMP_CXX)
    target_compile_definitions(bawal_backend PRIVATE BAWAL_USE_MIMALLOC=1)
    install(TARGETS bawal_backend DESTINATION bin)
endif()

# Legacy standalone vision viewer kept for the existing `just run-vision`
# workflow while the Crow backend becomes the primary deployment target.
if(BAWAL_BUILD_VISION_VIEWER)
    add_executable(vision_service src/main.cpp)
    target_link_libraries(vision_service PRIVATE bawal_core)
    target_compile_definitions(vision_service PRIVATE BAWAL_USE_MIMALLOC=1)
    if(ENABLE_AWS_LAMBDA)
        target_compile_definitions(vision_service PRIVATE BUILD_AWS_LAMBDA=1)
        target_link_libraries(vision_service PRIVATE AWS::aws-lambda-runtime)
        set_target_properties(vision_service PROPERTIES OUTPUT_NAME "bootstrap")
    endif()
    install(TARGETS vision_service DESTINATION bin)
endif()
>>>>>>> 1d20cdc2a15f3f4b16c7f19615855235ce9d8444
