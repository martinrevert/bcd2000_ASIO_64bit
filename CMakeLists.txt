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

# Milestone 4: 64-bit COM ASIO Driver DLL (bcd2000_asio.dll)
add_library(bcd2000_asio SHARED
    src/asio/bcd2000_asio_driver.cpp
    src/asio/dllmain_registry.cpp
    src/asio/bcd2000_asio.def
)

target_link_libraries(bcd2000_asio
    PRIVATE
        winusb
        setupapi
        ole32
        advapi32
        user32
        avrt
)

# Milestone 4: 64-bit ASIO Host Verification Utility (hostsample.exe)
add_executable(hostsample
    third_party/asio-sdk/host/pc/hostsample.cpp
)

target_link_libraries(hostsample
    PRIVATE
        ole32
        advapi32
        user32
)

# Milestone 4.5: 64-bit User-Mode WinMM MIDI Driver DLL (bcd2000_midi.dll)
add_library(bcd2000_midi SHARED
    src/midi/bcd2000_midi_driver.cpp
    src/midi/bcd2000_midi.def
)

target_link_libraries(bcd2000_midi
    PRIVATE
        winmm
        winusb
        setupapi
        advapi32
        ole32
)

# Milestone 4.5: WinMM MIDI Host Verification Utility (hostsample_midi.exe)
add_executable(hostsample_midi
    tools/hostsample_midi.cpp
)

target_link_libraries(hostsample_midi
    PRIVATE
        winmm
)

# Copy hostsample.exe to third_party/asio-sdk/host/pc/hostsample.exe to match ROADMAP_VERIFICATION.md
add_custom_command(TARGET hostsample POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:hostsample>
        "${CMAKE_SOURCE_DIR}/third_party/asio-sdk/host/pc/hostsample.exe"
    COMMENT "Copying hostsample.exe to third_party/asio-sdk/host/pc/hostsample.exe"
)

# Set standard output directories matching verification roadmap
set_target_properties(bcd2000_diag_usb bcd2000_diag_midi bcd2000_diag_audio bcd2000_asio hostsample bcd2000_midi hostsample_midi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
)
