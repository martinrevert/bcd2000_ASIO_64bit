/**
 * @file bcd2000_diag_midi.cpp
 * @brief Milestone 2 Diagnostic Tool: Physical Control Telemetry & LED Feedback
 *        for Behringer BCD2000 (VID 0x1397, PID 0x00BD) over WinUSB x64.
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
 * Implements the exact USB protocol of the Behringer BCD2000 8051 MCU
 * (verified against Linux kernel sound/usb/bcd2000/bcd2000.c and Mixxx
 *  Behringer BCD2000.midi.xml / Behringer-BCD2000-scripts.js):
 *  - Handshake: Complete 52-byte initialization sequence on Endpoint 0x01
 * (OUT).
 *  - Input (EP 0x81 IN): Byte 0 is payload length (1..63); bytes 1..len are a
 *    stateful MIDI stream using MIDI Running Status.
 *  - Output (EP 0x01 OUT): 64-byte frame [0x03, 0x00, len, midi_bytes...,
 * 0...].
 *  - Physical LEDs: All 26 hardware LEDs are controlled via MIDI Control Change
 *    on Channel 1 (0xB0, CC 0x00..0x19, value 0x7F=ON / 0x00=OFF).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <conio.h>
#include <initguid.h>
#include <setupapi.h>
#include <usb.h>
#include <winusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

// Generic WinUSB Device Interface GUID (Zadig / WinUSB default)
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB_GENERIC, 0xdee824ef, 0x729b, 0x4a0e, 0x9c,
            0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);

// Complete 52-byte initialization handshake from Linux kernel
// sound/usb/bcd2000/bcd2000.c
static const uint8_t BCD2000_INIT_SEQUENCE[52] = {
    0x07, 0x00, 0x00, 0x00, 0x78, 0x48, 0x1c, 0x81, 0xc4, 0x00, 0x00,
    0x00, 0x5e, 0x53, 0x4a, 0xa7, 0x18, 0xfa, 0x11, 0xff, 0x6c, 0xf3,
    0x90, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x18,
    0xfa, 0x11, 0xff, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf2, 0x34, 0x4a, 0xf7, 0x18, 0xfa, 0x11, 0xff};

// ============================================================================
// Global State & Synchronization Primitives
// ============================================================================

std::atomic<bool> g_Running{true};
std::atomic<bool> g_PauseReader{false};
std::atomic<uint64_t> g_TotalPacketsReceived{0};
std::atomic<uint64_t> g_TotalEventsDecoded{0};
std::atomic<uint64_t> g_TotalLedCommandsSent{0};

// Serializes WinUSB read/write operations on ctx.hMidiInterface so that
// WinUsb_ReadPipe timeouts never abort in-flight WinUsb_WritePipe calls.
std::mutex g_UsbIoMutex;

struct DecodedControlEvent {
  std::string controlType; // "FADER", "KNOB", "JOG", "BUTTON", "PITCH"
  std::string controlName;
  std::string detailValue;
  std::string rawHex;
  uint8_t status = 0;
  uint8_t channel = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
  bool hasPhysicalLed = false;
  bool ledToggled = false;
  bool newLedState = false;
  uint8_t ledCc = 0;
};

struct PendingLedCommand {
  uint8_t ledCc;
  uint8_t value;
};

std::mutex g_EventMutex;
std::queue<DecodedControlEvent> g_EventQueue;
std::queue<PendingLedCommand> g_PendingLedQueue;

// Tracks current state of the 26 physical LEDs (0xB0, CC 0x00..0x19)
std::map<uint8_t, bool> g_LedStates;
// Tracks last value of each control: key = (status << 8) | data1
std::map<uint16_t, uint8_t> g_ControlValues;

struct BcdUsbContext {
  HANDLE hDevice = INVALID_HANDLE_VALUE;
  WINUSB_INTERFACE_HANDLE hInterface0 = nullptr;
  WINUSB_INTERFACE_HANDLE hMidiInterface = nullptr;
  WINUSB_INTERFACE_HANDLE hAudioInterface = nullptr;
  std::vector<WINUSB_INTERFACE_HANDLE> allInterfaces;
  UCHAR midiOutPipe = 0x01;
  UCHAR midiInPipe = 0x81;
  UCHAR isoOutPipe = 0x02;
  UCHAR isoInPipe = 0x83;
};

// ============================================================================
// Helper Formatting Functions
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

/**
 * @brief Formats a 0..127 unipolar percentage progress bar: [========|       ]
 * 53%
 */
std::string FormatProgressBar(uint8_t value, int width = 16) {
  uint8_t clamped = std::min<uint8_t>(value, 127);
  int filled = (clamped * width) / 127;
  std::string bar = "[";
  for (int i = 0; i < width; ++i) {
    if (i < filled)
      bar += "=";
    else if (i == filled)
      bar += "|";
    else
      bar += " ";
  }
  int pct = (clamped * 100) / 127;
  std::ostringstream oss;
  oss << bar << "] " << std::setw(3) << std::setfill(' ') << pct << "%";
  return oss.str();
}

/**
 * @brief Formats a bipolar percentage progress bar (-8192..+8191): [    |====
 * ] +30%
 */
