/**
 * @file bcd2000_diag_audio.cpp
 * @brief Milestone 3 Diagnostic Tool: Isochronous PCM Audio Streaming &
 *        440 Hz Sine Wave Synthesis for Behringer BCD2000 (VID 0x1397, PID 0x00BD)
 *        over WinUSB x64.
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
 * Implements the hardware PCM audio streaming protocol of the Behringer BCD2000
 * (verified against Linux kernel snd-bcd2000 audio/audio.c & audio/audio.h):
 *  - Audio Format: 16-bit Signed Little-Endian PCM (S16_LE, int16_t)
 *  - Sample Rate: 44,100 Hz (44.1 kHz)
 *  - Channels: 4 Interleaved Channels (8 bytes per multi-channel frame):
 *      * Channels 1 & 2 (Index 0, 1): Master Output Left / Right (Rear RCA)
 *      * Channels 3 & 4 (Index 2, 3): Headphones / Monitor Left / Right (Front 1/4" TRS)
 *  - USB Playback Endpoint: Endpoint 0x02 (Isochronous OUT)
 *  - USB Packet Geometry: 360 bytes per 1 ms USB 1.1 Full-Speed frame (45 frames/packet),
 *    16 packets per URB (5,760 bytes = 720 frames per URB), 6 concurrent overlapped URBs.
 *  - Thread Scheduling: MMCSS "Pro Audio" / THREAD_PRIORITY_TIME_CRITICAL to prevent
 *    buffer underruns or temporal discontinuities.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <avrt.h>
#include <conio.h>
#include <initguid.h>
#include <setupapi.h>
#include <usb.h>
#include <winusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "avrt.lib")

// Generic WinUSB Device Interface GUID (Zadig / WinUSB default)
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB_GENERIC, 0xdee824ef, 0x729b, 0x4a0e, 0x9c,
            0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);

// Complete 52-byte initialization handshake from Linux kernel sound/usb/bcd2000/bcd2000.c
static const uint8_t BCD2000_INIT_SEQUENCE[52] = {
    0x07, 0x00, 0x00, 0x00, 0x78, 0x48, 0x1c, 0x81, 0xc4, 0x00, 0x00,
    0x00, 0x5e, 0x53, 0x4a, 0xa7, 0x18, 0xfa, 0x11, 0xff, 0x6c, 0xf3,
    0x90, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x18,
    0xfa, 0x11, 0xff, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf2, 0x34, 0x4a, 0xf7, 0x18, 0xfa, 0x11, 0xff};

// ============================================================================
// Constants & Audio Geometry (matching snd-bcd2000 audio.h)
// ============================================================================

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kNominalSampleRate = 44100.0;
constexpr size_t kNumChannels = 4;                         // Ch 1-2 Master, Ch 3-4 Phones
constexpr size_t kBytesPerSample = sizeof(int16_t);        // 16-bit signed LE (2 bytes)
constexpr size_t kBytesPerFrame = kNumChannels * kBytesPerSample; // 8 bytes per 4-ch frame

constexpr size_t kDefaultUsbPacketSize = 360;              // 360 B = 45 frames of 8 B
constexpr size_t kPacketsPerUrb = 16;                      // 16 packets = 5,760 B (720 frames)
constexpr size_t kNumUrbs = 6;                             // 6 concurrent URBs in flight (~96 ms)

// Hardware LED Control Change numbers (0xB0, CC 0x00..0x19) verified in Milestone 2
constexpr uint8_t kLedPlayA = 0x12; // Deck A Play/Pause LED
constexpr uint8_t kLedPlayB = 0x0A; // Deck B Play/Pause LED
constexpr uint8_t kLedPflA  = 0x02; // Phones Cue A (PFL A) LED
constexpr uint8_t kLedPflB  = 0x01; // Phones Cue B (PFL B) LED

// ============================================================================
// Global Runtime State & CLI Configuration
// ============================================================================

std::atomic<bool> g_Running{true};

enum class RoutingMode : int {
  AllChannels = 0, // Ch 1-2 Master RCA + Ch 3-4 Headphones (same tone)
  MasterOnly  = 1, // Ch 1-2 Master RCA only (Ch 3-4 silent)
  PhonesOnly  = 2, // Ch 3-4 Headphones only (Ch 1-2 silent)
  DualTone    = 3  // Ch 1-2 = base frequency (440 Hz), Ch 3-4 = octave (880 Hz)
};

struct AudioTestConfig {
  double frequencyHz = 440.0;
  double durationSec = 10.0;
  double amplitude = 0.75; // -2.5 dBFS clean headroom
  RoutingMode routingMode = RoutingMode::AllChannels;
  bool exact44100 = false; // Variable 1-packet URBs (e.g. 9x352 B + 1x360 B / 10 ms)
  double exactRateHz = kNominalSampleRate;
  size_t exactUrbs = 64;
  bool quietUi = false;
  bool syncFromIn = false; // Measure & lock OUT rate dynamically from EP 0x83 IN
};

struct BcdUsbAudioContext {
  HANDLE hDevice = INVALID_HANDLE_VALUE;
  WINUSB_INTERFACE_HANDLE hInterface0 = nullptr;
  WINUSB_INTERFACE_HANDLE hMidiInterface = nullptr;
  WINUSB_INTERFACE_HANDLE hAudioInterface = nullptr;
  std::vector<WINUSB_INTERFACE_HANDLE> allInterfaces;

  UCHAR midiOutPipe = 0x01;
  UCHAR midiInPipe = 0x81;

  UCHAR audioOutPipe = 0x02;
  UCHAR audioInPipe = 0x83;
  UCHAR audioAltSetting = 0;
  USBD_PIPE_TYPE audioOutPipeType = UsbdPipeTypeIsochronous;
  ULONG audioOutMaxPacketSize = kDefaultUsbPacketSize;
  ULONG audioOutBytesPerInterval = kDefaultUsbPacketSize;
  UCHAR audioOutInterval = 1;
};

// ============================================================================
// Helper Formatting Utilities
// ============================================================================

std::string WideToUtf8(const std::wstring &wstr) {
  if (wstr.empty())
    return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                                 static_cast<int>(wstr.size()), nullptr, 0,
                                 nullptr, nullptr);
  std::string result(size, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                      &result[0], size, nullptr, nullptr);
  return result;
}

bool ContainsInsensitive(const std::wstring &text, const std::wstring &target) {
  auto it = std::search(
      text.begin(), text.end(), target.begin(), target.end(),
      [](wchar_t ch1, wchar_t ch2) { return towlower(ch1) == towlower(ch2); });
  return it != text.end();
}

std::string HexByte(uint8_t b) {
  std::ostringstream oss;
  oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
      << static_cast<int>(b);
  return oss.str();
}

std::string FormatWin32Error(DWORD errorCode) {
  LPWSTR buffer = nullptr;
  DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::ostringstream oss;
  oss << "0x" << std::hex << errorCode << std::dec;
  if (size > 0 && buffer) {
    std::wstring wmsg(buffer, size);
    LocalFree(buffer);
    while (!wmsg.empty() && (wmsg.back() == L'\r' || wmsg.back() == L'\n' ||
                             wmsg.back() == L' ')) {
      wmsg.pop_back();
    }
    oss << " (" << WideToUtf8(wmsg) << ")";
  }
  return oss.str();
}

const char *RoutingModeToString(RoutingMode mode) {
  switch (mode) {
  case RoutingMode::AllChannels:
    return "Ch 1-2 (Master RCA) + Ch 3-4 (Phones)";
  case RoutingMode::MasterOnly:
    return "Solo Ch 1-2 (Master RCA)";
  case RoutingMode::PhonesOnly:
    return "Solo Ch 3-4 (Phones)";
  case RoutingMode::DualTone:
    return "Dual-Tone (Master=f0, Phones=2*f0)";
  default:
    return "All Channels";
  }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
      signal == CTRL_CLOSE_EVENT) {
    g_Running.store(false);
    return TRUE;
  }
  return FALSE;
}

// ============================================================================
// Control / LED Helpers (Endpoint 0x01 OUT & 0x81 IN)
// ============================================================================

bool SendBcdSingleMidi(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                       uint8_t status, uint8_t data1, uint8_t data2) {
  if (!hWinUsb || outPipeId == 0)
    return false;

  uint8_t frame[64] = {0};
  frame[0] = 0x03;
  frame[1] = 0x00;
  frame[2] = 0x03;
  frame[3] = status;
  frame[4] = data1;
  frame[5] = data2;

  ULONG bytesWritten = 0;
  BOOL ok = WinUsb_WritePipe(hWinUsb, outPipeId, frame, sizeof(frame),
                             &bytesWritten, nullptr);
  if (!ok) {
    WinUsb_ResetPipe(hWinUsb, outPipeId);
    Sleep(10);
    ok = WinUsb_WritePipe(hWinUsb, outPipeId, frame, sizeof(frame),
                          &bytesWritten, nullptr);
  }
  return ok && bytesWritten == sizeof(frame);
}

void SetAudioStatusLeds(const BcdUsbAudioContext &ctx, bool active) {
  uint8_t val = active ? 0x7F : 0x00;
  const uint8_t leds[4] = {kLedPlayA, kLedPlayB, kLedPflA, kLedPflB};
  for (uint8_t cc : leds) {
    SendBcdSingleMidi(ctx.hMidiInterface, ctx.midiOutPipe, 0xB0, cc, val);
    Sleep(12);
  }
}

/**
 * @brief Drains any pending startup bytes from Endpoint 0x81 IN after handshake
 *        so the 8051 internal buffer is clean before starting isochronous audio.
 */
