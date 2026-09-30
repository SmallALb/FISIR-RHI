# ══════════════════════════════════════════════════════════════
# TextureCube — RHI usage example
#   桌面   ：Win32 可执行程序（Main.cpp + PlatformWin32.cpp）
#   Android：NativeActivity 共享库 libTextureCube.so（Main.cpp + PlatformAndroid.cpp
#            + NDK 的 native_app_glue），由 tools/package_android_apk.ps1 打进 APK
# ══════════════════════════════════════════════════════════════

if(ANDROID)
    # native_app_glue 随 NDK 提供：它实现 ANativeActivity_onCreate → android_main 的桥接，
    # 并管理 APP_CMD_* 生命周期（见 PlatformAndroid.cpp）。
    set(GLUE_DIR "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")

    add_library(TextureCube SHARED
        Main.cpp
        PlatformAndroid.cpp
        ${IMGUI_SOURCES}          # vendor/imgui（核心，无平台后端）
        ${RHIIMGUI_SOURCES}       # 仓库根的 ImGui_Impl_FISIR.cpp（ImGui 的 FISIR RHI 后端）
        "${GLUE_DIR}/android_native_app_glue.c"
    )

    target_include_directories(TextureCube
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../..   # RHI root (RHI/*.h)
            ${CMAKE_CURRENT_SOURCE_DIR}         # local: glm/, stb_image.h, Platform.h
            ${IMGUI_INCLUDE_DIR}                # vendor/imgui（imgui.h）
            ${GLUE_DIR}
            ${Vulkan_INCLUDE_DIRS}
    )

    target_compile_definitions(TextureCube PRIVATE NOMINMAX)

    # android / log：AAssetManager、ALooper、ANativeWindow 与 __android_log_print 都在这两个库里
    target_link_libraries(TextureCube
        PRIVATE
            Log
            Slang::Headers
            android
            log
            $<$<BOOL:${BUILD_VULKAN_RHI}>:RHIVK>
    )

    message(STATUS "TextureCube(Android): 产物 = $<TARGET_FILE:TextureCube>（APK 打包脚本会收集依赖的 .so）")
else()
    add_executable(TextureCube
        Main.cpp
        PlatformWin32.cpp
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
            Slang::Headers
            $<$<BOOL:${BUILD_VULKAN_RHI}>:RHIVK>
    )

    # ── 运行时依赖 ────────────────────────────────────────────────
    # 着色器由 Slang 编译（见仓库根 ShaderComplier.h），运行期只需要下面拷贝的 Slang DLL。

    # VS debugger: run from output directory so DLL paths resolve
    set_target_properties(TextureCube PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )

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
endif()