std::string FormatBipolarBar(int value, int maxRange = 8192, int width = 16) {
  int half = width / 2;
  int pos = (value * half) / maxRange;
  pos = std::clamp(pos, -half, half);
  std::string bar = "[";
  for (int i = -half; i <= half; ++i) {
    if (i == 0) {
      bar += "|";
    } else if ((pos < 0 && i >= pos && i < 0) ||
               (pos > 0 && i <= pos && i > 0)) {
      bar += "=";
    } else {
      bar += " ";
    }
  }
  int pct = std::clamp((value * 100) / maxRange, -100, 100);
  std::ostringstream oss;
  oss << bar << "] " << (pct >= 0 ? "+" : "") << std::setw(3)
      << std::setfill(' ') << pct << "%";
  return oss.str();
}

// ============================================================================
// USB LED & MIDI Command Transmission Engine (Endpoint 0x01 OUT)
// ============================================================================

/**
 * @brief Sends a formatted 64-byte BCD2000 command frame to Endpoint 0x01.
 *        Caller should hold g_UsbIoMutex or ensure reader is paused.
 *
 * Exact framing from Linux kernel sound/usb/bcd2000/bcd2000.c
 * (bcd2000_midi_send):
 *  - Byte 0: 0x03 (device_cmd_prefix[0])
 *  - Byte 1: 0x00 (device_cmd_prefix[1])
 *  - Byte 2: payloadLen (number of MIDI bytes in this frame, 1..61)
 *  - Byte 3..(3+payloadLen-1): Raw MIDI stream bytes
 *  - Byte (3+payloadLen)..63: Zero-padded to BUFSIZE (64 bytes)
 */
bool SendBcdMidiFrameLocked(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                            const uint8_t *midiPayload, uint8_t payloadLen) {
  if (!hWinUsb || outPipeId == 0 || !midiPayload || payloadLen == 0 ||
      payloadLen > 61) {
    return false;
  }

  uint8_t frame[64] = {0};
  frame[0] = 0x03;
  frame[1] = 0x00;
  frame[2] = payloadLen;
  std::memcpy(&frame[3], midiPayload, payloadLen);

  ULONG bytesWritten = 0;
  BOOL ok = WinUsb_WritePipe(hWinUsb, outPipeId, frame, sizeof(frame),
                             &bytesWritten, nullptr);
  if (ok && bytesWritten == sizeof(frame)) {
    g_TotalLedCommandsSent.fetch_add(1);
    return true;
  }

  // If the 8051 pipe temporarily stalled or aborted due to timing, reset and
  // retry once
  WinUsb_ResetPipe(hWinUsb, outPipeId);
  Sleep(15);
  bytesWritten = 0;
  ok = WinUsb_WritePipe(hWinUsb, outPipeId, frame, sizeof(frame), &bytesWritten,
                        nullptr);
  if (ok && bytesWritten == sizeof(frame)) {
    g_TotalLedCommandsSent.fetch_add(1);
    return true;
  }

  DWORD err = GetLastError();
  std::cerr << "  [-] Error en WinUsb_WritePipe (EP 0x" << std::hex
            << static_cast<int>(outPipeId) << std::dec
            << "): " << FormatWin32Error(err) << "\n";
  return false;
}

bool SendBcdSingleMidiLocked(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                             uint8_t status, uint8_t data1, uint8_t data2) {
  uint8_t msg[3] = {status, data1, data2};
  return SendBcdMidiFrameLocked(hWinUsb, outPipeId, msg, sizeof(msg));
}

/**
 * @brief Illuminates or turns off all 26 physical LEDs on the BCD2000
 *        (0xB0, CC 0x00..0x19).
 *
 * Pauses the reader thread during the sweep so WinUsb_ReadPipe timeouts on the
 * shared interface handle cannot abort WinUsb_WritePipe transfers, and paces
 * individual 3-byte frames at 12 ms so the 8051 internal 31,250 bps UART FIFO
 * never overflows.
 */
void SetAllLedsState(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                     bool turnOn) {
  uint8_t val = turnOn ? 0x7F : 0x00;

  // Pause reader thread and acquire exclusive USB I/O lock
  g_PauseReader.store(true);
  std::lock_guard<std::mutex> ioLock(g_UsbIoMutex);
  Sleep(10);

  // Behringer BCD2000 has exactly 26 physical LEDs at 0xB0, CC 0x00..0x19
  // (0..25)
  for (uint8_t cc = 0x00; cc <= 0x19; ++cc) {
    SendBcdSingleMidiLocked(hWinUsb, outPipeId, 0xB0, cc, val);
    g_LedStates[cc] = turnOn;
    Sleep(12);
  }

  g_PauseReader.store(false);
}

// ============================================================================
// BCD2000 Hardware Control & LED Mapping Dictionary
// (Verified against Mixxx Behringer BCD2000.midi.xml &
// Behringer-BCD2000-scripts.js)
// ============================================================================

/**
 * @brief Maps a physical button input MIDI Note (0x90, inputNote) to its
 *        exact hardware LED Control Change address (0xB0, outLedCc).
 *
 * Returns true if the button has a physical LED on the BCD2000 PCB, or false
 * for the 12 momentary transport/bend/loop buttons (0x00..0x0B) that do not
 * have their own physical LED.
 */