void DrainInitialControlInPipe(const BcdUsbAudioContext &ctx) {
  if (!ctx.hMidiInterface || ctx.midiInPipe == 0)
    return;

  ULONG shortTimeoutMs = 40;
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiInPipe,
                       PIPE_TRANSFER_TIMEOUT, sizeof(shortTimeoutMs),
                       &shortTimeoutMs);

  uint8_t buf[64];
  for (int i = 0; i < 8; ++i) {
    ULONG bytesRead = 0;
    if (!WinUsb_ReadPipe(ctx.hMidiInterface, ctx.midiInPipe, buf, sizeof(buf),
                         &bytesRead, nullptr)) {
      break;
    }
  }
}

// ============================================================================
// SetupAPI & WinUSB Device Discovery + Handshake
// ============================================================================

bool InitializeBcdAudioDevice(BcdUsbAudioContext &ctx) {
  ctx.hDevice = INVALID_HANDLE_VALUE;
  ctx.hInterface0 = nullptr;
  ctx.hMidiInterface = nullptr;
  ctx.hAudioInterface = nullptr;
  ctx.allInterfaces.clear();

  std::cout << "[USB] Escaneando dispositivos USB mediante SetupAPI para VID "
               "1397 / PID 00BD...\n";

  HDEVINFO hIntfInfo =
      SetupDiGetClassDevsW(&GUID_DEVINTERFACE_WINUSB_GENERIC, nullptr, nullptr,
                           DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

  std::wstring targetPath;
  const std::wstring vidStr = L"vid_1397";
  const std::wstring pidStr = L"pid_00bd";

  if (hIntfInfo != INVALID_HANDLE_VALUE) {
    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(
             hIntfInfo, nullptr, &GUID_DEVINTERFACE_WINUSB_GENERIC, idx,
             &ifData);
         ++idx) {
      DWORD reqSize = 0;
      SetupDiGetDeviceInterfaceDetailW(hIntfInfo, &ifData, nullptr, 0, &reqSize,
                                       nullptr);
      if (reqSize == 0)
        continue;

      std::vector<BYTE> buf(reqSize);
      auto detailData =
          reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
      detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

      if (SetupDiGetDeviceInterfaceDetailW(hIntfInfo, &ifData, detailData,
                                           reqSize, nullptr, nullptr)) {
        std::wstring path(detailData->DevicePath);
        if (ContainsInsensitive(path, vidStr) &&
            ContainsInsensitive(path, pidStr)) {
          targetPath = path;
          break;
        }
      }
    }
    SetupDiDestroyDeviceInfoList(hIntfInfo);
  }

  if (targetPath.empty()) {
    std::cerr << "  [-] No se encontro la Behringer BCD2000 con driver WinUSB "
                 "activo.\n";
    return false;
  }

  std::cout << "  [+] Dispositivo localizado: " << WideToUtf8(targetPath)
            << "\n";

  ctx.hDevice =
      CreateFileW(targetPath.c_str(), GENERIC_WRITE | GENERIC_READ,
                  FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);

  if (ctx.hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "  [-] Error en CreateFileW: "
              << FormatWin32Error(GetLastError()) << "\n";
    return false;
  }

  if (!WinUsb_Initialize(ctx.hDevice, &ctx.hInterface0)) {
    std::cerr << "  [-] Error en WinUsb_Initialize: "
              << FormatWin32Error(GetLastError()) << "\n";
    CloseHandle(ctx.hDevice);
    ctx.hDevice = INVALID_HANDLE_VALUE;
    return false;
  }

  ctx.allInterfaces.push_back(ctx.hInterface0);

  for (UCHAR assocIdx = 0; assocIdx < 4; ++assocIdx) {
    WINUSB_INTERFACE_HANDLE hAssoc = nullptr;
    if (WinUsb_GetAssociatedInterface(ctx.hInterface0, assocIdx, &hAssoc)) {
      ctx.allInterfaces.push_back(hAssoc);
    }
  }

  ULONG deviceSpeed = 0;
  ULONG speedLen = sizeof(deviceSpeed);
  if (WinUsb_QueryDeviceInformation(ctx.hInterface0, DEVICE_SPEED, &speedLen,
                                    &deviceSpeed)) {
    const char *speedStr = (deviceSpeed == FullSpeed)
                               ? "USB 1.1 Full-Speed (12 Mbps)"
                               : (deviceSpeed == HighSpeed ? "USB 2.0 High-Speed"
                                                           : "Low-Speed");
    std::cout << "  [+] Velocidad de bus USB reportada: " << speedStr << "\n";
  }

  bool foundAudioOut = false;

  for (size_t ifIdx = 0; ifIdx < ctx.allInterfaces.size(); ++ifIdx) {
    WINUSB_INTERFACE_HANDLE hIf = ctx.allInterfaces[ifIdx];

    for (UCHAR altSetting = 0; altSetting < 4; ++altSetting) {
      USB_INTERFACE_DESCRIPTOR ifDesc{};
      if (!WinUsb_QueryInterfaceSettings(hIf, altSetting, &ifDesc)) {
        break;
      }

      std::cout << "      -> Interfaz #" << ifIdx << " (bInterfaceNumber="
                << static_cast<int>(ifDesc.bInterfaceNumber)
                << ", AltSetting=" << static_cast<int>(altSetting)
                << ", Endpoints=" << static_cast<int>(ifDesc.bNumEndpoints)
                << ")\n";

      for (UCHAR pIdx = 0; pIdx < ifDesc.bNumEndpoints; ++pIdx) {
        WINUSB_PIPE_INFORMATION_EX pipeInfoEx{};
        WINUSB_PIPE_INFORMATION pipeInfo{};
        bool hasEx = WinUsb_QueryPipeEx(hIf, altSetting, pIdx, &pipeInfoEx);
        if (!hasEx) {
          if (!WinUsb_QueryPipe(hIf, altSetting, pIdx, &pipeInfo)) {
            continue;
          }
          pipeInfoEx.PipeType = pipeInfo.PipeType;
          pipeInfoEx.PipeId = pipeInfo.PipeId;
          pipeInfoEx.MaximumPacketSize = pipeInfo.MaximumPacketSize;
          pipeInfoEx.Interval = pipeInfo.Interval;
          pipeInfoEx.MaximumBytesPerInterval = pipeInfo.MaximumPacketSize;
        }

        bool isIn = USB_ENDPOINT_DIRECTION_IN(pipeInfoEx.PipeId);
        std::string pType = "Unknown";
        if (pipeInfoEx.PipeType == UsbdPipeTypeControl)
          pType = "Control";
        else if (pipeInfoEx.PipeType == UsbdPipeTypeIsochronous)
          pType = "Isochronous";
        else if (pipeInfoEx.PipeType == UsbdPipeTypeBulk)
          pType = "Bulk";
        else if (pipeInfoEx.PipeType == UsbdPipeTypeInterrupt)
          pType = "Interrupt";

        std::cout << "         Pipe 0x" << HexByte(pipeInfoEx.PipeId) << " ["
                  << (isIn ? "IN " : "OUT") << "] Tipo: " << std::setw(12)
                  << std::setfill(' ') << std::left << pType << std::right
                  << " MaxPacket: " << std::setw(3)
                  << pipeInfoEx.MaximumPacketSize << " B"
                  << " | BytesPerInterval: " << std::setw(3)
                  << pipeInfoEx.MaximumBytesPerInterval << " B"
                  << " | Interval: " << static_cast<int>(pipeInfoEx.Interval)
                  << " ms\n";

        if (pipeInfoEx.PipeId == 0x01) {
          ctx.hMidiInterface = hIf;
          ctx.midiOutPipe = 0x01;
        } else if (pipeInfoEx.PipeId == 0x81) {
          ctx.hMidiInterface = hIf;
          ctx.midiInPipe = 0x81;
        } else if (pipeInfoEx.PipeId == 0x02 ||
                   (!isIn && pipeInfoEx.PipeType == UsbdPipeTypeIsochronous)) {
          if (!foundAudioOut || pipeInfoEx.MaximumPacketSize > 0) {
            ctx.hAudioInterface = hIf;
            ctx.audioOutPipe = pipeInfoEx.PipeId;
            ctx.audioAltSetting = altSetting;
            ctx.audioOutPipeType = pipeInfoEx.PipeType;
            ctx.audioOutMaxPacketSize = pipeInfoEx.MaximumPacketSize;
            ctx.audioOutBytesPerInterval =
                (pipeInfoEx.MaximumBytesPerInterval > 0)
                    ? pipeInfoEx.MaximumBytesPerInterval
                    : pipeInfoEx.MaximumPacketSize;
            ctx.audioOutInterval = pipeInfoEx.Interval;
            foundAudioOut = true;
          }
        } else if (pipeInfoEx.PipeId == 0x83 ||
                   (isIn && pipeInfoEx.PipeType == UsbdPipeTypeIsochronous)) {
          ctx.audioInPipe = pipeInfoEx.PipeId;
        }
      }
    }
  }

  if (!ctx.hMidiInterface) {
    ctx.hMidiInterface = ctx.hInterface0;
  }
  if (!ctx.hAudioInterface) {
    ctx.hAudioInterface = ctx.hInterface0;
  }
  if (ctx.audioOutBytesPerInterval == 0) {
    ctx.audioOutBytesPerInterval = kDefaultUsbPacketSize;
  }
  if (ctx.audioOutMaxPacketSize == 0) {
    ctx.audioOutMaxPacketSize = kDefaultUsbPacketSize;
  }

  // Activate the alternate setting for the audio interface so WinUSB.sys
  // initializes isochronous pipe tracking.
  if (!WinUsb_SetCurrentAlternateSetting(ctx.hAudioInterface,
                                         ctx.audioAltSetting)) {
    DWORD altErr = GetLastError();
    std::cout << "  [!] Nota en WinUsb_SetCurrentAlternateSetting(Alt="
              << static_cast<int>(ctx.audioAltSetting)
              << "): " << FormatWin32Error(altErr) << "\n";
  }

  // Configure Control/MIDI pipe policies
  ULONG timeoutMs = 500;
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiOutPipe,
                       PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
  UCHAR autoClearStall = 1;
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiOutPipe, AUTO_CLEAR_STALL,
                       sizeof(autoClearStall), &autoClearStall);
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiInPipe, AUTO_CLEAR_STALL,
                       sizeof(autoClearStall), &autoClearStall);

  // Send 52-byte hardware initialization sequence to Endpoint 0x01
  std::cout << "\n[HANDSHAKE] Transmitiendo trama de inicializacion de 52 "
               "bytes a EP 0x"
            << HexByte(ctx.midiOutPipe) << "...\n";
  ULONG bytesWritten = 0;
  BOOL ok =
      WinUsb_WritePipe(ctx.hMidiInterface, ctx.midiOutPipe,
                       const_cast<PUCHAR>(BCD2000_INIT_SEQUENCE),
                       sizeof(BCD2000_INIT_SEQUENCE), &bytesWritten, nullptr);

  if (!ok || bytesWritten != sizeof(BCD2000_INIT_SEQUENCE)) {
    std::cerr << "  [-] Error en handshake: "
              << FormatWin32Error(GetLastError()) << "\n";
    return false;
  }

  std::cout << "  [+] Handshake exitoso (" << bytesWritten
            << " bytes). Subsistema de control y DAC activos.\n";

  Sleep(100);
  DrainInitialControlInPipe(ctx);

  // Turn on Play A/B & PFL A/B LEDs to visually indicate active audio playback
  SetAudioStatusLeds(ctx, true);
  std::cout << "  [+] Indicadores LED (Play A/B + PFL A/B) encendidos.\n";

  return true;
}

