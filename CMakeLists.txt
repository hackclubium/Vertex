cmake_minimum_required(VERSION 3.20)
project(Vertex VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Derive version from the latest git tag (e.g. "v1.3.2" -> "1.3.2").
# Falls back to "0.0.0" if not in a git repo or no tags exist.
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_TAG_RESULT
)
if (GIT_TAG_RESULT EQUAL 0 AND GIT_TAG MATCHES "^v?(.+)$")
    string(REGEX REPLACE "^v" "" VERTEX_VERSION "${GIT_TAG}")
else ()
    set(VERTEX_VERSION "0.0.0")
endif ()
message(STATUS "Vertex version: ${VERTEX_VERSION}")
add_compile_definitions(VERTEX_VERSION="${VERTEX_VERSION}")

# ── mbedTLS (Linux-only TLS backend) ──────────────────────────────────────────
# Windows and macOS get real OS-native TLS (SChannel / Secure Transport —
# see src/network/tls_windows.cpp / tls_macos.cpp), zero third-party
# dependency. Linux has no OS-provided TLS stack at all, so it gets one
# small, bundled library instead (see src/network/tls_linux.cpp) — a
# deliberate, scoped exception, not a full third-party HTTP/TLS engine.
if (UNIX AND NOT APPLE)
    include(FetchContent)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    # A plain release tarball is missing the "framework" git submodule
    # mbedTLS 3.6.x's build needs — clone via git instead, which pulls
    # submodules along with it.
    FetchContent_Declare(mbedtls
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG v3.6.2
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(mbedtls)
endif ()

# Use /EHa everywhere so catch(...) also catches structured exceptions
# (access violations etc.) — a faulting page must never kill the browser.
# Strip the default /EHsc first so it doesn't override /EHa on any target.
if (MSVC)
    string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/EHs" ""  CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    add_compile_options(/EHa)
    # Read source files as UTF-8 so literal chars (×, •, …) in L"" literals aren't
    # mangled to mojibake (e.g. "Ã—") under the system codepage.
    add_compile_options(/utf-8)
    # Serialize PDB writes so parallel MSVC builds don't race on vc*.pdb.
    add_compile_options(/FS)
endif()

add_library(vertex_engine STATIC
    src/codec/inflate.cpp
    src/codec/crc32.cpp
    src/codec/png.cpp
    src/codec/jpeg.cpp
    src/codec/webp.cpp
    src/render/rasterizer.cpp
    src/font/truetype.cpp
    src/font/font_face.cpp
    src/network/fetcher.cpp
    src/network/resource_cache.cpp
    src/network/text_decode.cpp
    src/network/url.cpp
    src/network/websocket.cpp
    src/network/socket.cpp
    src/network/arti_client.cpp
    src/network/http_client.cpp
    src/network/tls_windows.cpp
    src/network/tls_linux.cpp
    src/network/tls_macos.cpp
    src/platform/downloads.cpp
    src/platform/profile.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
    src/html/resources.cpp
    src/html/serializer.cpp
    src/css/stylesheet.cpp
    src/layout/layout.cpp
    src/layout/layout_engine.cpp
    src/paint/display_list.cpp
    src/js/gc.cpp
    src/js/lexer.cpp
    src/js/parser.cpp
    src/js/compiler.cpp
    src/js/vm.cpp
    src/js/runtime.cpp
    src/js/dom_bridge.cpp
    src/js/canvas_bridge.cpp
    src/js/engine.cpp
)

target_include_directories(vertex_engine PUBLIC src)
if (WIN32)
    target_compile_definitions(vertex_engine PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if (MSVC)
    # /EHa: catch SEH exceptions (stack overflow, access violation) in catch(...)
    target_compile_options(vertex_engine PUBLIC /EHa)
endif()

# TLS backend per platform — see src/network/tls_socket.h.
if (WIN32)
    target_link_libraries(vertex_engine PUBLIC secur32 crypt32 ws2_32)
elseif (APPLE)
    target_link_libraries(vertex_engine PUBLIC "-framework Security" "-framework CoreFoundation")
elseif (UNIX)
    target_link_libraries(vertex_engine PUBLIC mbedtls mbedx509 mbedcrypto)
    target_link_libraries(vertex_engine PUBLIC ${CMAKE_DL_LIBS})
endif()

option(VERTEX_BUILD_ARTI "Build embedded Arti bridge (downloads Rust crates)" OFF)
find_program(CARGO_EXECUTABLE cargo)
if (VERTEX_BUILD_ARTI AND CARGO_EXECUTABLE)
    if (WIN32)
        set(VERTEX_ARTI_LIB_NAME vertex_arti.dll)
    elseif (APPLE)
        set(VERTEX_ARTI_LIB_NAME libvertex_arti.dylib)
    else()
        set(VERTEX_ARTI_LIB_NAME libvertex_arti.so)
    endif()
    add_custom_target(vertex_arti
        COMMAND ${CARGO_EXECUTABLE} build --release
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/vertex_arti/target/release/${VERTEX_ARTI_LIB_NAME}
            ${CMAKE_BINARY_DIR}/$<CONFIG>/${VERTEX_ARTI_LIB_NAME}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/vertex_arti
        COMMENT "Building embedded Arti bridge"
        VERBATIM
    )
elseif (VERTEX_BUILD_ARTI)
    message(STATUS "VERTEX_BUILD_ARTI=ON but cargo not found — embedded Arti bridge disabled")
else()
    message(STATUS "embedded Arti bridge disabled (set VERTEX_BUILD_ARTI=ON to build it); .onion falls back to VERTEX_TOR_SOCKS")
endif()

# ── platform backends ─────────────────────────────────────────────────────────
# Each OS gets its own renderer + windowing backend. The engine library is
# platform-independent; only these files differ per OS.

if (WIN32)
    add_executable(Vertex WIN32
        src/main.cpp
        src/platform/vertex.rc
        src/render/renderer.cpp
        src/render/box_paint.cpp
        src/render/canvas_renderer.cpp
        src/platform/media_win32.cpp
        src/platform/platform_win32.cpp
    )
    target_include_directories(Vertex PRIVATE src)
    target_link_libraries(Vertex PRIVATE
        vertex_engine d2d1 dwrite comctl32 ole32 strmiids
    )
    if (MSVC)
        target_link_options(Vertex PRIVATE /STACK:67108864)
    endif()
elseif (APPLE)
    set_source_files_properties(src/platform/vertex_icon.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    set_source_files_properties(src/platform/vertex_icon_light.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    add_executable(Vertex MACOSX_BUNDLE
        src/platform/main_macos.mm
        src/platform/platform_macos.mm
        src/platform/media_macos.mm
        src/render/canvas_coregraphics.cpp
        src/platform/vertex_icon.icns
        src/platform/vertex_icon_light.icns
    )
    target_include_directories(Vertex PRIVATE src)
    target_link_libraries(Vertex PRIVATE
        vertex_engine
        "-framework Cocoa"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework QuartzCore"
        "-framework CoreGraphics"
        "-framework CoreText"
    )
    set_target_properties(Vertex PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.vertex.browser"
        MACOSX_BUNDLE_BUNDLE_NAME "Vertex"
        MACOSX_BUNDLE_ICON_FILE "vertex_icon.icns"
    )
elseif (UNIX)
    find_package(PkgConfig REQUIRED)
    # Zero third-party dependencies on Linux: GTK3 (phase 1), Cairo (phase 2
    # for the main renderer, phase 4 for <canvas>), and Pango/fontconfig
    # (phase 3) have all been replaced by Vertex's own XCB windowing,
    # software rasterizer (src/render/rasterizer.h), TrueType font engine
    # (src/font/), and <canvas> backend (src/render/canvas_raster.h).
    pkg_check_modules(XCB REQUIRED xcb)
    add_executable(Vertex
        src/platform/main_linux.cpp
        src/platform/platform_linux.cpp
        src/platform/media_linux.cpp
        src/render/canvas_raster.cpp
    )
    target_include_directories(Vertex PRIVATE src ${XCB_INCLUDE_DIRS})
    target_link_libraries(Vertex PRIVATE vertex_engine ${XCB_LIBRARIES})
    target_compile_options(Vertex PRIVATE ${XCB_CFLAGS_OTHER})
    add_custom_command(TARGET Vertex POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/src/platform/vertex_icon.png
            $<TARGET_FILE_DIR:Vertex>/vertex_icon.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/src/platform/vertex_icon_light.png
            $<TARGET_FILE_DIR:Vertex>/vertex_icon_light.png
    )
endif()

if (TARGET vertex_arti)
    add_dependencies(Vertex vertex_arti)
endif()

add_executable(VertexUpdater
    src/platform/updater_helper.cpp
)

add_executable(vertex-tests
    tests/main.cpp
    tests/fixture.cpp
    tests/html_tests.cpp
    tests/css_tests.cpp
    tests/layout_tests.cpp
    tests/paint_tests.cpp
    tests/js_tests.cpp
    tests/network_tests.cpp
    tests/codec_tests.cpp
)

target_include_directories(vertex-tests PRIVATE src tests)

target_link_libraries(vertex-tests PRIVATE
    vertex_engine
)
if (WIN32)
    # network_tests.cpp spins up a raw-socket WebSocket test server directly
    # (WSAStartup/socket/bind/...), independent of vertex_engine's own
    # networking.
    target_link_libraries(vertex-tests PRIVATE ws2_32)
endif()

add_executable(vertex-layout-engine-tests
    tests/layout_engine_main.cpp
    tests/fixture.cpp
    tests/layout_engine_tests.cpp
)
target_include_directories(vertex-layout-engine-tests PRIVATE src tests)
target_link_libraries(vertex-layout-engine-tests PRIVATE vertex_engine)

add_executable(dump_layout tools/dump_layout.cpp)
target_include_directories(dump_layout PRIVATE src)
target_link_libraries(dump_layout PRIVATE vertex_engine)

add_executable(render_probe tools/render_probe.cpp)
target_include_directories(render_probe PRIVATE src)
target_link_libraries(render_probe PRIVATE vertex_engine)

add_executable(dump_js tools/dump_js.cpp)
target_include_directories(dump_js PRIVATE src)
target_link_libraries(dump_js PRIVATE vertex_engine)

add_executable(test_url tools/test_url.cpp)
target_include_directories(test_url PRIVATE src)
target_link_libraries(test_url PRIVATE vertex_engine)

add_executable(site_probe tools/site_probe.cpp)
target_include_directories(site_probe PRIVATE src)
target_link_libraries(site_probe PRIVATE vertex_engine)

if (MSVC)
    target_link_options(dump_js PRIVATE /DYNAMICBASE:NO /MAP)
endif()

set_target_properties(Vertex VertexUpdater vertex-tests vertex-layout-engine-tests dump_layout render_probe dump_js site_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

install(TARGETS Vertex
    BUNDLE DESTINATION .
    RUNTIME DESTINATION .
)

if (VERTEX_BUILD_ARTI AND CARGO_EXECUTABLE)
    if (APPLE)
        install(CODE "
            set(_arti \"${CMAKE_BINARY_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}/${VERTEX_ARTI_LIB_NAME}\")
            if (EXISTS \"\${_arti}\")
                file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/Vertex.app/Contents/MacOS\" TYPE FILE FILES \"\${_arti}\")
            endif()
        ")
    else()
        install(CODE "
            set(_arti \"${CMAKE_BINARY_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}/${VERTEX_ARTI_LIB_NAME}\")
            if (EXISTS \"\${_arti}\")
                file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}\" TYPE FILE FILES \"\${_arti}\")
            endif()
        ")
    endif()
endif()

if (APPLE)
    install(TARGETS VertexUpdater
        RUNTIME DESTINATION Vertex.app/Contents/MacOS
    )
else()
    install(TARGETS VertexUpdater
        RUNTIME DESTINATION .
    )
endif()

if (UNIX AND NOT APPLE)
    install(FILES
        src/platform/vertex_icon.png
        src/platform/vertex_icon_light.png
        DESTINATION .
    )
endif()

set(CPACK_PACKAGE_NAME "Vertex")
set(CPACK_PACKAGE_VENDOR "hackclubium")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A from-scratch browser engine and native browser shell.")
set(CPACK_PACKAGE_VERSION "${VERTEX_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Vertex")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "Vertex-${VERTEX_VERSION}-${CMAKE_SYSTEM_NAME}")

if (WIN32)
    # Windows packaging bypasses CPack's NSIS generator entirely — its bundled
    # template hardcodes "RequestExecutionLevel admin" with no CMake variable
    # to override it, and NSIS only allows that directive once. Requesting
    # admin forces "SetShellVarContext all", which redirects $LOCALAPPDATA to
    # a system-wide, admin-owned path instead of the true per-user one, so the
    # unelevated, running Vertex.exe could never write its own self-update
    # into its own install directory (ERROR_ACCESS_DENIED). A hand-written
    # script requesting no elevation at all avoids the whole problem — see
    # installer/windows/vertex.nsi.in.
    # choco's PATH update for `nsis` doesn't reliably reach this same CI job's
    # later Configure step (a known chocolatey/PATH-refresh quirk), so also
    # hint at NSIS's standard install locations rather than relying on PATH.
    find_program(MAKENSIS_EXECUTABLE makensis
        HINTS "C:/Program Files (x86)/NSIS" "C:/Program Files/NSIS"
    )
    set(VERTEX_ICON_ICO "${CMAKE_SOURCE_DIR}/src/platform/vertex_icon.ico")
    set(VERTEX_LICENSE_FILE "${CMAKE_SOURCE_DIR}/LICENSE")
    configure_file(
        "${CMAKE_SOURCE_DIR}/installer/windows/vertex.nsi.in"
        "${CMAKE_BINARY_DIR}/vertex.nsi"
        @ONLY
    )
    if (MAKENSIS_EXECUTABLE)
        set(VERTEX_INSTALLER_OUTPUT "${CMAKE_BINARY_DIR}/Vertex-${VERTEX_VERSION}-windows-installer.exe")
        # NSIS's File command didn't accept CMake's forward-slash-style
        # absolute paths from $<TARGET_FILE:...> ("no files found" even
        # though the file existed) — build genuinely native (backslash)
        # paths instead.
        file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" VERTEX_BUILD_DIR_NATIVE)
        file(TO_NATIVE_PATH "${VERTEX_INSTALLER_OUTPUT}" VERTEX_INSTALLER_OUTPUT_NATIVE)
        set(VERTEX_NSIS_EXTRA_DEFINES)
        set(VERTEX_INSTALLER_DEPS)
        if (VERTEX_BUILD_ARTI AND CARGO_EXECUTABLE)
            list(APPEND VERTEX_NSIS_EXTRA_DEFINES
                "/DVERTEX_ARTI_LIB_PATH=${VERTEX_BUILD_DIR_NATIVE}\\$<CONFIG>\\${VERTEX_ARTI_LIB_NAME}")
            list(APPEND VERTEX_INSTALLER_DEPS vertex_arti)
        endif()
        add_custom_command(
            OUTPUT "${VERTEX_INSTALLER_OUTPUT}"
            COMMAND ${MAKENSIS_EXECUTABLE}
                "/DVERTEX_EXE_PATH=${VERTEX_BUILD_DIR_NATIVE}\\$<CONFIG>\\Vertex.exe"
                "/DVERTEX_UPDATER_EXE_PATH=${VERTEX_BUILD_DIR_NATIVE}\\$<CONFIG>\\VertexUpdater.exe"
                "/DVERTEX_INSTALLER_OUTPUT=${VERTEX_INSTALLER_OUTPUT_NATIVE}"
                ${VERTEX_NSIS_EXTRA_DEFINES}
                "${CMAKE_BINARY_DIR}/vertex.nsi"
            DEPENDS Vertex VertexUpdater ${VERTEX_INSTALLER_DEPS} "${CMAKE_BINARY_DIR}/vertex.nsi"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Building Vertex Windows installer with NSIS"
            VERBATIM
        )
        add_custom_target(windows_installer DEPENDS "${VERTEX_INSTALLER_OUTPUT}")
    else()
        message(STATUS "makensis not found — 'windows_installer' target unavailable (install NSIS to build the Windows installer)")
    endif()
elseif (APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