bool MapButtonNoteToLedCc(uint8_t inputNote, uint8_t &outLedCc) {
  // Deck A (0x0C..0x13 -> LEDs 0x18..0x11) & Deck B (0x14..0x1B -> LEDs
  // 0x10..0x09) On the BCD2000 PCB shift register chain, these 16 illuminated
  // buttons are wired in exact reverse order: ledCc = 0x24 - inputNote (36 -
  // inputNote).
  if (inputNote >= 0x0C && inputNote <= 0x1B) {
    outLedCc = static_cast<uint8_t>(0x24 - inputNote);
    return true;
  }
  // Deck A Analog Input / Ext In A button (0x1C -> LED 0x19)
  if (inputNote == 0x1C) {
    outLedCc = 0x19;
    return true;
  }
  // Deck B Ext In B, FX 1..4, Talkover/Mic, PFL A/B, Phones Split
  // (0x1D..0x25 -> LEDs 0x08..0x00): ledCc = 0x25 - inputNote (37 - inputNote).
  if (inputNote >= 0x1D && inputNote <= 0x25) {
    outLedCc = static_cast<uint8_t>(0x25 - inputNote);
    return true;
  }
  // Buttons 0x00..0x0B (Search <<, >>, Pitch Bend -/+, Loop In/Reloop) are
  // momentary rubber buttons without individual physical LEDs on the PCB.
  return false;
}

struct ControlMapping {
  std::string type;
  std::string name;
};

ControlMapping LookupControl(uint8_t status, uint8_t channel, uint8_t data1) {
  uint8_t msgType = status & 0xF0;

  if (msgType == 0x90 ||
      msgType == 0x80) { // Note On / Note Off (Physical Buttons)
    switch (data1) {
    // Momentary Deck A Buttons (0x00..0x05)
    case 0x00:
      return {"BUTTON", "Deck A Search << (Back) [0x00]"};
    case 0x01:
      return {"BUTTON", "Deck A Search >> (Fwd) [0x01]"};
    case 0x02:
      return {"BUTTON", "Deck A Pitch Bend - [0x02]"};
    case 0x03:
      return {"BUTTON", "Deck A Pitch Bend + [0x03]"};
    case 0x04:
      return {"BUTTON", "Deck A Set Loop (In) [0x04]"};
    case 0x05:
      return {"BUTTON", "Deck A Reloop / Exit [0x05]"};

    // Momentary Deck B Buttons (0x06..0x0B)
    case 0x06:
      return {"BUTTON", "Deck B Search << (Back) [0x06]"};
    case 0x07:
      return {"BUTTON", "Deck B Search >> (Fwd) [0x07]"};
    case 0x08:
      return {"BUTTON", "Deck B Pitch Bend - [0x08]"};
    case 0x09:
      return {"BUTTON", "Deck B Pitch Bend + [0x09]"};
    case 0x0A:
      return {"BUTTON", "Deck B Set Loop (In) [0x0A]"};
    case 0x0B:
      return {"BUTTON", "Deck B Reloop / Exit [0x0B]"};

    // Illuminated Deck A Buttons (0x0C..0x13 -> LEDs 0x18..0x11)
    case 0x0C:
      return {"BUTTON", "Deck A Kill Low [0x0C]"};
    case 0x0D:
      return {"BUTTON", "Deck A Kill Mid [0x0D]"};
    case 0x0E:
      return {"BUTTON", "Deck A Kill High [0x0E]"};
    case 0x0F:
      return {"BUTTON", "Deck A Loop Out / Active [0x0F]"};
    case 0x10:
      return {"BUTTON", "Deck A Sync [0x10]"};
    case 0x11:
      return {"BUTTON", "Deck A Scratch Mode [0x11]"};
    case 0x12:
      return {"BUTTON", "Deck A Play / Pause [0x12]"};
    case 0x13:
      return {"BUTTON", "Deck A Cue [0x13]"};

    // Illuminated Deck B Buttons (0x14..0x1B -> LEDs 0x10..0x09)
    case 0x14:
      return {"BUTTON", "Deck B Kill Low [0x14]"};
    case 0x15:
      return {"BUTTON", "Deck B Kill Mid [0x15]"};
    case 0x16:
      return {"BUTTON", "Deck B Kill High [0x16]"};
    case 0x17:
      return {"BUTTON", "Deck B Loop Out / Active [0x17]"};
    case 0x18:
      return {"BUTTON", "Deck B Sync [0x18]"};
    case 0x19:
      return {"BUTTON", "Deck B Scratch Mode [0x19]"};
    case 0x1A:
      return {"BUTTON", "Deck B Play / Pause [0x1A]"};
    case 0x1B:
      return {"BUTTON", "Deck B Cue [0x1B]"};

    // Illuminated Mixer / FX / PFL Buttons (0x1C..0x25 -> LEDs 0x19,
    // 0x08..0x00)
    case 0x1C:
      return {"BUTTON", "Deck A Analog Input (Ext A) [0x1C]"};
    case 0x1D:
      return {"BUTTON", "Deck B Analog Input (Ext B) [0x1D]"};
    case 0x1E:
      return {"BUTTON", "FX Button 1 (Control A) [0x1E]"};
    case 0x1F:
      return {"BUTTON", "FX Button 2 (Control B) [0x1F]"};
    case 0x20:
      return {"BUTTON", "FX Button 3 (FX On A) [0x20]"};
    case 0x21:
      return {"BUTTON", "FX Button 4 (FX On B) [0x21]"};
    case 0x22:
      return {"BUTTON", "Mic On Air / Talkover [0x22]"};
    case 0x23:
      return {"BUTTON", "Phones Cue A (PFL A) [0x23]"};
    case 0x24:
      return {"BUTTON", "Phones Cue B (PFL B) [0x24]"};
    case 0x25:
      return {"BUTTON", "Phones Split [0x25]"};

    default:
      return {"BUTTON", "Boton Note 0x" + HexByte(data1) + " (#" +
                            std::to_string(data1) + ")"};
    }
  }

  if (msgType == 0xB0) { // Control Change (Faders, Knobs, Jog Wheels)
    switch (data1) {
    case 0x00:
      return {"FADER", "Deck A Volume Fader [CC 0x00]"};
    case 0x01:
      return {"FADER", "Crossfader [CC 0x01]"};
    case 0x02:
      return {"FADER", "Deck B Volume Fader [CC 0x02]"};
    case 0x03:
      return {"KNOB", "Deck A EQ Low [CC 0x03]"};
    case 0x04:
      return {"KNOB", "Deck A EQ Mid [CC 0x04]"};
    case 0x05:
      return {"KNOB", "Deck A EQ High [CC 0x05]"};
    case 0x06:
      return {"KNOB", "Deck A Gain [CC 0x06]"};
    case 0x07:
      return {"KNOB", "Deck B EQ Low [CC 0x07]"};
    case 0x08:
      return {"KNOB", "Deck B EQ Mid [CC 0x08]"};
    case 0x09:
      return {"KNOB", "Deck B EQ High [CC 0x09]"};
    case 0x0A:
      return {"KNOB", "Deck B Gain [CC 0x0A]"};
    case 0x0B:
      return {"FADER", "Deck A Pitch Fader [CC 0x0B]"};
    case 0x0C:
      return {"FADER", "Deck B Pitch Fader [CC 0x0C]"};
    case 0x0D:
      return {"KNOB", "FX Knob A (Param 1) [CC 0x0D]"};
    case 0x0E:
      return {"KNOB", "FX Knob B (Param 2) [CC 0x0E]"};
    case 0x0F:
      return {"KNOB", "FX Knob C (Param 3) [CC 0x0F]"};
    case 0x10:
      return {"KNOB", "Master Volume Knob [CC 0x10]"};
    case 0x11:
      return {"KNOB", "Phones Mix Knob [CC 0x11]"};
    case 0x12:
      return {"JOG", "Deck B Jog Wheel [CC 0x12]"};
    case 0x13:
      return {"JOG", "Deck A Jog Wheel [CC 0x13]"};
    default:
      return {"FADER", "Control Change CC 0x" + HexByte(data1) + " (#" +
                           std::to_string(data1) + ")"};
    }
  }

  if (msgType == 0xE0) { // Pitch Bend
    std::string deck = (channel == 1 ? "Deck A " : "Deck B ");
    return {"PITCH", deck + "High-Res Pitch Bend"};
  }

  return {"MIDI", "Evento MIDI 0x" + HexByte(status)};
}

