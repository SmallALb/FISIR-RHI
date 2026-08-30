# ══════════════════════════════════════════════════════════════
# BunnyPBR — 斯坦福兔子 PBR + 多物体碰撞样例
# 用法：BunnyPBR.exe [-C X] [-Test] [-Frames N] [-Warmup N]
#   -C X      兔子数量（可多次指定，默认 10）
#   -Test     性能测试模式，跑完 -Frames 帧后导出报告并退出
#   -Frames N 测试帧数（默认 5000）
#   -Warmup N 预热帧数（默认 60）
# 兜底：若无 Res/bunny.ply，自动用程序化球体网格顶替。
# ══════════════════════════════════════════════════════════════

add_executable(BunnyPBR
    Main.cpp
)

target_include_directories(BunnyPBR
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..   # RHI root (RHI/*.h)
        ${CMAKE_CURRENT_SOURCE_DIR}         # local: PLYLoader.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../glm  # shared glm (Example/glm)
        ${Vulkan_INCLUDE_DIRS}
        ${Vulkan_INCLUDE_DIRS}/dxc
)

target_compile_definitions(BunnyPBR
    PRIVATE
        NOMINMAX
)

target_link_libraries(BunnyPBR
    PRIVATE
        Log
        Slang::Slang
        $<$<BOOL:${BUILD_VULKAN_RHI}>:RHIVK>
)

# ── Copy runtime dependencies ─────────────────────────────────
set(VK_BIN_DIR ${Vulkan_INCLUDE_DIRS}/../Bin)

if(WIN32)
    set(DXC_LIBRARY_NAME "dxcompiler.dll")
    set(DXC_LIBRARY_PATH "${VK_BIN_DIR}/${DXC_LIBRARY_NAME}")

    set_target_properties(BunnyPBR PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
elseif(UNIX)
    set(DXC_LIBRARY_NAME "libdxcompiler.so")
    set(DXC_LIBRARY_PATH "${VK_BIN_DIR}/${DXC_LIBRARY_NAME}")
endif()

if(DXC_LIBRARY_PATH AND EXISTS "${DXC_LIBRARY_PATH}")
    add_custom_command(TARGET BunnyPBR POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DXC_LIBRARY_PATH}"
            "$<TARGET_FILE_DIR:BunnyPBR>"
        COMMENT "Copying ${DXC_LIBRARY_NAME} → BunnyPBR output"
    )
endif()

# ── Copy Slang runtime DLLs to output directory ────────────────
if(Slang_DLL_DIR AND EXISTS "${Slang_DLL_DIR}")
    add_custom_command(TARGET BunnyPBR POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Slang_DLL_DIR}/slang.dll"
            "$<TARGET_FILE_DIR:BunnyPBR>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Slang_DLL_DIR}/slang-compiler.dll"
            "$<TARGET_FILE_DIR:BunnyPBR>"
        COMMENT "Copying Slang DLLs → BunnyPBR output"
    )
endif()

# ── Copy HLSL shader sources to output directory ───────────────
add_custom_command(TARGET BunnyPBR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Shader"
        "$<TARGET_FILE_DIR:BunnyPBR>/Shader"
    COMMENT "Copying Shader/ → BunnyPBR output"
)

# ── Copy Res assets (folder + contents) to output directory ────
add_custom_command(TARGET BunnyPBR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Res"
        "$<TARGET_FILE_DIR:BunnyPBR>/Res"
    COMMENT "Copying Res/ → BunnyPBR output"
)
