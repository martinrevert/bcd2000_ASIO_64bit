/**
 * @file hostsample_midi.cpp
 * @brief WinMM MIDI In/Out verification utility for Behringer BCD2000 (x64).
 *
 * Enumerates standard Windows MME MIDI ports via winmm.dll, opens
 * "Behringer BCD2000 MIDI" for bidirectional I/O, blinks panel LEDs via
 * midiOutShortMsg, and prints incoming fader/knob/button events.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {

std::atomic<unsigned long> g_rxCount{0};

void CALLBACK MidiInProc(HMIDIIN /*hMidiIn*/,
                         UINT wMsg,
                         DWORD_PTR /*dwInstance*/,
                         DWORD_PTR dwParam1,
                         DWORD_PTR dwParam2) {
    if (wMsg == MIM_DATA) {
        const DWORD msg = static_cast<DWORD>(dwParam1);
        const DWORD ts = static_cast<DWORD>(dwParam2);
        const unsigned status = msg & 0xFFu;
        const unsigned d1 = (msg >> 8) & 0xFFu;
        const unsigned d2 = (msg >> 16) & 0xFFu;
        const unsigned long idx = ++g_rxCount;
        std::printf("  [MIDI IN #%lu] t=%5lums | raw=0x%06lX | st=0x%02X d1=%3u d2=%3u\n",
                    idx, ts, msg & 0xFFFFFFul, status, d1, d2);
        std::fflush(stdout);
    }
}

} // namespace

int wmain(int argc, wchar_t *argv[]) {
    int durationSec = 3;
    for (int i = 1; i < argc; ++i) {
        if ((std::wcscmp(argv[i], L"--duration") == 0 ||
             std::wcscmp(argv[i], L"-d") == 0) &&
            i + 1 < argc) {
            durationSec = std::max(1, static_cast<int>(std::wcstol(argv[++i], nullptr, 10)));
        }
    }

    std::printf("======================================================================\n");
    std::printf("   Behringer BCD2000 WinMM MIDI Verification (hostsample_midi - x64)\n");
    std::printf("======================================================================\n");

    const UINT numIn = midiInGetNumDevs();
    const UINT numOut = midiOutGetNumDevs();
    std::printf("[WinMM] MIDI IN ports: %u | MIDI OUT ports: %u\n", numIn, numOut);

    UINT bcdInId = UINT_MAX;
    for (UINT i = 0; i < numIn; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            std::wprintf(L"  IN  [%u]: %ls (wMid=0x%04X wPid=0x%04X)\n",
                         i, caps.szPname, caps.wMid, caps.wPid);
            if (std::wcsstr(caps.szPname, L"BCD2000") != nullptr && bcdInId == UINT_MAX) {
                bcdInId = i;
            }
        }
    }

    UINT bcdOutId = UINT_MAX;
    for (UINT i = 0; i < numOut; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            std::wprintf(L"  OUT [%u]: %ls (wMid=0x%04X wPid=0x%04X)\n",
                         i, caps.szPname, caps.wMid, caps.wPid);
            if (std::wcsstr(caps.szPname, L"BCD2000") != nullptr && bcdOutId == UINT_MAX) {
                bcdOutId = i;
            }
        }
    }

    if (bcdInId == UINT_MAX || bcdOutId == UINT_MAX) {
        std::printf("\n[!] 'Behringer BCD2000 MIDI' no figura aun en WinMM.\n");
        std::printf("    Ejecute scripts\\install_driver.ps1 como Administrador para registrarlo.\n");
        return 1;
    }

    HMIDIOUT hOut = nullptr;
    MMRESULT mrOut = midiOutOpen(&hOut, bcdOutId, 0, 0, CALLBACK_NULL);
    if (mrOut != MMSYSERR_NOERROR) {
        std::printf("[ERR] midiOutOpen fallo (MMRESULT=%u)\n", mrOut);
        return 1;
    }

    HMIDIIN hIn = nullptr;
    MMRESULT mrIn = midiInOpen(
        &hIn, bcdInId,
        reinterpret_cast<DWORD_PTR>(&MidiInProc),
        0,
        CALLBACK_FUNCTION);
    if (mrIn != MMSYSERR_NOERROR) {
        std::printf("[ERR] midiInOpen fallo (MMRESULT=%u)\n", mrIn);
        midiOutClose(hOut);
        return 1;
    }

    midiInStart(hIn);
    std::printf("\n[MIDI] Puertos IN/OUT abiertos. Enviando secuencia de prueba de LEDs...\n");

    const BYTE testLeds[4] = {0x12, 0x0A, 0x13, 0x0B}; // Play A, Play B, PFL A, PFL B
    for (BYTE cc : testLeds) {
        const DWORD msgOn = 0xB0u | (static_cast<DWORD>(cc) << 8) | (0x7Fu << 16);
        midiOutShortMsg(hOut, msgOn);
        Sleep(100);
    }

    std::printf("[MIDI] Escuchando controles fisicos durante %d s (mueve faders/knobs/jog)...\n",
                durationSec);
    for (int s = 0; s < durationSec * 10; ++s) {
        Sleep(100);
    }

    for (BYTE cc : testLeds) {
        const DWORD msgOff = 0xB0u | (static_cast<DWORD>(cc) << 8);
        midiOutShortMsg(hOut, msgOff);
    }

    midiInStop(hIn);
    midiInReset(hIn);
    midiInClose(hIn);
    midiOutClose(hOut);

    std::printf("\n[OK] Verificacion WinMM MIDI finalizada. Mensajes IN recibidos: %lu\n",
                g_rxCount.load());
    return 0;
}