// ============================================================================
// Stateful MIDI Stream Decoder & Endpoint 0x81 Reader Thread
// ============================================================================

void EmitDecodedMidiMessage(uint8_t status, uint8_t d1, uint8_t d2,
                            const std::string &rawPacketStr) {
  uint8_t msgType = status & 0xF0;
  uint8_t channel = (status & 0x0F) + 1;

  DecodedControlEvent ev;
  ev.status = status;
  ev.channel = channel;
  ev.data1 = d1;
  ev.data2 = d2;
  ev.rawHex = rawPacketStr;

  ControlMapping mapping = LookupControl(status, channel, d1);
  ev.controlType = mapping.type;
  ev.controlName = mapping.name;

  uint16_t stateKey = (static_cast<uint16_t>(status) << 8) | d1;
  g_ControlValues[stateKey] = d2;

  if (msgType == 0xB0) {
    // Every Control Change (Fader, Knob, Jog) displays a live % progress bar!
    std::string barStr =
        FormatProgressBar(d2) + " [Val: " + std::to_string(d2) + "/127]";
    if (mapping.type == "JOG") {
      int delta = (d2 >= 65) ? (static_cast<int>(d2) - 64)
                             : (static_cast<int>(d2) - 65);
      barStr += " (" + std::string(delta >= 0 ? "+" : "") +
                std::to_string(delta) + ")";
    }
    ev.detailValue = barStr;
  } else if (msgType == 0x90 || msgType == 0x80) {
    // Physical Button Note On / Note Off
    bool isPressed = (msgType == 0x90 && d2 > 0);
    ev.detailValue = isPressed ? "PRESIONADO (Down)" : "LIBERADO (Up)";

    uint8_t targetLedCc = 0;
    if (MapButtonNoteToLedCc(d1, targetLedCc)) {
      ev.hasPhysicalLed = true;
      ev.ledCc = targetLedCc;
      if (isPressed) {
        bool newLed = !g_LedStates[targetLedCc];
        g_LedStates[targetLedCc] = newLed;
        ev.ledToggled = true;
        ev.newLedState = newLed;

        uint8_t ledVel = newLed ? 0x7F : 0x00;
        std::lock_guard<std::mutex> lock(g_EventMutex);
        g_PendingLedQueue.push({targetLedCc, ledVel});
      }
    } else {
      ev.hasPhysicalLed = false;
    }
  } else if (msgType == 0xE0) {
    int pitchVal = (static_cast<int>(d2) << 7) | static_cast<int>(d1);
    int normalized = pitchVal - 8192;
    ev.detailValue = FormatBipolarBar(normalized, 8192) +
                     " [Raw: " + std::to_string(pitchVal) + "]";
  } else {
    ev.detailValue =
        "Data1: " + std::to_string(d1) + ", Data2: " + std::to_string(d2);
  }

  g_TotalEventsDecoded.fetch_add(1);

  std::lock_guard<std::mutex> lock(g_EventMutex);
  g_EventQueue.push(ev);
}