// ============================================================================
// Sine Wave PCM Synthesis Engine (16-bit Signed LE, 4 Interleaved Channels)
// ============================================================================

struct SineSynthesizerState {
  double phaseMaster = 0.0;
  double phasePhones = 0.0;
  uint64_t totalFramesSynthesized = 0;
};

/**
 * @brief Synthesizes `numFrames` of 4-channel 16-bit signed little-endian PCM
 *        into `outBuffer`.
 *
 * Frame layout (8 bytes per frame, S16_LE):
 *   int16_t ch[0] = Master Left  (RCA L)
 *   int16_t ch[1] = Master Right (RCA R)
 *   int16_t ch[2] = Phones Left  (Headphones L)
 *   int16_t ch[3] = Phones Right (Headphones R)
 */
void FillPcmSineBuffer(uint8_t *outBuffer, size_t bufferBytes,
                       SineSynthesizerState &synth, double frequencyHz,
                       double amplitude, RoutingMode routingMode,
                       uint64_t totalTargetFrames, bool stoppingFadeOut,
                       double sampleRateHz = kNominalSampleRate) {
  size_t numFrames = bufferBytes / kBytesPerFrame;
  int16_t *samples = reinterpret_cast<int16_t *>(outBuffer);

  // Use the active sample rate for phase progression
  const double activeRate = (sampleRateHz > 0.0) ? sampleRateHz : kNominalSampleRate;
  const double phaseStepMaster = (kTwoPi * frequencyHz) / activeRate;
  const double phaseStepPhones =
      (routingMode == RoutingMode::DualTone)
          ? ((kTwoPi * (frequencyHz * 2.0)) / activeRate)
          : phaseStepMaster;

  // Smooth 50 ms (2,205 frames) fade-in and fade-out envelope to prevent clicks
  constexpr uint64_t kFadeFrames = 2205;

  for (size_t f = 0; f < numFrames; ++f) {
    uint64_t frameIdx = synth.totalFramesSynthesized;
    double envelope = 1.0;

    if (frameIdx < kFadeFrames) {
      // Smooth raised-cosine fade-in at stream startup
      double t = static_cast<double>(frameIdx) / static_cast<double>(kFadeFrames);
      envelope = 0.5 * (1.0 - std::cos(t * 3.14159265358979323846));
    } else if (stoppingFadeOut) {
      // Smooth fade-out on early user stop
      double t =
          static_cast<double>(f) / static_cast<double>( std::max<size_t>(numFrames, 1));
      envelope = 0.5 * (1.0 + std::cos(t * 3.14159265358979323846));
    } else if (totalTargetFrames > 0 &&
               frameIdx + kFadeFrames >= totalTargetFrames) {
      if (frameIdx >= totalTargetFrames) {
        envelope = 0.0;
      } else {
        uint64_t remaining = totalTargetFrames - frameIdx;
        double t =
            static_cast<double>(remaining) / static_cast<double>(kFadeFrames);
        envelope = 0.5 * (1.0 - std::cos(t * 3.14159265358979323846));
      }
    }

    double gain = std::clamp(amplitude, 0.0, 0.98) * envelope * 32767.0;

    int16_t masterSample =
        static_cast<int16_t>(std::lrint(std::sin(synth.phaseMaster) * gain));
    int16_t phonesSample =
        (routingMode == RoutingMode::DualTone)
            ? static_cast<int16_t>(
                  std::lrint(std::sin(synth.phasePhones) * gain))
            : masterSample;

    synth.phaseMaster += phaseStepMaster;
    if (synth.phaseMaster >= kTwoPi) {
      synth.phaseMaster -= kTwoPi;
    }

    synth.phasePhones += phaseStepPhones;
    if (synth.phasePhones >= kTwoPi) {
      synth.phasePhones -= kTwoPi;
    }

    int16_t ch1 = 0, ch2 = 0, ch3 = 0, ch4 = 0;
    switch (routingMode) {
    case RoutingMode::AllChannels:
    case RoutingMode::DualTone:
      ch1 = masterSample;
      ch2 = masterSample;
      ch3 = phonesSample;
      ch4 = phonesSample;
      break;
    case RoutingMode::MasterOnly:
      ch1 = masterSample;
      ch2 = masterSample;
      ch3 = 0;
      ch4 = 0;
      break;
    case RoutingMode::PhonesOnly:
      ch1 = 0;
      ch2 = 0;
      ch3 = phonesSample;
      ch4 = phonesSample;
      break;
    }

    samples[f * 4 + 0] = ch1; // Channel 1: Master Left (RCA)
    samples[f * 4 + 1] = ch2; // Channel 2: Master Right (RCA)
    samples[f * 4 + 2] = ch3; // Channel 3: Headphones Left (1/4" TRS)
    samples[f * 4 + 3] = ch4; // Channel 4: Headphones Right (1/4" TRS)

    synth.totalFramesSynthesized++;
  }

  // Zero any trailing bytes if packetSize is not a multiple of 8
  size_t validBytes = numFrames * kBytesPerFrame;
  if (validBytes < bufferBytes) {
    std::memset(outBuffer + validBytes, 0, bufferBytes - validBytes);
  }
}

