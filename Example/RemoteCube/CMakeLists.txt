# ────────────────────────────────────────────────────────────────────────────
# RemoteCube — 线上渲染示例（headless 渲染 + 内置 HTTP/MJPEG 推流）
# ────────────────────────────────────────────────────────────────────────────

add_executable(RemoteCube
    Main.cpp
)

target_include_directories(RemoteCube
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..   # RHI root (RHI/*.h)
        ${CMAKE_CURRENT_SOURCE_DIR}         # local: stb_image_write.h
        ${CMAKE_CURRENT_SOURCE_DIR}/..      # Example/glm（共享的 glm）
        ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(RemoteCube
    PRIVATE
        NOMINMAX
)

target_link_libraries(RemoteCube
    PRIVATE
        Log
        Slang::Headers
        ws2_32                              # Winsock2：内置 HTTP 服务
        iphlpapi                            # GetAdaptersAddresses：按网卡列局域网地址
        shell32                             # ShellExecuteEx：-OpenFirewall 弹 UAC 放行端口
        $<$<BOOL:${BUILD_VULKAN_RHI}>:RHIVK>
)

# ── 运行时依赖 ───────────────────────────────────────────────────────────────

if(WIN32)

    set_target_properties(RemoteCube PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
endif()

if(Slang_DLL_DIR AND EXISTS "${Slang_DLL_DIR}")
    add_custom_command(TARGET RemoteCube POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Slang_DLL_DIR}/slang.dll"
            "$<TARGET_FILE_DIR:RemoteCube>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Slang_DLL_DIR}/slang-compiler.dll"
            "$<TARGET_FILE_DIR:RemoteCube>"
        COMMENT "Copying Slang DLLs -> RemoteCube output"
    )
endif()
