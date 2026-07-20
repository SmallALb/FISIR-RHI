# ══════════════════════════════════════════════════════════════
# Log — shared library (DLL)
# ══════════════════════════════════════════════════════════════
add_library(Log SHARED
    Logger.cpp
)

target_include_directories(Log
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(WIN32)
    target_compile_definitions(Log PRIVATE NOMINMAX)
    # MinGW adds "lib" prefix — suppress it on Windows
    set_target_properties(Log PROPERTIES PREFIX "")
endif()