// ============================================================================
// Continuous Overlapped Isochronous Audio Streaming Loop (Endpoint 0x02 OUT)
// ============================================================================

struct AudioUrbSlot {
  OVERLAPPED overlapped{};
  ULONG offset = 0;
  ULONG length = 0;
  bool inFlight = false;
};

int RunAudioStreamingTest(BcdUsbAudioContext &ctx, AudioTestConfig &cfg) {
  const ULONG packetSize = ctx.audioOutBytesPerInterval;
  const bool useExact44100 = cfg.exact44100 && (packetSize >= 360);
  const size_t numUrbs = useExact44100 ? std::clamp<size_t>(cfg.exactUrbs, 8, 256) : kNumUrbs;
  const size_t packetsPerUrb = useExact44100 ? 1 : kPacketsPerUrb;
  const ULONG slotStride =
      useExact44100 ? packetSize : static_cast<ULONG>(packetsPerUrb * packetSize);
  const ULONG totalRingBufferSize = static_cast<ULONG>(numUrbs * slotStride);
  const size_t framesPerSlot = slotStride / kBytesPerFrame;

  // Effective USB frame rate:
  //  - Standard WinUSB multi-packet mode sends BytesPerInterval/8 frames per 1 ms packet.
  //  - Exact mode paces single-packet URBs via fractional accumulator targeting cfg.exactRateHz.
  const double effectiveUsbFramesPerSec =
      useExact44100 ? cfg.exactRateHz
                    : (static_cast<double>(packetSize / kBytesPerFrame) * 1000.0);
  const uint64_t totalTargetFrames =
      (cfg.durationSec > 0.0)
          ? static_cast<uint64_t>(cfg.durationSec * effectiveUsbFramesPerSec)
          : 0;

  std::cout << "\n==============================================================="
               "=======\n";
  std::cout << "         PARAMETROS DE STREAMING DE AUDIO PCM (ENDPOINT 0x"
            << HexByte(ctx.audioOutPipe) << ")         \n";
  std::cout << "==============================================================="
               "=======\n";
  std::cout << "  Formato PCM          : 16-bit Signed Little-Endian (S16_LE)\n";
  std::cout << "  Canales Intercalados : 4 canales (Ch 1-2 Master RCA | Ch 3-4 "
               "Auriculares)\n";
  if (useExact44100) {
    std::cout << "  Frecuencia Muestreo  : " << std::fixed
              << std::setprecision(2) << cfg.exactRateHz
              << " Hz EXACTA (1 pkt/URB, lead-45 | MaxPkt: " << packetSize
              << " B)\n";
    std::cout << "  Geometria de Cola    : " << numUrbs
              << " URBs x 1 paquete (cola " << numUrbs << " ms"
              << (cfg.quietUi ? ", modo --quiet" : "") << ")\n";
  } else {
    std::cout << "  Frecuencia Muestreo  : 44100 Hz (Nominal DAC) | Paquete USB: "
              << packetSize << " B/ms (" << (packetSize / kBytesPerFrame)
              << " frames/ms)\n";
    std::cout << "  Geometria de Cola    : " << numUrbs << " URBs x "
              << packetsPerUrb << " paquetes = " << slotStride << " B/URB ("
              << framesPerSlot << " frames / " << packetsPerUrb << " ms)\n";
  }
  std::cout << "  Tono de Prueba       : Senoidal pura a " << std::fixed
            << std::setprecision(1) << cfg.frequencyHz << " Hz (Volumen: "
            << static_cast<int>(cfg.amplitude * 100.0) << "%)\n";
  std::cout << "  Ruteo Activo         : " << RoutingModeToString(cfg.routingMode)
            << "\n";
  std::cout << "  Duracion Objetivo    : "
            << (cfg.durationSec > 0.0
                    ? (std::to_string(static_cast<int>(cfg.durationSec)) +
                       " segundos")
                    : "Continuo (hasta presionar Q)")
            << "\n";
  std::cout << "---------------------------------------------------------------"
               "-------\n";
  std::cout << "  [!] IMPORTANTE EN EL HARDWARE FISICO BCD2000:\n";
  std::cout << "      1. Sube las perillas fisicas [MASTER] y [PHONES] (esquina "
               "superior derecha).\n";
  std::cout << "      2. Atajos en vivo: [1] Todos (Master+Phones) | [2] Solo "
               "Master RCA\n";
  std::cout << "                         [3] Solo Phones           | [4] "
               "Dual-Tone (440/880 Hz)\n";
  std::cout << "                         [+/-] Volumen             | [Q/ESC] "
               "Detener\n";
  std::cout << "==============================================================="
               "=======\n\n";

  // 1. Elevate current thread priority via Windows MMCSS ("Pro Audio")
  DWORD mmcssTaskIndex = 0;
  HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
  if (hMmcss) {
    AvSetMmThreadPriority(hMmcss, AVRT_PRIORITY_CRITICAL);
    std::cout << "[MMCSS] Hilo de streaming registrado en 'Pro Audio' "
                 "(AVRT_PRIORITY_CRITICAL).\n";
  } else {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::cout << "[MMCSS] Prioridad elevada a THREAD_PRIORITY_TIME_CRITICAL.\n";
  }

  // 2. Allocate contiguous ring buffer for all URBs
  std::vector<uint8_t> ringBuffer(totalRingBufferSize, 0);

  // 3. Register Isochronous Buffer if pipe is Isochronous
  WINUSB_ISOCH_BUFFER_HANDLE hIsochBuffer = nullptr;
  const bool isIsoch = (ctx.audioOutPipeType == UsbdPipeTypeIsochronous);

  if (isIsoch) {
    WinUsb_ResetPipe(ctx.hAudioInterface, ctx.audioOutPipe);
    if (!WinUsb_RegisterIsochBuffer(ctx.hAudioInterface, ctx.audioOutPipe,
                                    ringBuffer.data(), totalRingBufferSize,
                                    &hIsochBuffer)) {
      DWORD regErr = GetLastError();
      std::cerr << "  [-] Error en WinUsb_RegisterIsochBuffer (EP 0x"
                << HexByte(ctx.audioOutPipe)
                << "): " << FormatWin32Error(regErr) << "\n";
      if (hMmcss)
        AvRevertMmThreadCharacteristics(hMmcss);
      return 2;
    }
    std::cout << "[ISOCH] Buffer DMA isocrono registrado en WinUSB ("
              << totalRingBufferSize << " bytes, " << numUrbs << " URBs).\n\n";
  }

  // 4. Initialize URB slots and Manual-Reset Overlapped Events
  std::vector<AudioUrbSlot> urbs(numUrbs);
  std::vector<HANDLE> waitEvents(numUrbs, nullptr);

  for (size_t i = 0; i < numUrbs; ++i) {
    urbs[i].offset = static_cast<ULONG>(i * slotStride);
    urbs[i].length = slotStride;
    urbs[i].overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    waitEvents[i] = urbs[i].overlapped.hEvent;
  }

  SineSynthesizerState synth{};
  // Start with 0.9 accumulator so exact 44.1 kHz mode leads with a 45-frame (360 B)
  // packet, keeping positive FIFO margin instead of starting with a deficit.
  double exactFrameAccumulator = 0.9;
  const double exactFramesPerMs = effectiveUsbFramesPerSec / 1000.0;
  const size_t maxFramesPerPacket = packetSize / kBytesPerFrame;

  // Optional EP 0x83 IN hardware clock reader thread (Implicit Feedback)
  std::atomic<bool> inSyncRunning{false};
  std::atomic<bool> inClockLocked{false};
  std::atomic<double> inFramesPerMs{44.1};
  std::atomic<uint64_t> totalInFrames{0};
  std::atomic<uint64_t> totalInPackets{0};
  std::atomic<ULONG> minInPktBytes{9999};
  std::atomic<ULONG> maxInPktBytes{0};
  std::thread inSyncThread;

  if (cfg.syncFromIn && isIsoch && ctx.audioInPipe != 0) {
    inSyncRunning.store(true, std::memory_order_release);
    inSyncThread = std::thread([&]() {
      DWORD inTaskIdx = 0;
      HANDLE hInMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &inTaskIdx);
      if (hInMmcss) {
        AvSetMmThreadPriority(hInMmcss, AVRT_PRIORITY_CRITICAL);
      }

      constexpr size_t kInUrbs = 4;
      constexpr size_t kInPktsPerUrb = 16;
      const ULONG inSlotBytes = static_cast<ULONG>(kInPktsPerUrb * packetSize);
      std::vector<uint8_t> inBuf(kInUrbs * inSlotBytes, 0);
      WINUSB_ISOCH_BUFFER_HANDLE hInIsoch = nullptr;

      WinUsb_ResetPipe(ctx.hAudioInterface, ctx.audioInPipe);
      if (!WinUsb_RegisterIsochBuffer(ctx.hAudioInterface, ctx.audioInPipe,
                                      inBuf.data(),
                                      static_cast<ULONG>(inBuf.size()),
                                      &hInIsoch)) {
        if (hInMmcss)
          AvRevertMmThreadCharacteristics(hInMmcss);
        return;
      }

      struct InUrb {
        OVERLAPPED ov{};
        std::vector<USBD_ISO_PACKET_DESCRIPTOR> pkts;
        ULONG offset = 0;
        bool inFlight = false;
      };
      std::vector<InUrb> inUrbs(kInUrbs);
      for (size_t i = 0; i < kInUrbs; ++i) {
        inUrbs[i].offset = static_cast<ULONG>(i * inSlotBytes);
        inUrbs[i].pkts.resize(kInPktsPerUrb);
        inUrbs[i].ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      }

      bool continueIn = false;
      auto SubmitIn = [&](size_t idx) -> bool {
        InUrb &s = inUrbs[idx];
        ResetEvent(s.ov.hEvent);
        std::memset(s.pkts.data(), 0,
                    s.pkts.size() * sizeof(USBD_ISO_PACKET_DESCRIPTOR));
        BOOL ok = WinUsb_ReadIsochPipeAsap(
            hInIsoch, s.offset, inSlotBytes, continueIn ? TRUE : FALSE,
            kInPktsPerUrb, s.pkts.data(), &s.ov);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err != ERROR_IO_PENDING && continueIn) {
          ResetEvent(s.ov.hEvent);
          ok = WinUsb_ReadIsochPipeAsap(hInIsoch, s.offset, inSlotBytes, FALSE,
                                        kInPktsPerUrb, s.pkts.data(), &s.ov);
          err = ok ? ERROR_SUCCESS : GetLastError();
        }
        if (ok || err == ERROR_IO_PENDING) {
          continueIn = true;
          s.inFlight = true;
          return true;
        }
        s.inFlight = false;
        return false;
      };

      for (size_t i = 0; i < kInUrbs; ++i) {
        if (!SubmitIn(i)) {
          break;
        }
      }

      uint64_t accFrames = 0;
      uint64_t accPkts = 0;
      size_t reapIdx = 0;
      while (inSyncRunning.load(std::memory_order_acquire)) {
        InUrb &s = inUrbs[reapIdx];
        if (!s.inFlight) {
          break;
        }
        if (WaitForSingleObject(s.ov.hEvent, 200) != WAIT_OBJECT_0) {
          break;
        }
        DWORD dummy = 0;
        BOOL ok = WinUsb_GetOverlappedResult(ctx.hAudioInterface, &s.ov,
                                             &dummy, FALSE);
        s.inFlight = false;
        if (!inSyncRunning.load(std::memory_order_acquire)) {
          break;
        }
        if (ok) {
          for (size_t p = 0; p < kInPktsPerUrb; ++p) {
            const ULONG len = s.pkts[p].Length;
            if (s.pkts[p].Status == 0 && len > 0) {
              accFrames += (len / kBytesPerFrame);
              accPkts++;
              ULONG curMin = minInPktBytes.load(std::memory_order_relaxed);
              while (len < curMin &&
                     !minInPktBytes.compare_exchange_weak(curMin, len)) {
              }
              ULONG curMax = maxInPktBytes.load(std::memory_order_relaxed);
              while (len > curMax &&
                     !maxInPktBytes.compare_exchange_weak(curMax, len)) {
              }
            }
          }
          totalInFrames.store(accFrames, std::memory_order_relaxed);
          totalInPackets.store(accPkts, std::memory_order_relaxed);
          if (accPkts >= 200) {
            inFramesPerMs.store(static_cast<double>(accFrames) /
                                    static_cast<double>(accPkts),
                                std::memory_order_release);
            inClockLocked.store(true, std::memory_order_release);
          }
        } else {
          continueIn = false;
        }

        if (!SubmitIn(reapIdx)) {
          break;
        }
        reapIdx = (reapIdx + 1) % kInUrbs;
      }

      WinUsb_AbortPipe(ctx.hAudioInterface, ctx.audioInPipe);
      for (size_t i = 0; i < kInUrbs; ++i) {
        if (inUrbs[i].inFlight) {
          WaitForSingleObject(inUrbs[i].ov.hEvent, 100);
        }
        if (inUrbs[i].ov.hEvent) {
          CloseHandle(inUrbs[i].ov.hEvent);
        }
      }
      WinUsb_UnregisterIsochBuffer(hInIsoch);
      if (hInMmcss) {
        AvRevertMmThreadCharacteristics(hInMmcss);
      }
    });

    // Briefly warm up EP 0x83 IN clock measurement (~350 ms) before audio starts
    for (int w = 0; w < 35 && !inClockLocked.load(std::memory_order_acquire);
         ++w) {
      Sleep(10);
    }
    if (inClockLocked.load(std::memory_order_acquire)) {
      std::cout << "[SYNC-IN] Reloj hardware medido en EP 0x"
                << HexByte(ctx.audioInPipe) << ": " << std::fixed
                << std::setprecision(2)
                << (inFramesPerMs.load(std::memory_order_acquire) * 1000.0)
                << " Hz (rango pkt IN: " << minInPktBytes.load() << ".."
                << maxInPktBytes.load() << " B)\n\n";
    } else {
      std::cout << "[SYNC-IN] Esperando tramas de EP 0x"
                << HexByte(ctx.audioInPipe)
                << " durante streaming...\n\n";
    }
  }

  bool streamContinueFlag = false; // MUST be FALSE on the very first transfer!
  bool useExplicitFrameNumber = false;
  ULONG nextScheduledFrame = 0;

  uint64_t totalUrbsCompleted = 0;
  uint64_t totalPacketsCompleted = 0;
  uint64_t totalBytesTransferred = 0;
  uint64_t totalUnderruns = 0;
  uint64_t totalAsapResets = 0;
  uint64_t gapsOver5ms = 0;
  double maxUrbGapMs = 0.0;
  auto prevReapTime = std::chrono::steady_clock::now();

  auto SubmitAudioUrb = [&](size_t idx) -> bool {
    AudioUrbSlot &slot = urbs[idx];
    ResetEvent(slot.overlapped.hEvent);

    double activeRateHz = effectiveUsbFramesPerSec;
    if (useExact44100) {
      double fpm = exactFramesPerMs;
      if (cfg.syncFromIn && inClockLocked.load(std::memory_order_acquire)) {
        fpm = inFramesPerMs.load(std::memory_order_relaxed);
        activeRateHz = fpm * 1000.0;
      }
      exactFrameAccumulator += fpm;
      size_t pktFrames = static_cast<size_t>(std::floor(exactFrameAccumulator));
      exactFrameAccumulator -= static_cast<double>(pktFrames);
      pktFrames = std::clamp<size_t>(pktFrames, 1, maxFramesPerPacket);
      slot.length = static_cast<ULONG>(pktFrames * kBytesPerFrame);
    } else {
      slot.length = slotStride;
    }

    uint8_t *urbPtr = ringBuffer.data() + slot.offset;
    FillPcmSineBuffer(urbPtr, slot.length, synth, cfg.frequencyHz,
                      cfg.amplitude, cfg.routingMode, totalTargetFrames, false,
                      activeRateHz);

    BOOL submitOk = FALSE;
    DWORD err = ERROR_SUCCESS;

    if (isIsoch) {
      if (!useExplicitFrameNumber) {
        submitOk = WinUsb_WriteIsochPipeAsap(
            hIsochBuffer, slot.offset, slot.length,
            streamContinueFlag ? TRUE : FALSE, &slot.overlapped);
        err = submitOk ? ERROR_SUCCESS : GetLastError();

        // If ContinueStream=TRUE failed because the host schedule momentarily
        // drained, retry immediately with ContinueStream=FALSE to restart the
        // isochronous schedule seamlessly.
        if (!submitOk && err != ERROR_IO_PENDING && streamContinueFlag) {
          totalUnderruns++;
          totalAsapResets++;
          ResetEvent(slot.overlapped.hEvent);
          submitOk = WinUsb_WriteIsochPipeAsap(
              hIsochBuffer, slot.offset, slot.length, FALSE, &slot.overlapped);
          err = submitOk ? ERROR_SUCCESS : GetLastError();
        }

        // Fallback to explicit frame scheduling if host controller rejects ASAP
        if (!submitOk && err != ERROR_IO_PENDING) {
          LARGE_INTEGER ts{};
          ULONG currentFrame = 0;
          if (WinUsb_GetCurrentFrameNumber(ctx.hAudioInterface, &currentFrame,
                                           &ts)) {
            useExplicitFrameNumber = true;
            nextScheduledFrame = currentFrame + 4;
            ResetEvent(slot.overlapped.hEvent);
            submitOk = WinUsb_WriteIsochPipe(
                hIsochBuffer, slot.offset, slot.length, &nextScheduledFrame,
                &slot.overlapped);
            err = submitOk ? ERROR_SUCCESS : GetLastError();
          }
        } else {
          streamContinueFlag = true;
        }
      } else {
        submitOk = WinUsb_WriteIsochPipe(hIsochBuffer, slot.offset, slot.length,
                                         &nextScheduledFrame, &slot.overlapped);
        err = submitOk ? ERROR_SUCCESS : GetLastError();
      }
    } else {
      submitOk = WinUsb_WritePipe(ctx.hAudioInterface, ctx.audioOutPipe, urbPtr,
                                  slot.length, nullptr, &slot.overlapped);
      err = submitOk ? ERROR_SUCCESS : GetLastError();
    }

    if (submitOk || err == ERROR_IO_PENDING) {
      slot.inFlight = true;
      return true;
    }

    std::cerr << "\n  [-] Error al encolar URB #" << idx << ": "
              << FormatWin32Error(err) << "\n";
    slot.inFlight = false;
    return false;
  };

  // 5. Prime the USB Isochronous pipeline with all numUrbs transfers
  auto startTime = std::chrono::steady_clock::now();
  auto lastUiUpdate = startTime;

  for (size_t i = 0; i < numUrbs; ++i) {
    if (!SubmitAudioUrb(i)) {
      g_Running.store(false);
      break;
    }
  }

  size_t nextUrbToReap = 0;

  // 6. Real-Time Streaming Loop (round-robin FIFO harvesting of URBs)
  while (g_Running.load()) {
    AudioUrbSlot &slot = urbs[nextUrbToReap];
    if (!slot.inFlight) {
      break;
    }

    DWORD waitRes = WaitForSingleObject(slot.overlapped.hEvent, 1000);
    if (waitRes == WAIT_TIMEOUT) {
      std::cerr << "\n  [!] Timeout esperando completado de URB #"
                << nextUrbToReap << "\n";
      totalUnderruns++;
      break;
    }
    if (waitRes != WAIT_OBJECT_0) {
      break;
    }

    DWORD bytesTransferred = 0;
    BOOL ovOk = WinUsb_GetOverlappedResult(
        ctx.hAudioInterface, &slot.overlapped, &bytesTransferred, FALSE);
    slot.inFlight = false;

    if (!ovOk) {
      DWORD ovErr = GetLastError();
      if (ovErr == ERROR_DEVICE_NOT_CONNECTED ||
          ovErr == ERROR_DEV_NOT_EXIST) {
        std::cerr << "\n  [-] Dispositivo desconectado durante streaming: "
                  << FormatWin32Error(ovErr) << "\n";
        break;
      }
      // On transient bus glitch, reset continue flag and re-queue
      totalUnderruns++;
      streamContinueFlag = false;
    } else {
      totalUrbsCompleted++;
      totalPacketsCompleted += packetsPerUrb;
      totalBytesTransferred += slot.length;
    }

    // Check if target duration has been reached
    auto now = std::chrono::steady_clock::now();
    if (totalUrbsCompleted > 1) {
      double gapMs =
          std::chrono::duration<double, std::milli>(now - prevReapTime).count();
      if (gapMs > maxUrbGapMs) {
        maxUrbGapMs = gapMs;
      }
      if (gapMs > 5.0 && useExact44100) {
        gapsOver5ms++;
      }
    }
    prevReapTime = now;
    double elapsedSec =
        std::chrono::duration<double>(now - startTime).count();

    if (cfg.durationSec > 0.0 && elapsedSec >= cfg.durationSec) {
      break;
    }

    // Re-fill and immediately re-submit this URB slot
    if (!SubmitAudioUrb(nextUrbToReap)) {
      break;
    }

    nextUrbToReap = (nextUrbToReap + 1) % numUrbs;

    // Handle non-blocking interactive keyboard controls
    if (!cfg.quietUi && _kbhit()) {
      int ch = _getch();
      if (ch == 'q' || ch == 'Q' || ch == 27) {
        std::cout << "\n\n[INFO] Detencion solicitada por el usuario.\n";
        g_Running.store(false);
        break;
      } else if (ch == '1') {
        cfg.routingMode = RoutingMode::AllChannels;
      } else if (ch == '2') {
        cfg.routingMode = RoutingMode::MasterOnly;
      } else if (ch == '3') {
        cfg.routingMode = RoutingMode::PhonesOnly;
      } else if (ch == '4') {
        cfg.routingMode = RoutingMode::DualTone;
      } else if (ch == '+' || ch == '=') {
        cfg.amplitude = std::min(0.95, cfg.amplitude + 0.05);
      } else if (ch == '-' || ch == '_') {
        cfg.amplitude = std::max(0.05, cfg.amplitude - 0.05);
      }
    }

    // Update live console telemetry every 100 ms (disabled in --quiet mode to
    // prevent Windows console I/O locks from jittering 1 ms isochronous URBs)
    if (!cfg.quietUi &&
        std::chrono::duration<double>(now - lastUiUpdate).count() >= 0.10) {
      lastUiUpdate = now;
      double progress =
          (cfg.durationSec > 0.0)
              ? std::clamp(elapsedSec / cfg.durationSec, 0.0, 1.0)
              : 1.0;
      int barWidth = 20;
      int filled = static_cast<int>(progress * barWidth);

      std::ostringstream line;
      line << "  [AUDIO] [";
      for (int b = 0; b < barWidth; ++b) {
        line << (b < filled ? '#' : (b == filled ? '>' : '.'));
      }
      line << "] " << std::fixed << std::setprecision(1) << std::setw(4)
           << elapsedSec << "s";
      if (cfg.durationSec > 0.0) {
        line << "/" << std::setprecision(1) << cfg.durationSec << "s";
      }
      line << " | URBs: " << std::setw(4) << totalUrbsCompleted
           << " | Underruns: " << totalUnderruns
           << " | Vol: " << std::setw(3)
           << static_cast<int>(cfg.amplitude * 100.0) << "% | "
           << RoutingModeToString(cfg.routingMode) << "     \r";
      std::cout << line.str() << std::flush;
    }
  }

  // 7. Submit one silent zero-filled buffer and drain in-flight URBs cleanly
  //    so the DAC settles at 0 V without a pop/click on exit.
  for (size_t i = 0; i < numUrbs; ++i) {
    if (urbs[i].inFlight) {
      WaitForSingleObject(urbs[i].overlapped.hEvent, 200);
      DWORD dummy = 0;
      WinUsb_GetOverlappedResult(ctx.hAudioInterface, &urbs[i].overlapped,
                                 &dummy, FALSE);
      urbs[i].inFlight = false;
    }
  }

  // Send a brief zero-filled URB to park the DAC at 0
  if (isIsoch && hIsochBuffer) {
    const ULONG silenceLen = useExact44100 ? 352 : slotStride;
    std::memset(ringBuffer.data(), 0, silenceLen);
    ResetEvent(urbs[0].overlapped.hEvent);
    if (WinUsb_WriteIsochPipeAsap(hIsochBuffer, 0, silenceLen, FALSE,
                                  &urbs[0].overlapped) ||
        GetLastError() == ERROR_IO_PENDING) {
      WaitForSingleObject(urbs[0].overlapped.hEvent, 100);
    }
  }

  WinUsb_AbortPipe(ctx.hAudioInterface, ctx.audioOutPipe);

  for (size_t i = 0; i < numUrbs; ++i) {
    if (urbs[i].overlapped.hEvent) {
      CloseHandle(urbs[i].overlapped.hEvent);
      urbs[i].overlapped.hEvent = nullptr;
    }
  }

  if (hIsochBuffer) {
    WinUsb_UnregisterIsochBuffer(hIsochBuffer);
    hIsochBuffer = nullptr;
  }

  if (hMmcss) {
    AvRevertMmThreadCharacteristics(hMmcss);
  }

  if (inSyncThread.joinable()) {
    inSyncRunning.store(false, std::memory_order_release);
    WinUsb_AbortPipe(ctx.hAudioInterface, ctx.audioInPipe);
    inSyncThread.join();
  }

  auto endTime = std::chrono::steady_clock::now();
  double totalElapsedSec =
      std::chrono::duration<double>(endTime - startTime).count();
  uint64_t totalFramesTransferred = totalBytesTransferred / kBytesPerFrame;

  std::cout << "\n\n==========================================================="
               "===========\n";
  std::cout << "                RESUMEN DE TRANSMISION DE AUDIO (HITO 3)       "
               "       \n";
  std::cout << "==============================================================="
               "=======\n";
  std::cout << "  Tiempo de streaming activo : " << std::fixed
            << std::setprecision(2) << totalElapsedSec << " segundos\n";
  std::cout << "  Paquetes URB completados   : " << totalUrbsCompleted
            << " URBs (" << totalPacketsCompleted
            << " tramas isocronas de 1 ms)\n";
  std::cout << "  Muestras multicanal (4ch)  : " << totalFramesTransferred
            << " frames (" << (totalBytesTransferred / 1024) << " KB)\n";
  std::cout << "  Discontinuidades / XRUNs   : " << totalUnderruns
            << " (ASAP resets: " << totalAsapResets << ")\n";
  std::cout << "  Jitter max entre URBs      : " << std::fixed
            << std::setprecision(2) << maxUrbGapMs << " ms"
            << (useExact44100 ? (" (gaps >5ms: " + std::to_string(gapsOver5ms) + ")") : "")
            << "\n";
  if (cfg.syncFromIn) {
    const uint64_t inPkts = totalInPackets.load();
    const uint64_t inFrames = totalInFrames.load();
    double measHz =
        (inPkts > 0) ? ((static_cast<double>(inFrames) / static_cast<double>(inPkts)) * 1000.0)
                     : 0.0;
    std::cout << "  Reloj EP 0x" << HexByte(ctx.audioInPipe)
              << " IN           : " << std::fixed << std::setprecision(3)
              << measHz << " Hz (" << inPkts << " pkts IN";
    if (inPkts > 0) {
      std::cout << ", pkt " << minInPktBytes.load() << ".." << maxInPktBytes.load() << " B";
    }
    std::cout << ")\n";
  }
  std::cout << "==============================================================="
               "=======\n";

  return (totalUrbsCompleted > 0) ? 0 : 3;
}

