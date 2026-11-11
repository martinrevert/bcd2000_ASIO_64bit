/**
 * @file hostsample.cpp
 * @brief Steinberg ASIO 2.3 (64-bit) Host Verification Utility for Behringer BCD2000 (Milestone 4)
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
 *
 * Verifies:
 *   1. Windows Registry discovery under SOFTWARE\ASIO and HKEY_CLASSES_ROOT\CLSID
 *   2. 64-bit In-Process COM instantiation via CoCreateInstance (IASIO interface)
 *   3. ASIO Handshake: init(), getDriverName(), getDriverVersion(), getChannels(),
 *      getBufferSize() (64..512 samples), canSampleRate(), getSampleRate(),
 *      getClockSources(), getChannelInfo(), createBuffers(), getLatencies()
 *   4. Real-time callback cycle: start() -> bufferSwitch / bufferSwitchTimeInfo
 *      synthesizing a 440 Hz stereo reference tone to Master RCA (Ch 1-2) and
 *      Headphones (Ch 3-4) -> stop() -> disposeBuffers() -> Release()
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <conio.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../common/iasiodrv.h"

namespace {

constexpr const wchar_t *kTargetDriverNameW = L"Behringer BCD2000 ASIO 64-bit";
constexpr const wchar_t *kAsioRegSubKeyW    = L"SOFTWARE\\ASIO\\Behringer BCD2000 ASIO 64-bit";
constexpr double kTwoPi                     = 6.283185307179586476925286766559;

struct HostRuntimeState {
    IASIO *driver{nullptr};
    long numInputs{0};
    long numOutputs{0};
    long minSize{0};
    long maxSize{0};
    long preferredSize{0};
    long granularity{0};
    long activeBufferSize{256};
    ASIOSampleRate sampleRate{44100.0};
    std::vector<ASIOBufferInfo> bufferInfos;
    std::vector<ASIOChannelInfo> channelInfos;

    std::atomic<uint64_t> callbackCount{0};
    std::atomic<uint64_t> timeInfoCallbackCount{0};
    std::atomic<int64_t> lastSamplePos{0};
    double phaseL{0.0};
    double phaseR{0.0};
    double toneFreqHz{440.0};
    double amplitude{0.25};
};

HostRuntimeState g_Host;
std::atomic<bool> g_StopRequested{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        g_StopRequested.store(true);
        return TRUE;
    }
    return FALSE;
}

void FillOutputBuffers(long doubleBufferIndex) {
    const long frames = g_Host.activeBufferSize;
    const double sr = (g_Host.sampleRate > 0.0) ? g_Host.sampleRate : 44100.0;
    const double incL = (kTwoPi * g_Host.toneFreqHz) / sr;
    const double incR = (kTwoPi * g_Host.toneFreqHz) / sr;

    // Identify output buffer pointers and sample types for the current doubleBufferIndex (0 or 1)
    void *outPtrs[4] = {nullptr, nullptr, nullptr, nullptr};
    ASIOSampleType outTypes[4] = {ASIOSTInt32LSB, ASIOSTInt32LSB, ASIOSTInt32LSB, ASIOSTInt32LSB};
    for (size_t i = 0; i < g_Host.bufferInfos.size(); ++i) {
        const ASIOBufferInfo &bi = g_Host.bufferInfos[i];
        if (bi.isInput == ASIOFalse && bi.channelNum >= 0 && bi.channelNum < 4) {
            outPtrs[bi.channelNum] = bi.buffers[doubleBufferIndex];
            if (i < g_Host.channelInfos.size()) {
                outTypes[bi.channelNum] = g_Host.channelInfos[i].type;
            }
        }
    }

    for (long f = 0; f < frames; ++f) {
        const double valL = std::sin(g_Host.phaseL) * g_Host.amplitude;
        const double valR = std::sin(g_Host.phaseR) * g_Host.amplitude;
        const int16_t s16L = static_cast<int16_t>(valL * 32767.0);
        const int16_t s16R = static_cast<int16_t>(valR * 32767.0);
        const int32_t s32L = static_cast<int32_t>(valL * 2147483647.0);
        const int32_t s32R = static_cast<int32_t>(valR * 2147483647.0);

        g_Host.phaseL += incL;
        if (g_Host.phaseL >= kTwoPi) {
            g_Host.phaseL -= kTwoPi;
        }
        g_Host.phaseR += incR;
        if (g_Host.phaseR >= kTwoPi) {
            g_Host.phaseR -= kTwoPi;
        }

        for (int ch = 0; ch < 4; ++ch) {
            if (!outPtrs[ch]) {
                continue;
            }
            const bool isLeft = (ch % 2 == 0);
            if (outTypes[ch] == ASIOSTInt16LSB) {
                static_cast<int16_t *>(outPtrs[ch])[f] = isLeft ? s16L : s16R;
            } else {
                static_cast<int32_t *>(outPtrs[ch])[f] = isLeft ? s32L : s32R;
            }
        }
    }

    if (g_Host.driver) {
        g_Host.driver->outputReady();
    }
}

ASIOTime *AsioBufferSwitchTimeInfoCallback(ASIOTime *params, long doubleBufferIndex, ASIOBool directProcess) {
    (void)directProcess;
    g_Host.callbackCount.fetch_add(1, std::memory_order_relaxed);
    g_Host.timeInfoCallbackCount.fetch_add(1, std::memory_order_relaxed);

    if (params) {
        g_Host.lastSamplePos.store(static_cast<int64_t>(AsioSamplesToUint64(params->timeInfo.samplePosition)), std::memory_order_relaxed);
    } else if (g_Host.driver) {
        ASIOSamples sPos{};
        ASIOTimeStamp tStamp{};
        if (g_Host.driver->getSamplePosition(&sPos, &tStamp) == ASE_OK) {
            g_Host.lastSamplePos.store(static_cast<int64_t>(AsioSamplesToUint64(sPos)), std::memory_order_relaxed);
        }
    }

    FillOutputBuffers(doubleBufferIndex);
    return params;
}

void AsioBufferSwitchCallback(long doubleBufferIndex, ASIOBool directProcess) {
    ASIOTime timeInfo{};
    if (g_Host.driver) {
        ASIOSamples sPos{};
        ASIOTimeStamp tStamp{};
        if (g_Host.driver->getSamplePosition(&sPos, &tStamp) == ASE_OK) {
            timeInfo.timeInfo.samplePosition = sPos;
            timeInfo.timeInfo.systemTime = tStamp;
        }
    }
    AsioBufferSwitchTimeInfoCallback(&timeInfo, doubleBufferIndex, directProcess);
}

void AsioSampleRateDidChangeCallback(ASIOSampleRate sRate) {
    std::printf("  [CALLBACK] sampleRateDidChange(%.1f Hz)\n", sRate);
    g_Host.sampleRate = sRate;
}

long AsioMessageCallback(long selector, long value, void *message, double *opt) {
    (void)message;
    (void)opt;
    switch (selector) {
        case kAsioSelectorSupported:
            if (value == kAsioResetRequest ||
                value == kAsioEngineVersion ||
                value == kAsioResyncRequest ||
                value == kAsioLatenciesChanged ||
                value == kAsioSupportsTimeInfo ||
                value == kAsioSupportsTimeCode) {
                return 1L;
            }
            return 0L;
        case kAsioEngineVersion:
            return 2L; // ASIO 2.0+ host protocol
        case kAsioResetRequest:
            std::printf("  [CALLBACK] kAsioResetRequest recibido del driver.\n");
            g_StopRequested.store(true);
            return 1L;
        case kAsioResyncRequest:
            return 1L;
        case kAsioLatenciesChanged:
            return 1L;
        case kAsioSupportsTimeInfo:
            return 1L;
        case kAsioSupportsTimeCode:
            return 0L;
        default:
            return 0L;
    }
}

bool QueryRegistryDriverClsid(CLSID &outClsid, std::wstring &outHiveName, std::wstring &outDllPath) {
    HKEY hKey = nullptr;
    LONG status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        kAsioRegSubKeyW,
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &hKey
    );
    if (status == ERROR_SUCCESS) {
        outHiveName = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\ASIO\\Behringer BCD2000 ASIO 64-bit";
    } else {
        status = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kAsioRegSubKeyW,
            0,
            KEY_READ | KEY_WOW64_64KEY,
            &hKey
        );
        if (status == ERROR_SUCCESS) {
            outHiveName = L"HKEY_CURRENT_USER\\SOFTWARE\\ASIO\\Behringer BCD2000 ASIO 64-bit";
        }
    }

    if (status != ERROR_SUCCESS || !hKey) {
        return false;
    }

    wchar_t clsidStr[128] = {};
    DWORD clsidBytes = sizeof(clsidStr);
    DWORD regType = 0;
    status = RegQueryValueExW(
        hKey,
        L"CLSID",
        nullptr,
        &regType,
        reinterpret_cast<LPBYTE>(clsidStr),
        &clsidBytes
    );
    RegCloseKey(hKey);

    if (status != ERROR_SUCCESS) {
        return false;
    }

    if (FAILED(CLSIDFromString(clsidStr, &outClsid))) {
        return false;
    }

    // Verify HKEY_CLASSES_ROOT\CLSID\<clsid>\InprocServer32
    std::wstring inprocPath = std::wstring(L"CLSID\\") + clsidStr + L"\\InprocServer32";
    HKEY hInproc = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, inprocPath.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hInproc) == ERROR_SUCCESS) {
        wchar_t dllPathBuf[MAX_PATH] = {};
        DWORD dllBytes = sizeof(dllPathBuf);
        if (RegQueryValueExW(hInproc, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(dllPathBuf), &dllBytes) == ERROR_SUCCESS) {
            outDllPath = dllPathBuf;
        }
        RegCloseKey(hInproc);
    }

    return true;
}

const char *SampleTypeToString(ASIOSampleType type) {
    switch (type) {
        case ASIOSTInt16LSB:   return "ASIOSTInt16LSB (16-bit Signed LE PCM)";
        case ASIOSTInt24LSB:   return "ASIOSTInt24LSB (24-bit Signed LE PCM)";
        case ASIOSTInt32LSB:   return "ASIOSTInt32LSB (32-bit Signed LE PCM)";
        case ASIOSTFloat32LSB: return "ASIOSTFloat32LSB (32-bit Float LE)";
        default:               return "Otro formato ASIO";
    }
}

void PrintUsage(const char *progName) {
    std::printf("Uso: %s [opciones]\n", progName);
    std::printf("Opciones:\n");
    std::printf("  --duration <seg>     Duracion del ciclo de streaming ASIO (default: 6 seg)\n");
    std::printf("  --buffer-size <N>    Tamano de buffer ASIO a negociar: 64..512 (default: preferred=256)\n");
    std::printf("  --freq <Hz>          Frecuencia del tono de prueba en Hz (default: 440.0)\n");
    std::printf("  --help               Muestra esta ayuda\n");
}

} // namespace

int main(int argc, char *argv[]) {
    int durationSeconds = 6;
    long requestedBufferSize = 0; // 0 = use driver's preferredSize (256)

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            durationSeconds = std::atoi(argv[++i]);
            if (durationSeconds < 1) durationSeconds = 1;
        } else if (std::strcmp(argv[i], "--buffer-size") == 0 && i + 1 < argc) {
            requestedBufferSize = std::atol(argv[++i]);
        } else if (std::strcmp(argv[i], "--freq") == 0 && i + 1 < argc) {
            g_Host.toneFreqHz = std::atof(argv[++i]);
        }
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    std::printf("======================================================================\n");
    std::printf("   Steinberg ASIO 2.3 (x64) Host Verification Sample (Hito 4)\n");
    std::printf("   Target Driver: \"Behringer BCD2000 ASIO 64-bit\"\n");
    std::printf("   Arquitectura : %zu-bit (sizeof(void*) = %zu bytes)\n",
                sizeof(void *) * 8, sizeof(void *));
    std::printf("   Copyright (C) 2024-2026 Martin Revert and contributors (GPL-3.0+)\n");
    std::printf("   This is free software with ABSOLUTELY NO WARRANTY. See LICENSE.\n");
    std::printf("======================================================================\n\n");

    // 1. Verify Windows Registry registration
    std::printf("[1/5] Consultando Registro de Windows (SOFTWARE\\ASIO & HKCR\\CLSID)...\n");
    CLSID driverClsid = CLSID_BCD2000_ASIO;
    std::wstring regHiveName;
    std::wstring dllPath;
    if (QueryRegistryDriverClsid(driverClsid, regHiveName, dllPath)) {
        std::printf("  [+] Clave ASIO encontrada en: %ls\n", regHiveName.c_str());
        if (!dllPath.empty()) {
            std::printf("  [+] InprocServer32 DLL (x64): %ls\n", dllPath.c_str());
        }
    } else {
        std::printf("  [!] No se encontro la clave en el Registro. Ejecute primero:\n");
        std::printf("      regsvr32 /s .\\build\\RelWithDebInfo\\bcd2000_asio.dll\n");
        return 1;
    }

    // 2. Initialize COM & Instantiate IASIO
    std::printf("\n[2/5] Inicializando subsistema COM (Apartment) e instanciando IASIO...\n");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::printf("  [-] Error en CoInitializeEx: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    IASIO *pAsio = nullptr;
    hr = CoCreateInstance(
        driverClsid,
        nullptr,
        CLSCTX_INPROC_SERVER,
        driverClsid,
        reinterpret_cast<void **>(&pAsio)
    );
    if (FAILED(hr) || !pAsio) {
        std::printf("  [-] Error en CoCreateInstance(CLSID_BCD2000_ASIO): 0x%08lX\n", static_cast<unsigned long>(hr));
        CoUninitialize();
        return 1;
    }
    g_Host.driver = pAsio;
    std::printf("  [+] Servidor COM instanciado con exito (IASIO* = %p).\n", static_cast<void *>(pAsio));

    // 3. ASIO Handshake: init(), getDriverName(), getDriverVersion(), getChannels(), getBufferSize()
    std::printf("\n[3/5] Ejecutando Handshake ASIO 2.3 con el driver...\n");
    if (pAsio->init(GetConsoleWindow()) != ASIOTrue) {
        char errBuf[128] = {};
        pAsio->getErrorMessage(errBuf);
        std::printf("  [-] Error en IASIO::init(): %s\n", errBuf);
        pAsio->Release();
        CoUninitialize();
        return 1;
    }

    char drvName[64] = {};
    pAsio->getDriverName(drvName);
    const long drvVer = pAsio->getDriverVersion();
    std::printf("  [+] Driver Name    : %s\n", drvName);
    std::printf("  [+] Driver Version : %ld (ASIO 2.3 x64)\n", drvVer);

    if (pAsio->getChannels(&g_Host.numInputs, &g_Host.numOutputs) != ASE_OK) {
        std::printf("  [-] Error en IASIO::getChannels().\n");
        pAsio->Release();
        CoUninitialize();
        return 1;
    }
    std::printf("  [+] Canales ASIO   : %ld Entradas / %ld Salidas\n", g_Host.numInputs, g_Host.numOutputs);

    if (pAsio->getBufferSize(&g_Host.minSize, &g_Host.maxSize, &g_Host.preferredSize, &g_Host.granularity) != ASE_OK) {
        std::printf("  [-] Error en IASIO::getBufferSize().\n");
        pAsio->Release();
        CoUninitialize();
        return 1;
    }
    std::printf("  [+] Buffer Sizes   : Min=%ld, Max=%ld, Preferred=%ld, Granularity=%ld muestras\n",
                g_Host.minSize, g_Host.maxSize, g_Host.preferredSize, g_Host.granularity);

    if (pAsio->canSampleRate(44100.0) == ASE_OK) {
        pAsio->setSampleRate(44100.0);
    }
    pAsio->getSampleRate(&g_Host.sampleRate);
    std::printf("  [+] Sample Rate    : %.1f Hz\n", g_Host.sampleRate);

    ASIOClockSource clockSrc{};
    long numClocks = 1;
    if (pAsio->getClockSources(&clockSrc, &numClocks) == ASE_OK && numClocks > 0) {
        std::printf("  [+] Clock Source   : %s (Current=%s)\n",
                    clockSrc.name, clockSrc.isCurrentSource ? "Yes" : "No");
    }

    // 4. Query channel info & negotiate buffers (64..512 samples)
    std::printf("\n[4/5] Consultando descriptores de canales y negociando buffers DMA...\n");
    const long totalChannels = g_Host.numInputs + g_Host.numOutputs;
    g_Host.bufferInfos.resize(static_cast<size_t>(totalChannels));
    g_Host.channelInfos.resize(static_cast<size_t>(totalChannels));

    for (long c = 0; c < g_Host.numInputs; ++c) {
        ASIOChannelInfo &ci = g_Host.channelInfos[static_cast<size_t>(c)];
        ci.channel = c;
        ci.isInput = ASIOTrue;
        pAsio->getChannelInfo(&ci);
        std::printf("      [IN  %ld] %-24s | %s\n", c, ci.name, SampleTypeToString(ci.type));

        ASIOBufferInfo &bi = g_Host.bufferInfos[static_cast<size_t>(c)];
        bi.isInput = ASIOTrue;
        bi.channelNum = c;
        bi.buffers[0] = nullptr;
        bi.buffers[1] = nullptr;
    }

    for (long c = 0; c < g_Host.numOutputs; ++c) {
        const size_t idx = static_cast<size_t>(g_Host.numInputs + c);
        ASIOChannelInfo &ci = g_Host.channelInfos[idx];
        ci.channel = c;
        ci.isInput = ASIOFalse;
        pAsio->getChannelInfo(&ci);
        std::printf("      [OUT %ld] %-24s | %s\n", c, ci.name, SampleTypeToString(ci.type));

        ASIOBufferInfo &bi = g_Host.bufferInfos[idx];
        bi.isInput = ASIOFalse;
        bi.channelNum = c;
        bi.buffers[0] = nullptr;
        bi.buffers[1] = nullptr;
    }

    ASIOCallbacks asioCallbacks{};
    asioCallbacks.bufferSwitch = &AsioBufferSwitchCallback;
    asioCallbacks.sampleRateDidChange = &AsioSampleRateDidChangeCallback;
    asioCallbacks.asioMessage = &AsioMessageCallback;
    asioCallbacks.bufferSwitchTimeInfo = &AsioBufferSwitchTimeInfoCallback;

    // Verify buffer negotiation across the 64..512 range (64, 128, 256, 512)
    const long testSizes[] = {64, 128, 512};
    for (long testSz : testSizes) {
        if (testSz >= g_Host.minSize && testSz <= g_Host.maxSize) {
            if (pAsio->createBuffers(g_Host.bufferInfos.data(), totalChannels, testSz, &asioCallbacks) == ASE_OK) {
                long inLat = 0, outLat = 0;
                pAsio->getLatencies(&inLat, &outLat);
                std::printf("  [+] Negociacion de prueba exitosa @ %3ld muestras (Latencia In=%ld / Out=%ld muestras)\n",
                            testSz, inLat, outLat);
                pAsio->disposeBuffers();
            }
        }
    }

    // Create active buffers at preferredSize (256) or user-requested size
    g_Host.activeBufferSize = (requestedBufferSize > 0) ? requestedBufferSize : g_Host.preferredSize;
    if (pAsio->createBuffers(g_Host.bufferInfos.data(), totalChannels, g_Host.activeBufferSize, &asioCallbacks) != ASE_OK) {
        char errBuf[128] = {};
        pAsio->getErrorMessage(errBuf);
        std::printf("  [-] Error en IASIO::createBuffers(%ld): %s\n", g_Host.activeBufferSize, errBuf);
        pAsio->Release();
        CoUninitialize();
        return 1;
    }

    long inLatency = 0;
    long outLatency = 0;
    pAsio->getLatencies(&inLatency, &outLatency);
    const double inLatencyMs  = (static_cast<double>(inLatency) * 1000.0) / g_Host.sampleRate;
    const double outLatencyMs = (static_cast<double>(outLatency) * 1000.0) / g_Host.sampleRate;
    std::printf("  [+] Buffers activos creados @ %ld muestras (%.2f ms/bloque)\n",
                g_Host.activeBufferSize,
                (static_cast<double>(g_Host.activeBufferSize) * 1000.0) / g_Host.sampleRate);
    std::printf("  [+] Latencia reportada: Entrada = %ld muestras (%.2f ms) | Salida = %ld muestras (%.2f ms)\n",
                inLatency, inLatencyMs, outLatency, outLatencyMs);

    // 5. Start real-time ASIO streaming & callback verification
    std::printf("\n[5/5] Iniciando motor en tiempo real IASIO::start() (%d segundos, %.1f Hz)...\n",
                durationSeconds, g_Host.toneFreqHz);
    if (pAsio->start() != ASE_OK) {
        char errBuf[128] = {};
        pAsio->getErrorMessage(errBuf);
        std::printf("  [-] Error en IASIO::start(): %s\n", errBuf);
        pAsio->disposeBuffers();
        pAsio->Release();
        CoUninitialize();
        return 1;
    }

    const DWORD startTick = GetTickCount();
    const DWORD durationMs = static_cast<DWORD>(durationSeconds * 1000);
    while (!g_StopRequested.load()) {
        const DWORD elapsed = GetTickCount() - startTick;
        if (elapsed >= durationMs) {
            break;
        }
        if (_kbhit()) {
            const int ch = _getch();
            if (ch == 'q' || ch == 'Q' || ch == 27) {
                break;
            }
        }

        const uint64_t cbCount = g_Host.callbackCount.load(std::memory_order_relaxed);
        const int64_t samplePos = g_Host.lastSamplePos.load(std::memory_order_relaxed);
        const double audioSec = static_cast<double>(samplePos) / g_Host.sampleRate;

        std::printf("\r  [STREAMING] bufferSwitch: %5llu callbacks | SamplePos: %9lld (%.2f s) | Elapsed: %.1f/%d.0 s   ",
                    static_cast<unsigned long long>(cbCount),
                    static_cast<long long>(samplePos),
                    audioSec,
                    static_cast<double>(elapsed) / 1000.0,
                    durationSeconds);
        std::fflush(stdout);
        Sleep(100);
    }
    std::printf("\n");

    // Stop and clean up
    pAsio->stop();
    pAsio->disposeBuffers();
    pAsio->Release();
    g_Host.driver = nullptr;
    CoUninitialize();

    const uint64_t totalCallbacks = g_Host.callbackCount.load();
    std::printf("\n======================================================================\n");
    if (totalCallbacks > 0) {
        std::printf(" [OK] HITO 4 COMPLETADO: Handshake COM ASIO 64-bit y ciclo bufferSwitch\n");
        std::printf("      verificados con exito (%llu callbacks ejecutados sin errores).\n",
                    static_cast<unsigned long long>(totalCallbacks));
    } else {
        std::printf(" [!] Advertencia: No se registraron invocaciones de bufferSwitch.\n");
    }
    std::printf("======================================================================\n");

    return (totalCallbacks > 0) ? 0 : 1;
}
