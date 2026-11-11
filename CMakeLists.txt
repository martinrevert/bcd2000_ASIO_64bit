# Behringer BCD2000 64-bit ASIO and MIDI Driver Suite
# Copyright (C) 2024-2026 Martin Revert <martinrevert@gmail.com> and contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

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

# Static MSVC runtime (/MT) so bcd2000_asio.dll and System32\bcd2000_midi.dll
# never depend on host VCRUNTIME140.dll / MSVCP140.dll inside any DAW process.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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

# Milestone 5: WinUSB INF/Catalog Signer & Driver Installer Helper (bcd2000_winusb_setup.exe)
add_executable(bcd2000_winusb_setup
    tools/bcd2000_winusb_setup.cpp
)

target_link_libraries(bcd2000_winusb_setup
    PRIVATE
        crypt32
        wintrust
        setupapi
        newdev
)

# Copy hostsample.exe to third_party/asio-sdk/host/pc/hostsample.exe to match ROADMAP_VERIFICATION.md
add_custom_command(TARGET hostsample POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:hostsample>
        "${CMAKE_SOURCE_DIR}/third_party/asio-sdk/host/pc/hostsample.exe"
    COMMENT "Copying hostsample.exe to third_party/asio-sdk/host/pc/hostsample.exe"
)

# Set standard output directories matching verification roadmap
set_target_properties(bcd2000_diag_usb bcd2000_diag_midi bcd2000_diag_audio bcd2000_asio hostsample bcd2000_midi hostsample_midi bcd2000_winusb_setup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
)

# Milestone 5: Single-EXE NSIS Installer (Behringer_BCD2000_Setup_x64.exe)
find_program(MAKENSIS_EXECUTABLE
    NAMES makensis
    PATHS
        "${CMAKE_SOURCE_DIR}/.tools/nsis/nsis-3.13/Bin"
        "$ENV{ProgramFiles}/NSIS/Bin"
        "$ENV{ProgramFiles\(x86\)}/NSIS/Bin"
        "$ENV{ProgramFiles}/NSIS"
        "$ENV{ProgramFiles\(x86\)}/NSIS"
    NO_DEFAULT_PATH
)
if(NOT MAKENSIS_EXECUTABLE)
    find_program(MAKENSIS_EXECUTABLE NAMES makensis)
endif()

if(MAKENSIS_EXECUTABLE)
    message(STATUS "[NSIS] Found makensis: ${MAKENSIS_EXECUTABLE}")
    add_custom_target(bcd2000_installer
        COMMAND "${MAKENSIS_EXECUTABLE}"
            /V3
            /INPUTCHARSET UTF8
            "/DREPO_DIR=${CMAKE_SOURCE_DIR}"
            "/DBUILD_BIN_DIR=${CMAKE_BINARY_DIR}/$<CONFIG>"
            "/DOUT_FILE=${CMAKE_BINARY_DIR}/$<CONFIG>/Behringer_BCD2000_Setup_x64.exe"
            "${CMAKE_SOURCE_DIR}/installer/bcd2000_setup.nsi"
        DEPENDS
            bcd2000_asio
            bcd2000_midi
            bcd2000_winusb_setup
            bcd2000_diag_usb
            bcd2000_diag_midi
            bcd2000_diag_audio
            hostsample
            hostsample_midi
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/installer"
        COMMENT "Building single-EXE NSIS installer Behringer_BCD2000_Setup_x64.exe"
    )
else()
    message(STATUS "[NSIS] makensis not found; bcd2000_installer target disabled.")
endif()