// ============================================================================
// CLI Argument Parser & Entry Point
// ============================================================================

void PrintUsage(const char *progName) {
  std::cout
      << "Uso: " << progName << " [opciones]\n\n"
      << "Opciones:\n"
      << "  --frequency, -f <Hz>     Frecuencia de la onda senoidal en Hz "
         "(defecto: 440.0)\n"
      << "  --duration,  -d <seg>    Duracion de la prueba en segundos, 0 = "
         "continuo (defecto: 10.0)\n"
      << "  --volume,    -v <0..1>   Amplitud digital 0.0 a 1.0 (defecto: "
         "0.75)\n"
      << "  --channels,  -c <modo>   Canales de salida: all | master | phones "
         "| dual (defecto: all)\n"
      << "  --exact,     -e          Cadencia exacta 44100 Hz (1 pkt/URB, lead-45)\n"
      << "  --rate,      -r <Hz>     Tasa exacta USB en modo 1 pkt/URB (ej. 44100, 44098, 45000)\n"
      << "  --sync,      -s          Sincroniza OUT automaticamente al reloj real de EP 0x83 IN\n"
      << "  --urbs,      -u <N>      Cantidad de URBs en cola en modo exacto (defecto: 64)\n"
      << "  --quiet,     -q          Sin refresco de consola durante el streaming\n"
      << "  --help,      -h          Muestra esta pantalla de ayuda\n\n"
      << "Ejemplo de verificacion del Hito 3:\n"
      << "  " << progName << " --frequency 440 --duration 10\n";
}

