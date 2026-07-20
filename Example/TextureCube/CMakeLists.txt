# ══════════════════════════════════════════════════════════════
# TextureCube — RHI usage example
# ══════════════════════════════════════════════════════════════

add_executable(TextureCube
    Main.cpp
)

target_include_directories(TextureCube
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..   # RHI root (RHI/*.h)
        ${CMAKE_CURRENT_SOURCE_DIR}         # local: glm/, stb_image.h, etc.
        ${Vulkan_INCLUDE_DIRS}
        ${Vulkan_INCLUDE_DIRS}/dxc
)

target_compile_definitions(TextureCube
    PRIVATE
        NOMINMAX
)

target_link_libraries(TextureCube
    PRIVATE
        Log
)

# ── Copy runtime dependencies ─────────────────────────────────
# dxcompiler is needed by ShaderComplier for HLSL→SPIRV
set(VK_BIN_DIR ${Vulkan_INCLUDE_DIRS}/../Bin)

if(WIN32)
    set(DXC_LIBRARY_NAME "dxcompiler.dll")
    set(DXC_LIBRARY_PATH "${VK_BIN_DIR}/${DXC_LIBRARY_NAME}")

    # VS debugger: run from output directory so DLL paths resolve
    set_target_properties(TextureCube PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
elseif(UNIX)
    set(DXC_LIBRARY_NAME "libdxcompiler.so")
    set(DXC_LIBRARY_PATH "${VK_BIN_DIR}/${DXC_LIBRARY_NAME}")
endif()

if(DXC_LIBRARY_PATH AND EXISTS "${DXC_LIBRARY_PATH}")
    add_custom_command(TARGET TextureCube POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DXC_LIBRARY_PATH}"
            "$<TARGET_FILE_DIR:TextureCube>"
        COMMENT "Copying ${DXC_LIBRARY_NAME} → TextureCube output"
    )
endif()

# ── Copy test texture to output directory ──────────────────────
add_custom_command(TARGET TextureCube POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Test3.png"
        "$<TARGET_FILE_DIR:TextureCube>"
    COMMENT "Copying Test3.png → TextureCube output"
)
