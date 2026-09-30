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

if(ANDROID)
    # __android_log_print 在 liblog.so 里（见 Logger.cpp 的 Android 分支）
    target_link_libraries(Log PRIVATE log)
endif()