int main(int argc, char *argv[]) {
  AudioTestConfig cfg{};

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    } else if (arg == "--exact" || arg == "-e") {
      cfg.exact44100 = true;
    } else if (arg == "--sync" || arg == "-s") {
      cfg.syncFromIn = true;
      cfg.exact44100 = true;
      cfg.quietUi = true;
    } else if (arg == "--quiet" || arg == "-q") {
      cfg.quietUi = true;
    } else if ((arg == "--rate" || arg == "-r") && i + 1 < argc) {
      cfg.exact44100 = true;
      cfg.exactRateHz = std::clamp(std::atof(argv[++i]), 32000.0, 48000.0);
    } else if ((arg == "--urbs" || arg == "-u") && i + 1 < argc) {
      cfg.exactUrbs = static_cast<size_t>(std::clamp(std::atoi(argv[++i]), 8, 256));
    } else if ((arg == "--frequency" || arg == "-f") && i + 1 < argc) {
      cfg.frequencyHz = std::clamp(std::atof(argv[++i]), 20.0, 20000.0);
    } else if ((arg == "--duration" || arg == "-d") && i + 1 < argc) {
      cfg.durationSec = std::max(0.0, std::atof(argv[++i]));
    } else if ((arg == "--volume" || arg == "-v") && i + 1 < argc) {
      cfg.amplitude = std::clamp(std::atof(argv[++i]), 0.05, 0.98);
    } else if ((arg == "--channels" || arg == "-c") && i + 1 < argc) {
      std::string mode = argv[++i];
      if (mode == "master")
        cfg.routingMode = RoutingMode::MasterOnly;
      else if (mode == "phones")
        cfg.routingMode = RoutingMode::PhonesOnly;
      else if (mode == "dual")
        cfg.routingMode = RoutingMode::DualTone;
      else
        cfg.routingMode = RoutingMode::AllChannels;
    }
  }

  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  std::cout << "==============================================================="
               "=======\n";
  std::cout << "   Behringer BCD2000 Streaming PCM y Sintesis Senoidal (Hito 3 "
               "- x64) \n";
  std::cout << "   Target: VID 0x1397, PID 0x00BD | Driver: WinUSB Isochronous "
               "      \n";
  std::cout << "   Copyright (C) 2024-2026 Martin Revert and contributors (GPL-3.0+)  \n";
  std::cout << "   This is free software with ABSOLUTELY NO WARRANTY.                 \n";
  std::cout << "==============================================================="
               "=======\n\n";

  BcdUsbAudioContext ctx{};
  if (!InitializeBcdAudioDevice(ctx)) {
    return 1;
  }

  int result = RunAudioStreamingTest(ctx, cfg);

  std::cout << "\n[CLEANUP] Apagando indicadores LED y liberando recursos "
               "WinUSB...\n";
  SetAudioStatusLeds(ctx, false);

  for (size_t i = ctx.allInterfaces.size(); i > 0; --i) {
    WinUsb_Free(ctx.allInterfaces[i - 1]);
  }
  if (ctx.hDevice != INVALID_HANDLE_VALUE) {
    CloseHandle(ctx.hDevice);
  }

  std::cout << "[OK] Sesion de audio finalizada limpiamente.\n";
  return result;
}