void MidiReaderThreadWorker(WINUSB_INTERFACE_HANDLE hWinUsbMidi, UCHAR inPipeId,
                            UCHAR outPipeId) {
  ULONG inTimeoutMs = 250;
  WinUsb_SetPipePolicy(hWinUsbMidi, inPipeId, PIPE_TRANSFER_TIMEOUT,
                       sizeof(inTimeoutMs), &inTimeoutMs);
  UCHAR autoClearStall = 1;
  WinUsb_SetPipePolicy(hWinUsbMidi, inPipeId, AUTO_CLEAR_STALL,
                       sizeof(autoClearStall), &autoClearStall);
  UCHAR rawIo = 1;
  WinUsb_SetPipePolicy(hWinUsbMidi, inPipeId, RAW_IO, sizeof(rawIo), &rawIo);

  uint8_t buffer[64];

  // Persistent MIDI Running Status State Machine across USB packets
  uint8_t runningStatus = 0;
  int expectedDataBytes = 0;
  int collectedDataBytes = 0;
  uint8_t pendingD1 = 0;

  while (g_Running.load()) {
    if (g_PauseReader.load()) {
      Sleep(5);
      continue;
    }

    ULONG bytesRead = 0;
    BOOL ok = FALSE;
    DWORD readErr = ERROR_SUCCESS;

    {
      std::lock_guard<std::mutex> ioLock(g_UsbIoMutex);
      ok = WinUsb_ReadPipe(hWinUsbMidi, inPipeId, buffer, sizeof(buffer),
                           &bytesRead, nullptr);
      if (!ok) {
        readErr = GetLastError();
      }
    }

    if (ok && bytesRead > 0) {
      g_TotalPacketsReceived.fetch_add(1);

      // Linux snd-bcd2000 protocol (bcd2000_input_complete):
      // buffer[0] is the exact number of valid MIDI bytes in this packet
      // (1..63). Bytes after buffer[payloadLen] are uninitialized 8051 RAM
      // garbage and MUST be ignored.
      uint8_t payloadLen = buffer[0];
      if (payloadLen == 0 || payloadLen > 63 ||
          static_cast<ULONG>(payloadLen) >= bytesRead) {
        continue;
      }

      // Format ONLY the valid MIDI payload bytes for diagnostic clarity
      std::ostringstream rawOss;
      rawOss << "Len=" << static_cast<int>(payloadLen) << ": ";
      for (uint8_t i = 1; i <= payloadLen; ++i) {
        rawOss << HexByte(buffer[i]);
        if (i < payloadLen)
          rawOss << " ";
      }
      std::string rawPacketStr = rawOss.str();

      // Feed each valid byte into the stateful MIDI Running Status parser
      for (uint8_t i = 1; i <= payloadLen; ++i) {
        uint8_t b = buffer[i];

        if ((b & 0x80) != 0) {
          // Status byte received
          if (b >= 0xF8) {
            // System Real-Time (does not affect Running Status)
            continue;
          }
          if (b >= 0xF0) {
            // System Common / SysEx resets Running Status
            runningStatus = 0;
            expectedDataBytes = 0;
            collectedDataBytes = 0;
            continue;
          }

          // Channel Voice Status Byte (0x80..0xEF)
          runningStatus = b;
          collectedDataBytes = 0;
          uint8_t msgType = b & 0xF0;
          expectedDataBytes = (msgType == 0xC0 || msgType == 0xD0) ? 1 : 2;
        } else {
          // Data byte received (0x00..0x7F)
          if (runningStatus == 0 || expectedDataBytes == 0) {
            continue;
          }

          if (expectedDataBytes == 1) {
            EmitDecodedMidiMessage(runningStatus, b, 0, rawPacketStr);
            collectedDataBytes = 0; // Keep runningStatus active!
          } else {
            if (collectedDataBytes == 0) {
              pendingD1 = b;
              collectedDataBytes = 1;
            } else {
              uint8_t d1 = pendingD1;
              uint8_t d2 = b;
              collectedDataBytes =
                  0; // Keep runningStatus active for Running Status!
              EmitDecodedMidiMessage(runningStatus, d1, d2, rawPacketStr);
            }
          }
        }
      }

      // Drain any queued LED updates now that WinUsb_ReadPipe is complete
      std::queue<PendingLedCommand> ledsToSend;
      {
        std::lock_guard<std::mutex> lock(g_EventMutex);
        std::swap(ledsToSend, g_PendingLedQueue);
      }
      if (!ledsToSend.empty()) {
        std::lock_guard<std::mutex> ioLock(g_UsbIoMutex);
        Sleep(4); // Allow 8051 MCU to finish USB IN transaction before USB OUT
        while (!ledsToSend.empty()) {
          auto cmd = ledsToSend.front();
          ledsToSend.pop();
          SendBcdSingleMidiLocked(hWinUsbMidi, outPipeId, 0xB0, cmd.ledCc,
                                  cmd.value);
          if (!ledsToSend.empty()) {
            Sleep(8);
          }
        }
      }
    } else if (!ok) {
      if (readErr != ERROR_SEM_TIMEOUT) {
        if (readErr == ERROR_DEVICE_NOT_CONNECTED ||
            readErr == ERROR_GEN_FAILURE || readErr == ERROR_DEV_NOT_EXIST) {
          std::cerr << "\n[!] Comunicacion USB perdida en EP 0x" << std::hex
                    << static_cast<int>(inPipeId) << std::dec << ": "
                    << FormatWin32Error(readErr) << "\n";
          g_Running.store(false);
          break;
        }
      }
    }
  }
}

