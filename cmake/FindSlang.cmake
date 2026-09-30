# ── FindSlang.cmake ─────────────────────────────────────────────
# Find or auto-download the Slang shader compiler SDK.
#
# Input hints (all optional):
#   SLANG_SDK_DIR        - Path to Slang SDK root (contains include/, lib/, bin/)
#   SLANG_VERSION        - Slang release version to download (default: 2026.13)
#   SLANG_DOWNLOAD_URL   - Direct URL to the Slang SDK zip (overrides auto-detection)
#
# Output:
#   Slang_FOUND          - True if Slang is available
#   Slang_INCLUDE_DIR    - Path to the directory containing slang.h
#   Slang_LIBRARY        - Path to the Slang library
#   Slang_DLL_DIR        - Platform-specific directory for Slang DLLs at runtime
#
# Imported target:
#   Slang::Slang         - Slang core library (with include path + link library)
# ─────────────────────────────────────────────────────────────────

include_guard(GLOBAL)

# ── Default version ────────────────────────────────────────────
if(NOT SLANG_VERSION)
    set(SLANG_VERSION "2026.13")
endif()

# ═══════════════════════════════════════════════════════════════
# Step 1: Try upstream CMake package config
# ═══════════════════════════════════════════════════════════════
if(NOT Slang_FOUND)
    if(SLANG_SDK_DIR AND NOT Slang_ROOT)
        set(Slang_ROOT "${SLANG_SDK_DIR}")
    endif()
    if(Slang_ROOT AND NOT SLANG_SDK_DIR)
        set(SLANG_SDK_DIR "${Slang_ROOT}")
    endif()

    find_package(slang CONFIG QUIET
        HINTS
            ${SLANG_SDK_DIR}
            ${Slang_ROOT}
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/slang
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/slang/cmake
            # 上游 SDK 的 config 装在 <SDK>/lib/cmake/slang/ 下，交叉编译时 CMake 的
            # 默认搜索路径未必会走到那里，这里显式给上。
            ${SLANG_SDK_DIR}/lib/cmake/slang
            ${SLANG_SDK_DIR}/cmake/slang
        PATH_SUFFIXES cmake
        # 交叉编译（Android/NDK）时 CMAKE_FIND_ROOT_PATH 会被设成 sysroot，HINTS 里的宿主路径
        # 会被"重新挂"到 sysroot 下从而找不到 —— 这里明确按原样搜索。
        NO_CMAKE_FIND_ROOT_PATH
    )

    if(slang_FOUND)
        if(TARGET slang::slang AND NOT TARGET Slang::Slang)
            add_library(Slang::Slang INTERFACE IMPORTED)
            target_link_libraries(Slang::Slang INTERFACE slang::slang)
            get_target_property(_slang_inc slang::slang INTERFACE_INCLUDE_DIRECTORIES)
            if(_slang_inc)
                set(Slang_INCLUDE_DIR "${_slang_inc}")
            endif()
        endif()

        if(TARGET slang::slang)
            get_target_property(_slang_loc slang::slang IMPORTED_LOCATION)
            if(_slang_loc)
                get_filename_component(Slang_DLL_DIR "${_slang_loc}" DIRECTORY)
            endif()
        endif()

        set(Slang_FOUND TRUE)
    endif()
endif()

