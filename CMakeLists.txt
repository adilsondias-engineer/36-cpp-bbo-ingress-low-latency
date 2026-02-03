cmake_minimum_required(VERSION 3.16)
project(ultra_low_latency_rx
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "Ultra Low Latency Network Handler for HFT"
)

# Require C++20 for best performance features
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type defaults to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")

# Find DPDK using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

if(NOT DPDK_FOUND)
    message(FATAL_ERROR "DPDK not found. Install DPDK and ensure PKG_CONFIG_PATH is set.")
endif()

message(STATUS "DPDK version: ${DPDK_VERSION}")
message(STATUS "DPDK include dirs: ${DPDK_INCLUDE_DIRS}")
message(STATUS "DPDK libraries: ${DPDK_LIBRARIES}")

# Source files
set(SOURCES
    src/main.cpp
    src/dpdk_receiver.cpp
)

# Create executable
add_executable(network_handler ${SOURCES})

# Include directories
target_include_directories(network_handler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/disruptor
    ${DPDK_INCLUDE_DIRS}
)

# Link directories for DPDK
target_link_directories(network_handler PRIVATE
    ${DPDK_LIBRARY_DIRS}
)

# Aggressive compiler optimizations for ultra low latency
target_compile_options(network_handler PRIVATE
    # Optimization level
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-DNDEBUG>

    # Target native CPU architecture
    -march=native
    -mtune=native

    # Disable exception handling (no try/catch overhead)
    -fno-exceptions

    # Disable RTTI (no dynamic_cast/typeid overhead)
    -fno-rtti

    # Fast math (allows compiler to reorder FP operations)
    -ffast-math

    # Unroll loops for better pipelining
    -funroll-loops

    # Omit frame pointer (frees up a register)
    -fomit-frame-pointer

    # Link-time optimization
    $<$<CONFIG:Release>:-flto>

    # Inline aggressively
    -finline-functions
    -finline-limit=1000

    # Prefetch hints
    -fprefetch-loop-arrays

    # Warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

# Linker optimizations
target_link_options(network_handler PRIVATE
    $<$<CONFIG:Release>:-flto>
    $<$<CONFIG:Release>:-Wl,-O2>
    -Wl,--as-needed
    -Wl,--gc-sections
)

# Link libraries
target_link_libraries(network_handler PRIVATE
    ${DPDK_LIBRARIES}
    pthread
    numa
    dl
    rt
)

# DPDK compile definitions (reduce logging overhead)
target_compile_definitions(network_handler PRIVATE
    RTE_LOG_DP_LEVEL=RTE_LOG_WARNING
    ALLOW_EXPERIMENTAL_API
)

# Profile-guided optimization support (optional)
option(ENABLE_PGO_GENERATE "Enable PGO instrumentation" OFF)
option(ENABLE_PGO_USE "Enable PGO optimization" OFF)

if(ENABLE_PGO_GENERATE)
    target_compile_options(network_handler PRIVATE -fprofile-generate)
    target_link_options(network_handler PRIVATE -fprofile-generate)
    message(STATUS "PGO: Instrumentation enabled")
endif()

if(ENABLE_PGO_USE)
    target_compile_options(network_handler PRIVATE -fprofile-use -fprofile-correction)
    target_link_options(network_handler PRIVATE -fprofile-use)
    message(STATUS "PGO: Optimization enabled")
endif()

# Install target
install(TARGETS network_handler
    RUNTIME DESTINATION bin
)

# Print configuration summary
message(STATUS "")
message(STATUS "=== Configuration Summary ===")
message(STATUS "  Project:        ${PROJECT_NAME}")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:       ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ Standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "  DPDK version:   ${DPDK_VERSION}")
message(STATUS "")
