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

# 第三方 vendored 库（meshoptimizer / imgui）**一律不建库目标**：它们各自的 CMakeLists.txt
# 只往父作用域导出「源码列表 + include 路径」，由用到它们的 exe 直接编译（见各自的文件头注释）。
add_subdirectory(vendor/meshoptimizer)

if(BUILD_VULKAN_RHI)
    add_subdirectory(Vulkan)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(vendor/imgui)
    # ImGui 的 FISIR RHI 后端（渲染 + Win32 输入）：同样不建库，示例把它编进自己的 exe。
    set(RHIIMGUI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/ImGui_Impl_FISIR.cpp)

    add_subdirectory(Example/TextureCube)
    add_subdirectory(Example/Nanite)
    add_subdirectory(Example/BunnyPBR)
    add_subdirectory(Example/RemoteCube)   # headless 渲染 + 内置 HTTP/MJPEG 推流（手机浏览器观看）
endif()

# VS startup project (Windows only)
if(WIN32 AND BUILD_EXAMPLES)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        PROPERTY VS_STARTUP_PROJECT TextureCube
    )
endif()
