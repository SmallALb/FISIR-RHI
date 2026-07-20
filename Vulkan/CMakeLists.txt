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
)

# ── Output to RHI/Vulkan/ subfolder ──────────────────────────
# multiple backends (Dx12, Metal...) will each have their own subfolder
set_target_properties(RHIVK PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/RHI/Vulkan
)

# ── Platform definitions ─────────────────────────────────────
if(WIN32)
    target_compile_definitions(RHIVK PRIVATE
        VK_USE_PLATFORM_WIN32_KHR
        NOMINMAX
    )
    # MinGW adds "lib" prefix — suppress it on Windows
    set_target_properties(RHIVK PROPERTIES PREFIX "")
endif()
