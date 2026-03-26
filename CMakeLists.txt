cmake_minimum_required(VERSION 3.10)
project(BoWWClient VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 64-bit AArch64 Cortex-A53 Optimizations
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -mcpu=cortex-a53 -mtune=cortex-a53")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mcpu=cortex-a53 -mtune=cortex-a53")

# --- External Paths ---
set(TF_SOURCE_DIR "$ENV{HOME}/tensorflow")
set(TF_BUILD_DIR "${TF_SOURCE_DIR}/tensorflow/lite/build")
set(TF_DEPS_DIR "${TF_BUILD_DIR}/_deps")

# --- Include Directories ---
include_directories(
    include
    third_party/pffft
    ${TF_SOURCE_DIR}
    ${TF_DEPS_DIR}/flatbuffers-src/include
    ${TF_BUILD_DIR}/flatbuffers/include
    ${TF_DEPS_DIR}/abseil-cpp-src
    ${TF_BUILD_DIR}/abseil-cpp
)

# --- Source Files ---
set(SOURCES
    src/main.cpp
    src/feature_extract.cpp
    src/tflite_runner.cpp
    third_party/pffft/pffft.c
)

add_executable(BoWWClient ${SOURCES})

# --- Linking ---
find_package(ALSA REQUIRED)

# Scope up every single micro-library based exactly on your find_deps.sh output
file(GLOB_RECURSE TF_DEPS_LIBS
    "${TF_DEPS_DIR}/abseil-cpp-build/*.a"
    "${TF_DEPS_DIR}/cpuinfo-build/*.a"
    "${TF_DEPS_DIR}/farmhash-build/*.a"
    "${TF_DEPS_DIR}/fft2d-build/*.a"
    "${TF_DEPS_DIR}/flatbuffers-build/*.a"
    "${TF_DEPS_DIR}/gemmlowp-build/*.a"
    "${TF_DEPS_DIR}/ruy-build/*.a"
    "${TF_DEPS_DIR}/xnnpack-build/*.a"
    "${TF_BUILD_DIR}/pthreadpool/*.a"
)

# Link all required libraries.
# --allow-multiple-definition fixes the fft2d duplicate symbol collision
# --start-group and --end-group resolve circular dependencies among the TF micro-libraries
target_link_libraries(BoWWClient PRIVATE
    ALSA::ALSA
    -Wl,--allow-multiple-definition
    -Wl,--start-group
    ${TF_BUILD_DIR}/libtensorflow-lite.a
    ${TF_DEPS_LIBS}
    -Wl,--end-group
    pthread
    dl
    m
)
