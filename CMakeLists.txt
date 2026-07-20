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

# ── Unified output directory ──────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Dependencies ──────────────────────────────────────────────
find_package(Vulkan REQUIRED)

# ══════════════════════════════════════════════════════════════
# Sub-projects
# ══════════════════════════════════════════════════════════════
add_subdirectory(Log)
add_subdirectory(Vulkan)
add_subdirectory(Example/TextureCube)

# VS startup project (Windows only)
if(WIN32)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        PROPERTY VS_STARTUP_PROJECT TextureCube
    )
endif()
