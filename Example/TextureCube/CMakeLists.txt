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
)

target_compile_definitions(TextureCube
    PRIVATE
        NOMINMAX
)

target_link_libraries(TextureCube
    PRIVATE
        Log
        Slang::Slang
        $<$<BOOL:${BUILD_VULKAN_RHI}>:RHIVK>
)

# ── 运行时依赖 ────────────────────────────────────────────────
# 着色器由 Slang 编译（见仓库根 ShaderComplier.h），运行期只需要下面拷贝的 Slang DLL。

if(WIN32)

    # VS debugger: run from output directory so DLL paths resolve
    set_target_properties(TextureCube PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
endif()

# ── Copy Slang runtime DLLs to output directory ──────────────────
if(Slang_DLL_DIR AND EXISTS "${Slang_DLL_DIR}")
    add_custom_command(TARGET TextureCube POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Slang_DLL_DIR}/slang.dll"
            "$<TARGET_FILE_DIR:TextureCube>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Slang_DLL_DIR}/slang-compiler.dll"
            "$<TARGET_FILE_DIR:TextureCube>"
        COMMENT "Copying Slang DLLs → TextureCube output"
    )
endif()

# ── Copy test texture to output directory ──────────────────────
add_custom_command(TARGET TextureCube POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Test3.png"
        "$<TARGET_FILE_DIR:TextureCube>"
    COMMENT "Copying Test3.png → TextureCube output"
)
