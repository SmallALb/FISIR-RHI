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
endif()
