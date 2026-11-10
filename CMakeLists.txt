cmake_minimum_required(VERSION 3.20)
project(bcd2000_ASIO_64bit LANGUAGES C CXX)

# Enforce 64-bit architecture check
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(STATUS "[ARCH] Target architecture validated: 64-bit (x64).")
else()
    message(FATAL_ERROR "[ARCH] This project requires a 64-bit (x64) compiler target. Current pointer size: ${CMAKE_SIZEOF_VOID_P} bytes.")
endif()

# C++17 Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Global compiler definitions
add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# MSVC specific options
if(MSVC)
    add_compile_options(
        /W4             # Warning level 4
        /permissive-    # Strict standards conformance
        /utf-8          # UTF-8 source and execution charset
        /EHsc           # C++ exception handling
    )
endif()

# Milestone 1: USB Diagnostic Utility
add_executable(bcd2000_diag_usb
    tools/bcd2000_diag_usb.cpp
)

target_link_libraries(bcd2000_diag_usb
    PRIVATE
        winusb
        setupapi
        ole32
)

# Milestone 2: Physical Control Telemetry & Bi-Directional LED Engine
add_executable(bcd2000_diag_midi
    tools/bcd2000_diag_midi.cpp
)

target_link_libraries(bcd2000_diag_midi
    PRIVATE
        winusb
        setupapi
        ole32
)

# Milestone 3: Isochronous PCM Audio Streaming & Sine Wave Synthesizer
add_executable(bcd2000_diag_audio
    tools/bcd2000_diag_audio.cpp
)

target_link_libraries(bcd2000_diag_audio
    PRIVATE
        winusb
        setupapi
        ole32
        avrt
)

# Set standard output directories matching verification roadmap
set_target_properties(bcd2000_diag_usb bcd2000_diag_midi bcd2000_diag_audio PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
)
