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

# ── libcurl (cross-platform HTTP) ─────────────────────────────────────────────
# Try system-installed libcurl first (apt install libcurl4-openssl-dev, brew
# install curl, etc.). Fall back to FetchContent download if not found.
find_package(CURL QUIET)
if (CURL_FOUND)
    message(STATUS "Using system libcurl: ${CURL_VERSION_STRING}")
else ()
    message(STATUS "System libcurl not found — downloading via FetchContent")
    include(FetchContent)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    # Per-OS TLS backend for libcurl
    if (WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
        set(CURL_USE_SECTRANSP OFF CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    else ()
        set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
        set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_SECTRANSP OFF CACHE BOOL "" FORCE)
        if (APPLE AND EXISTS "/opt/homebrew/opt/openssl")
            set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl" CACHE PATH "" FORCE)
        elseif (APPLE AND EXISTS "/usr/local/opt/openssl")
            set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl" CACHE PATH "" FORCE)
        endif ()
    endif ()
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(curl
        URL https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(curl)
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
    src/network/fetcher.cpp
    src/network/resource_cache.cpp
    src/network/text_decode.cpp
    src/network/url.cpp
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
    src/js/engine.cpp
    src/third_party/stb_image_impl.cpp
)

target_include_directories(vertex_engine PUBLIC src)
target_link_libraries(vertex_engine PUBLIC CURL::libcurl)
if (NOT CURL_FOUND)
    target_compile_definitions(vertex_engine PUBLIC CURL_STATICLIB)
endif ()
if (WIN32)
    target_compile_definitions(vertex_engine PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if (MSVC)
    # /EHa: catch SEH exceptions (stack overflow, access violation) in catch(...)
    target_compile_options(vertex_engine PUBLIC /EHa)
endif()

# ── platform backends ─────────────────────────────────────────────────────────
# Each OS gets its own renderer + windowing backend. The engine library is
# platform-independent; only these files differ per OS.

if (WIN32)
    add_executable(Vertex WIN32
        src/main.cpp
        src/render/renderer.cpp
        src/render/box_paint.cpp
        src/platform/platform_win32.cpp
    )
    target_include_directories(Vertex PRIVATE src)
    target_link_libraries(Vertex PRIVATE
        vertex_engine d2d1 dwrite comctl32 ole32
    )
    if (MSVC)
        target_link_options(Vertex PRIVATE /STACK:67108864)
    endif()
elseif (APPLE)
    add_executable(Vertex MACOSX_BUNDLE
        src/platform/main_macos.mm
        src/platform/platform_macos.mm
    )
    target_include_directories(Vertex PRIVATE src)
    target_link_libraries(Vertex PRIVATE
        vertex_engine
        "-framework Cocoa"
        "-framework CoreGraphics"
        "-framework CoreText"
    )
    set_target_properties(Vertex PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.vertex.browser"
        MACOSX_BUNDLE_BUNDLE_NAME "Vertex"
    )
elseif (UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    add_executable(Vertex
        src/platform/main_linux.cpp
        src/platform/platform_linux.cpp
    )
    target_include_directories(Vertex PRIVATE src ${GTK3_INCLUDE_DIRS})
    target_link_libraries(Vertex PRIVATE vertex_engine ${GTK3_LIBRARIES})
    target_compile_options(Vertex PRIVATE ${GTK3_CFLAGS_OTHER})
endif()

add_executable(vertex-tests
    src/test/main.cpp
    src/test/fixture.cpp
    src/test/html_tests.cpp
    src/test/css_tests.cpp
    src/test/layout_tests.cpp
    src/test/paint_tests.cpp
    src/test/js_tests.cpp
    src/test/network_tests.cpp
)

target_include_directories(vertex-tests PRIVATE src)

target_link_libraries(vertex-tests PRIVATE
    vertex_engine
)

add_executable(vertex-layout-engine-tests
    src/test/layout_engine_main.cpp
    src/test/fixture.cpp
    src/test/layout_engine_tests.cpp
)
target_include_directories(vertex-layout-engine-tests PRIVATE src)
target_link_libraries(vertex-layout-engine-tests PRIVATE vertex_engine)

add_executable(dump_layout tools/dump_layout.cpp)
target_include_directories(dump_layout PRIVATE src)
target_link_libraries(dump_layout PRIVATE vertex_engine)

add_executable(dump_js tools/dump_js.cpp)
target_include_directories(dump_js PRIVATE src)
target_link_libraries(dump_js PRIVATE vertex_engine)

add_executable(test_url tools/test_url.cpp)
target_include_directories(test_url PRIVATE src)
target_link_libraries(test_url PRIVATE vertex_engine)
if (MSVC)
    target_link_options(dump_js PRIVATE /DYNAMICBASE:NO /MAP)
endif()

set_target_properties(Vertex vertex-tests vertex-layout-engine-tests dump_layout dump_js PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)
