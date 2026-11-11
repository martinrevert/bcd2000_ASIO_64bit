/**
 * @file bcd2000_shared_state.h
 * @brief Inter-Process Communication, Shared Memory & Persistent Registry Settings
 *        for Behringer BCD2000 ASIO & MIDI Driver Suite.
 *
 * Behringer BCD2000 64-bit ASIO and MIDI Driver Suite
 * Copyright (C) 2024-2026 Martin Revert <martinrevert@gmail.com> and contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BCD2000_SHARED_STATE_H
#define BCD2000_SHARED_STATE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace bcd2000 {

constexpr const wchar_t *kSharedMemoryName = L"Local\\BCD2000_ASIO_SHARED_MEM_V1";
constexpr const wchar_t *kRegistryKeyPath  = L"SOFTWARE\\ASIO\\Behringer BCD2000 ASIO 64-bit";

enum RoutingMode : int32_t {
    Routing_Normal = 0,    // Ch 1-2 Master, Ch 3-4 Phones
    Routing_Swap   = 1,    // Ch 1-2 Phones, Ch 3-4 Master
    Routing_Mirror = 2     // Ch 1-2 Master, Ch 3-4 Master Mirror
};

enum SafetyHeadroom : int32_t {
    Headroom_Aggressive = 1, // 1 buffer prefill (~1.5 - 3 ms headroom)
    Headroom_Balanced   = 3, // 3 buffers prefill (default ~10 - 15 ms headroom)
    Headroom_Relaxed    = 5  // 5 buffers prefill (~25 ms headroom)
};

enum ClockMode : int32_t {
    Clock_DynamicLock  = 0,  // EP 0x83 IN implicit crystal lock
    Clock_FixedCadence = 1   // Fixed 44/45 frame cadence toggle
};

#pragma pack(push, 8)
struct Bcd2000SharedState {
    uint32_t structSize;
    uint32_t version;               // 0x02000000

    // Status & Host Context
    uint32_t driverActive;          // 1 if driver loaded in host, 0 if idle
    uint32_t isRunning;             // 1 if ASIO audio streaming active
    uint32_t activePid;             // Process ID of the DAW/Host
    wchar_t  dawProcessName[64];    // e.g. L"mixxx.exe"

    // Audio & Streaming Settings (Active / Applied)
    int32_t  bufferSize;            // 64, 128, 256, 512, 1024, 2048, 4096
    int32_t  routingMode;           // RoutingMode enum
    int32_t  monoDownmix;           // 1 = Mono, 0 = Stereo
    int32_t  outputTrimDb;          // 0, -3, -6, -12 dB
    int32_t  masterMute;            // 1 = Muted, 0 = Active
    int32_t  phonesMute;            // 1 = Muted, 0 = Active
    int32_t  safetyHeadroom;        // 1, 3, 5
    int32_t  clockMode;             // ClockMode enum
    int32_t  directMonitor;         // 1 = On, 0 = Off
    int32_t  alwaysOnTop;           // 1 = Keep on top (default), 0 = Normal z-order

    // Control Panel Action Triggers
    uint32_t requestedBufferSize;   // Non-zero requests kAsioResetRequest
    uint32_t resetGlitchCounter;    // Set to 1 by UI, cleared by driver
    uint32_t flashLedsTrigger;      // Set to 1 by UI, cleared by driver

    // Live Telemetry from Driver to UI
    double   sampleRate;            // Active ASIO sample rate (44100.0)
    double   liveDacRate;           // Live measured DAC rate (44098.4)
    double   clockDriftPpm;         // Clock drift in PPM
    uint32_t clockLocked;           // 1 if EP 0x83 locked
    uint64_t xrunCount;             // Glitch / underrun count
    uint32_t ringFillPercent;       // 0 - 100 %
    int16_t  peakMasterL;           // Peak levels (0 - 32767)
    int16_t  peakMasterR;
    int16_t  peakPhonesL;
    int16_t  peakPhonesR;

    // Hardware Status
    uint32_t hardwareConnected;     // 1 if USB hardware attached
    uint32_t midiPipeMode;          // 1 = Mode A (ASIO Pipe), 0 = Mode B (Standalone WinUSB)
    uint32_t midiClientCount;       // Number of connected WinMM clients
};
#pragma pack(pop)

inline void InitDefaultSharedState(Bcd2000SharedState *state) {
    if (!state) return;
    std::memset(state, 0, sizeof(Bcd2000SharedState));
    state->structSize = sizeof(Bcd2000SharedState);
    state->version = 0x02000000;
    state->bufferSize = 256;
    state->routingMode = Routing_Normal;
    state->monoDownmix = 0;
    state->outputTrimDb = 0;
    state->masterMute = 0;
    state->phonesMute = 0;
    state->safetyHeadroom = Headroom_Balanced;
    state->clockMode = Clock_DynamicLock;
    state->directMonitor = 0;
    state->alwaysOnTop = 1;
    state->sampleRate = 44100.0;
    state->liveDacRate = 44100.0;
    state->clockLocked = 1;
}

inline void LoadSettingsFromRegistry(Bcd2000SharedState *state) {
    if (!state) return;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwordVal = 0;
        DWORD size = sizeof(DWORD);
        DWORD type = REG_DWORD;

        if (RegQueryValueExW(hKey, L"BufferSize", nullptr, &type, reinterpret_cast<LPBYTE>(&dwordVal), &size) == ERROR_SUCCESS) {
            if (dwordVal >= 64 && dwordVal <= 4096) state->bufferSize = static_cast<int32_t>(dwordVal);
        }
        size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"RoutingMode", nullptr, &type, reinterpret_cast<LPBYTE>(&dwordVal), &size) == ERROR_SUCCESS) {
            if (dwordVal <= 2) state->routingMode = static_cast<int32_t>(dwordVal);
        }
        size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"MonoDownmix", nullptr, &type, reinterpret_cast<LPBYTE>(&dwordVal), &size) == ERROR_SUCCESS) {
            state->monoDownmix = (dwordVal != 0) ? 1 : 0;
        }
        size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"OutputTrimDb", nullptr, &type, reinterpret_cast<LPBYTE>(&dwordVal), &size) == ERROR_SUCCESS) {
            state->outputTrimDb = static_cast<int32_t>(dwordVal);
        }
        size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"SafetyHeadroom", nullptr, &type, reinterpret_cast<LPBYTE>(&dwordVal), &size) == ERROR_SUCCESS) {
            if (dwordVal == 1 || dwordVal == 3 || dwordVal == 5) state->safetyHeadroom = static_cast<int32_t>(dwordVal);
        }
        size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"ClockMode", nullptr, &type, reinterpret_cast<LPBYTE>(&dwordVal), &size) == ERROR_SUCCESS) {
            state->clockMode = (dwordVal != 0) ? Clock_FixedCadence : Clock_DynamicLock;
        }
        size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"DirectMonitor", nullptr, &type, reinterpret_cast<LPBYTE>(&dwordVal), &size) == ERROR_SUCCESS) {
            state->directMonitor = (dwordVal != 0) ? 1 : 0;
        }
        size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"AlwaysOnTop", nullptr, &type, reinterpret_cast<LPBYTE>(&dwordVal), &size) == ERROR_SUCCESS) {
            state->alwaysOnTop = (dwordVal != 0) ? 1 : 0;
        } else {
            state->alwaysOnTop = 1;
        }
        RegCloseKey(hKey);
    }
}

inline void SaveSettingsToRegistry(const Bcd2000SharedState *state) {
    if (!state) return;
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD dwordVal = static_cast<DWORD>(state->bufferSize);
        RegSetValueExW(hKey, L"BufferSize", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dwordVal), sizeof(DWORD));

        dwordVal = static_cast<DWORD>(state->routingMode);
        RegSetValueExW(hKey, L"RoutingMode", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dwordVal), sizeof(DWORD));

        dwordVal = static_cast<DWORD>(state->monoDownmix);
        RegSetValueExW(hKey, L"MonoDownmix", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dwordVal), sizeof(DWORD));

        dwordVal = static_cast<DWORD>(state->outputTrimDb);
        RegSetValueExW(hKey, L"OutputTrimDb", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dwordVal), sizeof(DWORD));

        dwordVal = static_cast<DWORD>(state->safetyHeadroom);
        RegSetValueExW(hKey, L"SafetyHeadroom", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dwordVal), sizeof(DWORD));

        dwordVal = static_cast<DWORD>(state->clockMode);
        RegSetValueExW(hKey, L"ClockMode", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dwordVal), sizeof(DWORD));

        dwordVal = static_cast<DWORD>(state->directMonitor);
        RegSetValueExW(hKey, L"DirectMonitor", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dwordVal), sizeof(DWORD));

        dwordVal = static_cast<DWORD>(state->alwaysOnTop);
        RegSetValueExW(hKey, L"AlwaysOnTop", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dwordVal), sizeof(DWORD));

        RegCloseKey(hKey);
    }
}

inline Bcd2000SharedState *OpenOrCreateSharedState(HANDLE *outHandle, bool isServer) {
    if (!outHandle) return nullptr;
    *outHandle = nullptr;

    HANDLE hMap = nullptr;
    bool created = false;

    if (isServer) {
        hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(Bcd2000SharedState), kSharedMemoryName);
        if (hMap) {
            created = (GetLastError() != ERROR_ALREADY_EXISTS);
        }
    } else {
        hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedMemoryName);
        if (!hMap) {
            // If server not running, client can create mapping as fallback
            hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(Bcd2000SharedState), kSharedMemoryName);
            if (hMap) {
                created = (GetLastError() != ERROR_ALREADY_EXISTS);
            }
        }
    }

    if (!hMap) {
        return nullptr;
    }

    void *buf = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Bcd2000SharedState));
    if (!buf) {
        CloseHandle(hMap);
        return nullptr;
    }

    Bcd2000SharedState *state = static_cast<Bcd2000SharedState *>(buf);
    if (created) {
        InitDefaultSharedState(state);
        LoadSettingsFromRegistry(state);
    }

    *outHandle = hMap;
    return state;
}

inline void CloseSharedState(HANDLE hMap, Bcd2000SharedState *state) {
    if (state) {
        UnmapViewOfFile(state);
    }
    if (hMap) {
        CloseHandle(hMap);
    }
}

} // namespace bcd2000

#endif // BCD2000_SHARED_STATE_H