// ============================================================================
// Console Signal Handler
// ============================================================================

BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
      signal == CTRL_CLOSE_EVENT) {
    g_Running.store(false);
    return TRUE;
  }
  return FALSE;
}

// ============================================================================
// SetupAPI & WinUSB Multi-Interface Initializer
// ============================================================================

bool InitializeBcdDevice(BcdUsbContext &ctx) {
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

  std::cout << "  [+] Interfaces WinUSB descubiertas: "
            << ctx.allInterfaces.size() << "\n";

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

      if (ifDesc.bNumEndpoints > 0 && altSetting > 0) {
        WinUsb_SetCurrentAlternateSetting(hIf, altSetting);
      }

      for (UCHAR pIdx = 0; pIdx < ifDesc.bNumEndpoints; ++pIdx) {
        WINUSB_PIPE_INFORMATION pipeInfo{};
        if (WinUsb_QueryPipe(hIf, altSetting, pIdx, &pipeInfo)) {
          bool isIn = USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId);
          std::string pType = "Unknown";
          if (pipeInfo.PipeType == UsbdPipeTypeControl)
            pType = "Control";
          else if (pipeInfo.PipeType == UsbdPipeTypeIsochronous)
            pType = "Isochronous";
          else if (pipeInfo.PipeType == UsbdPipeTypeBulk)
            pType = "Bulk";
          else if (pipeInfo.PipeType == UsbdPipeTypeInterrupt)
            pType = "Interrupt";

          std::cout << "         Pipe 0x" << HexByte(pipeInfo.PipeId) << " ["
                    << (isIn ? "IN " : "OUT") << "] Tipo: " << std::setw(12)
                    << std::setfill(' ') << std::left << pType << std::right
                    << " MaxPacket: " << pipeInfo.MaximumPacketSize << " B\n";

          if (pipeInfo.PipeId == 0x01 ||
              (!isIn && (pipeInfo.PipeType == UsbdPipeTypeBulk ||
                         pipeInfo.PipeType == UsbdPipeTypeInterrupt))) {
            ctx.hMidiInterface = hIf;
            ctx.midiOutPipe = pipeInfo.PipeId;
          }
          if (pipeInfo.PipeId == 0x81 ||
              (isIn && (pipeInfo.PipeType == UsbdPipeTypeBulk ||
                        pipeInfo.PipeType == UsbdPipeTypeInterrupt))) {
            ctx.hMidiInterface = hIf;
            ctx.midiInPipe = pipeInfo.PipeId;
          }
          if (pipeInfo.PipeType == UsbdPipeTypeIsochronous) {
            ctx.hAudioInterface = hIf;
            if (isIn)
              ctx.isoInPipe = pipeInfo.PipeId;
            else
              ctx.isoOutPipe = pipeInfo.PipeId;
          }
        }
      }
    }
  }

  if (!ctx.hMidiInterface) {
    ctx.hMidiInterface = ctx.hInterface0;
  }

  std::cout << "  [+] Canal Control/MIDI asignado -> Endpoint OUT: 0x"
            << HexByte(ctx.midiOutPipe) << " | Endpoint IN: 0x"
            << HexByte(ctx.midiInPipe) << "\n\n";

  // Set pipe policies on the MIDI / Control interface
  ULONG timeoutMs = 500;
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiOutPipe,
                       PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
  UCHAR autoClearStall = 1;
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiOutPipe, AUTO_CLEAR_STALL,
                       sizeof(autoClearStall), &autoClearStall);

  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiInPipe,
                       PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiInPipe, AUTO_CLEAR_STALL,
                       sizeof(autoClearStall), &autoClearStall);

  std::cout << "[HANDSHAKE] Transmitiendo trama de inicializacion completa de "
               "52 bytes al microcontrolador 8051...\n";
  ULONG bytesWritten = 0;
  BOOL ok =
      WinUsb_WritePipe(ctx.hMidiInterface, ctx.midiOutPipe,
                       const_cast<PUCHAR>(BCD2000_INIT_SEQUENCE),
                       sizeof(BCD2000_INIT_SEQUENCE), &bytesWritten, nullptr);

  if (!ok || bytesWritten != sizeof(BCD2000_INIT_SEQUENCE)) {
    DWORD writeErr = GetLastError();
    std::cerr << "  [-] Error en handshake en Pipe 0x"
              << HexByte(ctx.midiOutPipe) << ": " << FormatWin32Error(writeErr)
              << "\n";
    return false;
  }

  std::cout << "  [+] Handshake exitoso (" << bytesWritten
            << " bytes transmitidos). Microcontrolador 8051 activo.\n";

  Sleep(150);

  // Enable Jog Wheel acceleration and request initial knob/slider positions
  // (from Mixxx Behringer-BCD2000-scripts.js init sequence)
  {
    std::lock_guard<std::mutex> ioLock(g_UsbIoMutex);
    SendBcdSingleMidiLocked(ctx.hMidiInterface, ctx.midiOutPipe, 0xB0, 0x63,
                            0x7F);
    Sleep(15);
  }
  std::cout
      << "  [+] Aceleracion de Jog Wheels activada (0xB0, 0x63, 0x7F).\n\n";

  return true;
}

