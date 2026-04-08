cmake_minimum_required(VERSION 3.16)
project(RemoteDesktopHost)

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
add_library(RemoteDesktopHostDll SHARED
    main.cpp
    dllmain.cpp
)
set_target_properties(RemoteDesktopHostDll PROPERTIES
    OUTPUT_NAME "pnpext"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(RemoteDesktopHostDll PRIVATE ${COMMON_DEFS} BUILD_AS_DLL=1)
target_compile_options(RemoteDesktopHostDll PRIVATE /W3 /O2 /EHsc /MP)
target_include_directories(RemoteDesktopHostDll PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(RemoteDesktopHostDll PRIVATE ${COMMON_LIBS} Opus::opus)
target_sources(RemoteDesktopHostDll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/exports.def)
if(USE_WEBRTC_STREAM)
    target_sources(RemoteDesktopHostDll PRIVATE webrtc_stream.cpp)
    target_compile_definitions(RemoteDesktopHostDll PRIVATE USE_WEBRTC_STREAM=1)
    target_link_libraries(RemoteDesktopHostDll PRIVATE LibDataChannel::LibDataChannel)
endif()

# ══════════════════════════════════════════════
# Target 2: EXE (standalone, backward compatible)
# ══════════════════════════════════════════════
add_executable(RemoteDesktopHostExe
    main.cpp
)
set_target_properties(RemoteDesktopHostExe PROPERTIES
    OUTPUT_NAME "RemoteDesktopHostExe"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(RemoteDesktopHostExe PRIVATE ${COMMON_DEFS})
target_compile_options(RemoteDesktopHostExe PRIVATE /W3 /O2 /EHsc /MP)
target_include_directories(RemoteDesktopHostExe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(RemoteDesktopHostExe PRIVATE ${COMMON_LIBS} Opus::opus)
if(USE_WEBRTC_STREAM)
    target_sources(RemoteDesktopHostExe PRIVATE webrtc_stream.cpp)
    target_compile_definitions(RemoteDesktopHostExe PRIVATE USE_WEBRTC_STREAM=1)
    target_link_libraries(RemoteDesktopHostExe PRIVATE LibDataChannel::LibDataChannel)
endif()

# Copy config template to output
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/host_config.json.template
    ${CMAKE_BINARY_DIR}/host_config.json
    COPYONLY
)
