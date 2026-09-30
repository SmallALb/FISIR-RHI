# ══════════════════════════════════════════════════════════════
# Dear ImGui (vendored, v1.91.5, MIT) — https://github.com/ocornut/imgui
#
# **不建库目标**：第三方源码直接编进用到它的可执行文件（当前只有 Example/Nanite）。
# 本文件只往外暴露两个变量（PARENT_SCOPE，供其后 add_subdirectory 的示例继承）：
#     IMGUI_SOURCES       —— 核心 .cpp 列表，塞进 add_executable 即可
#     IMGUI_INCLUDE_DIR   —— imgui.h 所在目录
# 消费方式：
#     add_executable(X ... ${IMGUI_SOURCES})
#     target_include_directories(X PRIVATE ${IMGUI_INCLUDE_DIR})
#
# 只含核心库，不含任何平台/渲染后端：本工程的渲染后端是仓库根目录的 ImGui_Impl_FISIR.cpp
# （走 FISIR RHI），平台输入也在同一个文件里（Win32 段）。
# 上游 vendor/ 布局原样保留，方便整体替换升级。
# ══════════════════════════════════════════════════════════════

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    PARENT_SCOPE
)

set(IMGUI_INCLUDE_DIR ${IMGUI_DIR} PARENT_SCOPE)
