cmake_minimum_required(VERSION 3.21)
project(FISIR_RHI VERSION 1.0.0 LANGUAGES CXX)

# ── C++ Standard ──────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Windows-specific defaults ─────────────────────────────────
if(WIN32)
    # Force .sln (not .slnx) for better VS compatibility
    set(CMAKE_VS_SOLUTION_EXTENSION ".sln")
endif()

# ── MSVC: force UTF-8 source encoding ─────────────────────────
# Without this, MSVC treats UTF-8 files as the system code page
# (e.g. 936/GBK), corrupting non-ASCII characters in comments/strings.
if(MSVC)
    add_compile_options("/utf-8")
endif()

# ── Unified output directory ──────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Build options (visible in CMake GUI) ──────────────────────
option(BUILD_VULKAN_RHI "Build Vulkan RHI backend" ON)
option(BUILD_EXAMPLES   "Build example projects"    ON)

# ── CMake module path ───────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ── Dependencies ──────────────────────────────────────────────
if(BUILD_VULKAN_RHI)
    find_package(Vulkan REQUIRED)
endif()

# Slang shader compiler SDK (required for shader compilation)
# Set -DSLANG_SDK_DIR=<path> or place under vendor/slang/
find_package(Slang REQUIRED)

# ══════════════════════════════════════════════════════════════
# Sub-projects
# ══════════════════════════════════════════════════════════════
add_subdirectory(Log)

if(BUILD_VULKAN_RHI)
    add_subdirectory(Vulkan)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(Example/TextureCube)
endif()

# VS startup project (Windows only)
if(WIN32 AND BUILD_EXAMPLES)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        PROPERTY VS_STARTUP_PROJECT TextureCube
    )
endif()
