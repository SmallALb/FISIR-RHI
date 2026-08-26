# ══════════════════════════════════════════════════════════════
# Nanite — Nanite-style virtualized geometry example (WIP)
# ══════════════════════════════════════════════════════════════

add_executable(Nanite
    Main.cpp
 "ClusterSelection.h" "ClisterSelection.cpp")

target_include_directories(Nanite
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..   # RHI root (RHI/*.h)
        ${Vulkan_INCLUDE_DIRS}
        ${Vulkan_INCLUDE_DIRS}/dxc
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

# ── Copy runtime dependencies ─────────────────────────────────
# dxcompiler is needed by ShaderComplier for HLSL→SPIRV
set(VK_BIN_DIR ${Vulkan_INCLUDE_DIRS}/../Bin)

if(WIN32)
    set(DXC_LIBRARY_NAME "dxcompiler.dll")
    set(DXC_LIBRARY_PATH "${VK_BIN_DIR}/${DXC_LIBRARY_NAME}")

    # VS debugger: run from output directory so DLL paths resolve
    set_target_properties(Nanite PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
elseif(UNIX)
    set(DXC_LIBRARY_NAME "libdxcompiler.so")
    set(DXC_LIBRARY_PATH "${VK_BIN_DIR}/${DXC_LIBRARY_NAME}")
endif()

if(DXC_LIBRARY_PATH AND EXISTS "${DXC_LIBRARY_PATH}")
    add_custom_command(TARGET Nanite POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DXC_LIBRARY_PATH}"
            "$<TARGET_FILE_DIR:Nanite>"
        COMMENT "Copying ${DXC_LIBRARY_NAME} → Nanite output"
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

# ── Copy HLSL shader sources to output directory ─────────────────
# Runtime code loads shaders via relative paths like "Shader/ClusterSelection.hlsl".
add_custom_command(TARGET Nanite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Shader"
        "$<TARGET_FILE_DIR:Nanite>/Shader"
    COMMENT "Copying Shader/ → Nanite output"
)