// ============================================================================
// Main Application Routine
// ============================================================================

int main() {
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  std::cout << "==============================================================="
               "=======\n";
  std::cout << "   Behringer BCD2000 Telemetria de Controles y Retorno a LEDs "
               "(Hito 2)\n";
  std::cout << "   Target: VID 0x1397, PID 0x00BD | Driver: WinUSB (x64)       "
               "      \n";
  std::cout << "   Copyright (C) 2024-2026 Martin Revert and contributors (GPL-3.0+)  \n";
  std::cout << "   This is free software with ABSOLUTELY NO WARRANTY.                 \n";
  std::cout << "==============================================================="
               "=======\n\n";

  BcdUsbContext ctx{};

  if (!InitializeBcdDevice(ctx)) {
    return 1;
  }

  // --------------------------------------------------------------------------
  // Visual LED Greeting Sweep (All 26 Hardware LEDs: 0xB0, CC 0x00..0x19)
  // --------------------------------------------------------------------------
  std::cout << "==============================================================="
               "=======\n";
  std::cout << "       VERIFICACION FISICA DE RETORNO A LEDS (SALUDO VISUAL)   "
               "       \n";
  std::cout << "==============================================================="
               "=======\n";
  std::cout << "  Mapa Hardware LED: 26 LEDs en 0xB0 (CC 0x00..0x19), valor "
               "0x7F/0x00\n";
  std::cout << "  [Fase 1] Encendiendo los 26 LEDs fisicos de la BCD2000...\n";

  SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true);
  std::cout << "  [+] Comandos LED ON transmitidos ("
            << g_TotalLedCommandsSent.load() << " tramas de 64B a EP 0x"
            << HexByte(ctx.midiOutPipe) << ").\n";
  std::cout << "  >>> ¡TODOS LOS LEDS DEBEN ESTAR VISIBLEMENTE ILUMINADOS "
               "AHORA! <<<\n";

  for (int sec = 3; sec > 0; --sec) {
    std::cout << "      Manteniendo iluminacion activa: " << sec
              << " segundos restantes...\r" << std::flush;
    Sleep(1000);
  }
  std::cout << "      Manteniendo iluminacion activa: completado!              "
               "        \n";

  std::cout << "  [Fase 2] Apagando LEDs para iniciar monitor interactivo...\n";
  SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, false);
  std::cout << "  [+] Ciclo de saludo completado.\n\n";

  std::cout << "==============================================================="
               "=======\n";
  std::cout << "                 MODO MONITOR EN VIVO Y CONTROL BIDIRECCIONAL  "
               "       \n";
  std::cout << "==============================================================="
               "=======\n";
  std::cout << "  INSTRUCCIONES:\n";
  std::cout << "  - Mueve Faders, Knobs o Pitch: veras la BARRA DE % EN TIEMPO "
               "REAL.\n";
  std::cout
      << "  - Gira Ruedas Jog: veras la barra y el desplazamiento relativo.\n";
  std::cout << "  - Presiona cualquier Boton con LED: su PROPIO LED fisico "
               "conmutara.\n";
  std::cout << "  "
               "---------------------------------------------------------------"
               "-----\n";
  std::cout << "  COMANDOS DE TECLADO INTERACTIVOS:\n";
  std::cout << "      [L] : Encender TODOS los 26 LEDs en una sola pasada.\n";
  std::cout << "      [A] : Apagar TODOS los 26 LEDs en una sola pasada.\n";
  std::cout
      << "      [S] : Solicitar al hardware posicion actual de Knobs/Faders.\n";
  std::cout << "      [T] : Mostrar tabla resumen con el estado actual de cada "
               "control.\n";
  std::cout << "      [C] : Limpiar pantalla de consola.\n";
  std::cout << "      [Q] o ESC : Salir ordenadamente del programa.\n";
  std::cout << "---------------------------------------------------------------"
               "-------\n\n";

  std::thread readerThread(MidiReaderThreadWorker, ctx.hMidiInterface,
                           ctx.midiInPipe, ctx.midiOutPipe);

  while (g_Running.load()) {
    // 1. Process queued decoded control events (swap out of mutex in <1us so
    //    console std::cout never blocks MidiReaderThreadWorker!)
    std::queue<DecodedControlEvent> localEvents;
    {
      std::lock_guard<std::mutex> lock(g_EventMutex);
      std::swap(localEvents, g_EventQueue);
    }

    if (!localEvents.empty()) {
      std::ostringstream batchOut;
      while (!localEvents.empty()) {
        auto ev = localEvents.front();
        localEvents.pop();

        batchOut << std::setfill(' ') << std::left << std::setw(10)
                 << ("[" + ev.controlType + "]") << std::setw(36)
                 << ev.controlName << " " << std::setw(36) << ev.detailValue;

        if (ev.controlType == "BUTTON") {
          if (ev.ledToggled) {
            batchOut << " ==> [LED CC 0x" << HexByte(ev.ledCc) << " "
                     << (ev.newLedState ? "ENCENDIDO" : "APAGADO") << "]";
          } else if (!ev.hasPhysicalLed) {
            batchOut << " [Sin LED fisico]";
          }
        }

        batchOut << "  (" << ev.rawHex << ")\n";
      }
      std::cout << batchOut.str() << std::flush;
    }

    // 2. Process non-blocking keyboard commands
    if (_kbhit()) {
      int ch = _getch();
      if (ch == 'q' || ch == 'Q' || ch == 27) {
        std::cout << "\n[INFO] Solicitud de salida recibida. Finalizando...\n";
        g_Running.store(false);
        break;
      } else if (ch == 'l' || ch == 'L') {
        std::cout << "\n[TEST] Encendiendo los 26 LEDs fisicos (0xB0, CC "
                     "0x00..0x19)...\n";
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true);
        std::cout << "  [+] Todos los 26 LEDs encendidos sin errores. Presiona "
                     "[A] para apagarlos.\n\n";
      } else if (ch == 'a' || ch == 'A') {
        std::cout << "\n[TEST] Apagando los 26 LEDs fisicos (0xB0, CC "
                     "0x00..0x19)...\n";
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, false);
        std::cout << "  [+] Todos los 26 LEDs apagados.\n\n";
      } else if (ch == 's' || ch == 'S') {
        std::cout << "\n[SNAPSHOT] Solicitando estado actual de todos los "
                     "Knobs y Faders (0xB0, 0x64, 0x7F)...\n";
        g_PauseReader.store(true);
        {
          std::lock_guard<std::mutex> ioLock(g_UsbIoMutex);
          SendBcdSingleMidiLocked(ctx.hMidiInterface, ctx.midiOutPipe, 0xB0,
                                  0x64, 0x7F);
        }
        g_PauseReader.store(false);
      } else if (ch == 'c' || ch == 'C') {
        system("cls");
        std::cout
            << "[INFO] Pantalla limpiada. Modo monitor en vivo activo...\n\n";
      } else if (ch == 't' || ch == 'T') {
        std::cout << "\n======================================================="
                     "===============\n";
        std::cout << "                    TABLA RESUMEN DE ESTADO DE CONTROLES "
                     "             \n";
        std::cout << "========================================================="
                     "=============\n";
        std::cout << "  Eventos MIDI decodificados : "
                  << g_TotalEventsDecoded.load() << "\n";
        std::cout << "  Tramas USB recibidas       : "
                  << g_TotalPacketsReceived.load() << "\n";
        std::cout << "  Tramas LED 64B enviadas    : "
                  << g_TotalLedCommandsSent.load() << "\n";
        std::cout << "---------------------------------------------------------"
                     "-------------\n";
        for (const auto &kv : g_ControlValues) {
          uint8_t st = static_cast<uint8_t>(kv.first >> 8);
          uint8_t d1 = static_cast<uint8_t>(kv.first & 0xFF);
          uint8_t val = kv.second;
          auto map = LookupControl(st, (st & 0x0F) + 1, d1);
          std::cout << "  " << std::setfill(' ') << std::left << std::setw(36)
                    << map.name << " = " << std::setw(4)
                    << static_cast<int>(val) << " "
                    << FormatProgressBar(val, 16) << "\n";
        }
        std::cout << "========================================================="
                     "=============\n\n";
      }
    }

    Sleep(10);
  }

  if (readerThread.joinable()) {
    readerThread.join();
  }

  std::cout
      << "[CLEANUP] Apagando todos los LEDs antes de cerrar la sesion USB...\n";
  SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, false);

  for (size_t i = ctx.allInterfaces.size(); i > 0; --i) {
    WinUsb_Free(ctx.allInterfaces[i - 1]);
  }
  if (ctx.hDevice != INVALID_HANDLE_VALUE) {
    CloseHandle(ctx.hDevice);
  }

  std::cout << "[OK] Sesion WinUSB finalizada limpiamente.\n";
  return 0;
}