# ═══════════════════════════════════════════════════════════════
# Step 2: Manual search in local directories
# ═══════════════════════════════════════════════════════════════
if(NOT Slang_FOUND)
    set(_slang_search_paths
        ${SLANG_SDK_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/slang
    )

    find_path(Slang_INCLUDE_DIR
        NAMES slang.h
        HINTS ${_slang_search_paths}
        PATH_SUFFIXES include
        NO_CMAKE_FIND_ROOT_PATH
    )

    find_library(Slang_LIBRARY
        NAMES slang slang-compiler
        HINTS ${_slang_search_paths}
        PATH_SUFFIXES lib
        NO_CMAKE_FIND_ROOT_PATH
    )

    if(WIN32)
        find_path(Slang_DLL_DIR
            NAMES slang.dll
            HINTS ${_slang_search_paths}
            PATH_SUFFIXES bin
        )
    endif()

    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(Slang
        REQUIRED_VARS Slang_INCLUDE_DIR Slang_LIBRARY
        HANDLE_COMPONENTS
    )
endif()

# ═══════════════════════════════════════════════════════════════
# Step 3: Auto-download from GitHub Releases if not found
# ═══════════════════════════════════════════════════════════════
if(NOT Slang_FOUND)
    # ── Resolve platform string for the release asset ──────────
    if(NOT SLANG_PLATFORM)
        if(WIN32)
            set(SLANG_PLATFORM "windows-x86_64")
        elseif(APPLE)
            set(SLANG_PLATFORM "macos-x86_64")
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(SLANG_PLATFORM "linux-x86_64")
        else()
            set(SLANG_PLATFORM "linux-x86_64")
        endif()
    endif()

    # User can override with a direct URL
    if(NOT SLANG_DOWNLOAD_URL)
        set(SLANG_DOWNLOAD_URL
            "https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}-${SLANG_PLATFORM}.zip"
        )
    endif()

    set(_slang_download_dir "${CMAKE_BINARY_DIR}/_deps/slang-sdk")
    set(_slang_zip "${_slang_download_dir}/slang-sdk.zip")

    message(STATUS "Slang SDK not found locally. Downloading ${SLANG_DOWNLOAD_URL} ...")

    # Download the zip
    file(DOWNLOAD "${SLANG_DOWNLOAD_URL}" "${_slang_zip}"
        STATUS _slang_dl_status
        TIMEOUT 300
        INACTIVITY_TIMEOUT 60
    )

    list(GET _slang_dl_status 0 _slang_dl_code)
    list(GET _slang_dl_status 1 _slang_dl_msg)

    if(NOT _slang_dl_code EQUAL 0)
        message(FATAL_ERROR
            "Failed to download Slang SDK (HTTP ${_slang_dl_code}: ${_slang_dl_msg})\n"
            "  URL: ${SLANG_DOWNLOAD_URL}\n"
            "  Manual download: https://github.com/shader-slang/slang/releases\n"
            "  Extract to vendor/slang/ and re-run CMake."
        )
    endif()

    message(STATUS "Download complete. Extracting Slang SDK ...")

    # Extract the zip
    file(ARCHIVE_EXTRACT
        INPUT "${_slang_zip}"
        DESTINATION "${_slang_download_dir}"
    )

    # Slang zips have a top-level directory like "slang-2026.13-windows-x86_64/"
    file(GLOB _slang_extracted_dirs LIST_DIRECTORIES true
        "${_slang_download_dir}/slang-*")
    list(FILTER _slang_extracted_dirs INCLUDE REGEX "/slang-[^/]*$")
    list(LENGTH _slang_extracted_dirs _slang_dir_count)

    if(_slang_dir_count EQUAL 0)
        # The zip might not have a top-level directory; use the download dir itself
        set(SLANG_SDK_DIR "${_slang_download_dir}")
    else()
        list(GET _slang_extracted_dirs 0 SLANG_SDK_DIR)
    endif()

    message(STATUS "Slang SDK extracted to: ${SLANG_SDK_DIR}")

    # ── Re-run manual search with the downloaded SDK ──────────
    find_path(Slang_INCLUDE_DIR
        NAMES slang.h
        HINTS ${SLANG_SDK_DIR}
        PATH_SUFFIXES include
        NO_DEFAULT_PATH
    )

    find_library(Slang_LIBRARY
        NAMES slang
        HINTS ${SLANG_SDK_DIR}
        PATH_SUFFIXES lib
        NO_DEFAULT_PATH
    )

    if(WIN32)
        find_path(Slang_DLL_DIR
            NAMES slang.dll
            HINTS ${SLANG_SDK_DIR}
            PATH_SUFFIXES bin
            NO_DEFAULT_PATH
        )
    endif()

    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(Slang
        REQUIRED_VARS Slang_INCLUDE_DIR Slang_LIBRARY
    )
endif()

# ═══════════════════════════════════════════════════════════════
# Resolve the runtime DLL directory
# ═══════════════════════════════════════════════════════════════
# 各示例的 POST_BUILD 步骤靠 Slang_DLL_DIR 拷贝 slang.dll / slang-compiler.dll，
# 漏掉这里就是「编译通过、启动失败（找不到 slang.dll）」。上游 config 包只设了
# 配置相关的 IMPORTED_LOCATION_RELEASE / IMPORTED_IMPLIB_RELEASE，裸的
# IMPORTED_LOCATION 查询会返回 NOTFOUND，所以这里按配置逐个查，最后回退到 SDK 的 bin/。
if(Slang_FOUND AND NOT Slang_DLL_DIR)
    if(TARGET slang::slang)
        foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL DEBUG NOCONFIG)
            foreach(_prop IMPORTED_LOCATION IMPORTED_IMPLIB)
                get_target_property(_slang_loc slang::slang "${_prop}_${_cfg}")
                if(_slang_loc AND NOT _slang_loc MATCHES "-NOTFOUND$")
                    get_filename_component(Slang_DLL_DIR "${_slang_loc}" DIRECTORY)
                    break()
                endif()
            endforeach()
            if(Slang_DLL_DIR)
                break()
            endif()
        endforeach()
    endif()

    if(NOT Slang_DLL_DIR)
        foreach(_dir "${SLANG_SDK_DIR}/bin" "${Slang_ROOT}/bin" "${CMAKE_CURRENT_SOURCE_DIR}/vendor/slang/bin")
            if(EXISTS "${_dir}/slang.dll")
                set(Slang_DLL_DIR "${_dir}")
                break()
            endif()
        endforeach()
    endif()

    if(Slang_DLL_DIR)
        message(STATUS "Slang runtime DLL dir: ${Slang_DLL_DIR}")
    else()
        message(WARNING "Slang runtime DLLs not located; examples may fail to start (slang.dll missing)")
    endif()
endif()

# ═══════════════════════════════════════════════════════════════
# Create imported target
# ═══════════════════════════════════════════════════════════════
if(Slang_FOUND AND NOT TARGET Slang::Slang)
    add_library(Slang::Slang UNKNOWN IMPORTED)
    set_target_properties(Slang::Slang PROPERTIES
        IMPORTED_LOCATION "${Slang_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Slang_INCLUDE_DIR}"
    )

    if(Slang_DLL_DIR)
        set_target_properties(Slang::Slang PROPERTIES
            SLANG_DLL_DIR "${Slang_DLL_DIR}"
        )
    endif()

    message(STATUS "Slang::Slang target created")
    message(STATUS "  Include:  ${Slang_INCLUDE_DIR}")
    message(STATUS "  Library:  ${Slang_LIBRARY}")
    if(Slang_DLL_DIR)
        message(STATUS "  DLL dir:  ${Slang_DLL_DIR}")
    endif()
endif()

mark_as_advanced(
    Slang_INCLUDE_DIR
    Slang_LIBRARY
    Slang_DLL_DIR
)
