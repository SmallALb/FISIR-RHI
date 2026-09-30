cmake_minimum_required(VERSION 3.21)
# 注意 LANGUAGES 里必须带 C：Android 的 native_app_glue（sources/android/native_app_glue/
# android_native_app_glue.c）是 C 源码，只开 CXX 的话 CMake 会**静默忽略**它 ——
# 表现就是 libTextureCube.so 里没有 ANativeActivity_onCreate，启动时
# "Unable to load native library ... undefined symbol: ANativeActivity_onCreate"。
project(FISIR_RHI VERSION 1.0.0 LANGUAGES C CXX)

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
    if(ANDROID)
        # Android 上没有 LunarG SDK：Vulkan 头与 libvulkan.so 都在 NDK 的 sysroot 里。
        # 这里手工造一个与 find_package 同名的 imported target，后面的 link 语句就不用分平台。
        find_library(ANDROID_VULKAN_LIBRARY vulkan REQUIRED)
        add_library(Vulkan::Vulkan INTERFACE IMPORTED)
        target_link_libraries(Vulkan::Vulkan INTERFACE ${ANDROID_VULKAN_LIBRARY})

        # ⚠ 头文件版本：NDK r27c 自带的 Vulkan 头是 VK_HEADER_VERSION 275（Vulkan 1.3 时代），
        # 里面没有 RHI 用到的 VK_EXT_descriptor_heap（PFN_vkCmdBindResourceHeapEXT、VkDeviceAddressRangeEXT…）
        # 以及 KHR 版的 present-fence 结构。本机的 Vulkan SDK 头更新（341+），优先用它；
        # 找不到时退回 NDK 自带的（那种情况下需要把 descriptor heap 相关代码编译掉，见 .claude/Results）。
        if(EXISTS "$ENV{VULKAN_SDK}/include/vulkan/vulkan.h")
            set(ANDROID_VULKAN_HEADERS "$ENV{VULKAN_SDK}/include" CACHE PATH "Android 构建用的 Vulkan 头目录")
        else()
            set(ANDROID_VULKAN_HEADERS "${CMAKE_SYSROOT}/usr/include" CACHE PATH "Android 构建用的 Vulkan 头目录")
            message(WARNING "没找到 $ENV{VULKAN_SDK}/include：Android 构建将用 NDK 自带的 Vulkan 头（可能缺 VK_EXT_descriptor_heap 类型）")
        endif()
        target_include_directories(Vulkan::Vulkan INTERFACE ${ANDROID_VULKAN_HEADERS})
        set(Vulkan_INCLUDE_DIRS ${CMAKE_SYSROOT}/usr/include)   # 兼容示例里对它的引用
        message(STATUS "Android Vulkan: ${ANDROID_VULKAN_LIBRARY}")
        message(STATUS "Android Vulkan headers: ${ANDROID_VULKAN_HEADERS}")

        # CMake 的 Android 支持只往编译/链接行加 -stdlib=libc++，**不会**自动链上 libc++_shared.so
        # （实测 CMake 4.2 + NDK r27c：链接期会留一堆 std::__ndk1::* / operator new 未定义符号），
        # 而多个 .so（Log / RHIVK / TextureCube）共用一份 STL 正是 c++_shared 的意义所在，所以显式补上。
        link_libraries(c++_shared)
    else()
        find_package(Vulkan REQUIRED)
    endif()
endif()

# Slang shader compiler SDK (required for shader compilation)
# 桌面：设 -DSLANG_SDK_DIR=<路径> 或用 vendor/slang/。
# Android：用交叉编译出来的 Android SDK，例如
#   -DSLANG_SDK_DIR=<repo>/vendor/slang-android-arm64
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

    # TextureCube 已经做了平台层移植（见 Example/TextureCube/Platform*.cpp），桌面与 Android 都能建。
    add_subdirectory(Example/TextureCube)

    if(ANDROID)
        # 其余示例还是 Win32 专用（窗口/消息循环直接写在 Main.cpp 里），
        # 等它们也接上 Platform 层再在 Android 上打开。
        message(STATUS "Android：只构建 TextureCube（其余示例尚未做平台层移植）")
    else()
        add_subdirectory(Example/Nanite)
        add_subdirectory(Example/BunnyPBR)
        add_subdirectory(Example/RemoteCube)   # headless 渲染 + 内置 HTTP/MJPEG 推流（手机浏览器观看）
    endif()
endif()

# VS startup project (Windows only)
if(WIN32 AND BUILD_EXAMPLES)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        PROPERTY VS_STARTUP_PROJECT TextureCube
    )
endif()
