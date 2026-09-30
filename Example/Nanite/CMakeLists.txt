# ══════════════════════════════════════════════════════════════
# Nanite — Nanite-style virtualized geometry example (WIP)
# ══════════════════════════════════════════════════════════════

add_executable(Nanite
    Main.cpp
    NaniteUI.cpp
    ClisterSelection.cpp
    # 第三方 vendored 源码直接编进本 exe（不建库）：变量由 vendor/*/CMakeLists.txt 导出
    ${IMGUI_SOURCES}          # vendor/imgui
    ${MESHOPTIMIZER_SOURCES}  # vendor/meshoptimizer（NaniteBuilder.h 的聚簇/简化）
    ${RHIIMGUI_SOURCES}       # 仓库根的 ImGui_Impl_FISIR.cpp（ImGui 的 FISIR RHI 后端）
 "ClusterSelection.h" "NaniteUI.h")

target_include_directories(Nanite
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..   # RHI root (RHI/*.h)
        ${CMAKE_CURRENT_SOURCE_DIR}         # local: NaniteMesh.h / ClusterSelection.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../glm  # shared glm (Example/glm)
        ${IMGUI_INCLUDE_DIR}                # vendor/imgui
        ${MESHOPTIMIZER_INCLUDE_DIRS}       # vendor/meshoptimizer 的 src / demo
        ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(Nanite
    PRIVATE
        NOMINMAX
)

target_link_libraries(Nanite
    PRIVATE
        Log
        Slang::Slang
        $<$<BOOL:${BUILD_VULKAN_RHI}>:RHIVK>
)

# ── 运行时依赖 ────────────────────────────────────────────────
# 着色器由 Slang 编译（见仓库根 ShaderComplier.h），运行期只需要下面拷贝的 Slang DLL。

if(WIN32)
    # VS debugger: run from output directory so DLL paths resolve
    set_target_properties(Nanite PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
endif()

# ── Copy Slang runtime DLLs to output directory ──────────────────
if(Slang_DLL_DIR AND EXISTS "${Slang_DLL_DIR}")
    add_custom_command(TARGET Nanite POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Slang_DLL_DIR}/slang.dll"
            "$<TARGET_FILE_DIR:Nanite>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Slang_DLL_DIR}/slang-compiler.dll"
            "$<TARGET_FILE_DIR:Nanite>"
        COMMENT "Copying Slang DLLs → Nanite output"
    )
endif()

# ── Copy Res assets (folder + contents) to output directory ─────
# Runtime code loads assets via relative paths like "Res/mitsuba.nanitemesh".
add_custom_command(TARGET Nanite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Res"
        "$<TARGET_FILE_DIR:Nanite>/Res"
    COMMENT "Copying Res/ → Nanite output"
)

# ── Copy Slang shader sources to output directory ─────────────────
# Runtime code loads shaders via relative paths like "Shader/ClusterSelection.slang".
add_custom_command(TARGET Nanite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Shader"
        "$<TARGET_FILE_DIR:Nanite>/Shader"
    COMMENT "Copying Shader/ → Nanite output"
)
