# ── Vulkan RHI DLL sources ───────────────────────────────────
file(GLOB VK_SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB VK_HEADERS  "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

file(GLOB RHI_ROOT_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/../*.h")
list(FILTER RHI_ROOT_HEADERS EXCLUDE REGEX ".*/Log/.*")
list(FILTER RHI_ROOT_HEADERS EXCLUDE REGEX ".*/Example/.*")

# ── RHIVK shared library ─────────────────────────────────────
add_library(RHIVK SHARED
    ${VK_SOURCES} ${VK_HEADERS} ${RHI_ROOT_HEADERS}
)

target_include_directories(RHIVK
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(RHIVK
    PUBLIC  Vulkan::Vulkan
    PRIVATE Log
    PRIVATE Slang::Slang
)

# ── Output to RHI/Vulkan/ subfolder ──────────────────────────
# multiple backends (Dx12, Metal...) will each have their own subfolder.
# RHICreator 按相对路径 "RHI/Vulkan/RHIVK.dll" 从 exe 所在目录加载，故 DLL 需
# 落在 exe 同目录下的 RHI/Vulkan/。这里不能写 $<CONFIG>：exe 由根 CMakeLists 的
# CMAKE_RUNTIME_OUTPUT_DIRECTORY（bin/）决定，多配置生成器（VS）会为其自动追加
# 配置子目录（bin/<Config>/），单配置生成器（Ninja）则不会（bin/）。若此处显式
# 写 $<CONFIG>，Ninja 单配置下 DLL 会落到 bin/<Config>/RHI/Vulkan/，与 exe 的
# bin/ 错位，导致运行时找不到 RHIVK.dll。省略 $<CONFIG> 后两种生成器均与 exe 对齐。
set_target_properties(RHIVK PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/RHI/Vulkan
)

# 上面的对齐只对单配置生成器（Ninja/Makefile）成立：多配置生成器（VS）会再给
# RUNTIME_OUTPUT_DIRECTORY 追加配置子目录，于是 DLL 落到 bin/RHI/Vulkan/<Config>/，
# 而 exe 在 bin/<Config>/ 下、按 "RHI/Vulkan/RHIVK.dll" 找 —— 找不到就会静默加载
# 到上一次手工拷贝的旧 DLL（结构体一旦变过就是未定义行为/崩溃）。这里补一条拷贝，
# 把它放到 exe 真正会去读的位置。单配置生成器下 CMAKE_CONFIGURATION_TYPES 为空，跳过。
if(CMAKE_CONFIGURATION_TYPES)
    add_custom_command(TARGET RHIVK POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:RHIVK>"
            "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/RHI/Vulkan/RHIVK.dll"
        COMMENT "Copying RHIVK.dll -> exe 目录 (bin/$<CONFIG>/RHI/Vulkan)"
    )
endif()

# ── Platform definitions ─────────────────────────────────────
if(WIN32)
    target_compile_definitions(RHIVK PRIVATE
        VK_USE_PLATFORM_WIN32_KHR
        NOMINMAX
    )
    # MinGW adds "lib" prefix — suppress it on Windows
    set_target_properties(RHIVK PROPERTIES PREFIX "")
endif()
