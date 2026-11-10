/**
 * @file bcd2000_diag_midi.cpp
 * @brief Behringer BCD2000 Physical Control Telemetry and Bi-Directional LED Engine
 *
 * Part of Milestone 2 ("Telemetría de Controles Físicos y Retorno a LEDs")
 * Target Hardware: Behringer BCD2000 DJ Controller (VID: 0x1397, PID: 0x00BD)
 * Driver: WinUSB (x64)
 *
 * Capabilities:
 *  1. WinUSB Connection & 52-byte 8051 Handshake Sequence (Linux snd-bcd2000).
 *  2. Background Reader Thread on Endpoint 0x81 (IN):
 *     - Universally decodes all incoming MIDI control packets.
 *     - ALWAYS displays raw incoming USB bytes: [RAW: HH HH HH ...].
 *     - Faders & Knobs: Continuous real-time percentage progress bar [========|       ] 50%.
 *     - Jog Wheels: Rotational direction and tick counts (CW / CCW).
 *     - Pitch Sliders: High-resolution bipolar progress bar [   |====   ] +35%.
 *     - Push Buttons & Kill Switches: Instant state decode (PRESIONADO / LIBERADO).
 *  3. Bi-Directional LED Engine on Endpoint 0x01 (OUT):
 *     - Multi-mode support:
 *         Modo 0: Trama estándar Linux snd-bcd2000 (5 bytes: [0x03, 0x00, status, d1, d2]).
 *         Modo 1: Trama MIDI pura (3 bytes: [status, d1, d2]).
 *         Modo 2: Trama alineada a 64 bytes ([0x03, 0x00, status, d1, d2, pad 0...]).
 *         Modo 3: Trama con prefijo de longitud (6 bytes: [0x03, 0x00, 0x03, status, d1, d2]).
 *     - Instant hardware feedback: pressing any physical button toggles its LED on hardware.
 *     - Visual startup test: sequential illumination with on-screen countdown.
 *  4. Interactive Runtime Keyboard Commands:
 *     - [L]: Sweep illumination test across all LEDs (keeps them ON).
 *     - [A]: Turn off all LEDs.
 *     - [M]: Cycle active transmission mode (0 -> 1 -> 2 -> 3).
 *     - [1], [2], [3], [4]: Directly switch mode and send immediate LED burst.
 *     - [T]: Print table of all captured controls and their current values with progress bars.
 *     - [C]: Clear console screen.
 *     - [Q] / [ESC]: Clean exit, extinguishing LEDs and releasing handles.
 */

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>
#include <conio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Target Device Identifiers
constexpr uint16_t BCD2000_VID = 0x1397;
constexpr uint16_t BCD2000_PID = 0x00BD;

// Standard WinUSB Device Interface GUID
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB_GENERIC, 0xdee824ef, 0x729b, 0x4a0e, 0x9c,
            0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);

// 52-byte hardware initialization handshake sequence (from Linux snd-bcd2000)
static const uint8_t BCD2000_INIT_SEQUENCE[52] = {
    0x07, 0x00, 0x00, 0x00, 0x78, 0x48, 0x1c, 0x81,
    0xc4, 0x00, 0x00, 0x00, 0x5e, 0x53, 0x4a, 0xf7,
    0x18, 0xfa, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// ============================================================================
// Data Structures and Global State
// ============================================================================

struct DecodedControlEvent {
  std::chrono::system_clock::time_point timestamp;
  uint8_t status = 0;
  uint8_t channel = 1;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
  std::string rawHex;
  std::string controlType; // "FADER", "KNOB", "BUTTON", "JOG", "PITCH", "MIDI", "RAW_USB"
  std::string controlName;
  std::string detailValue;
  bool ledToggled = false;
  bool newLedState = false;
};

// Global flags and synchronization
static std::atomic<bool> g_Running{true};
static std::atomic<int> g_LedSendMode{0}; // 0 = 5B (Linux), 1 = 3B (MIDI), 2 = 64B, 3 = 6B
static std::mutex g_UsbWriteMutex;
static std::mutex g_EventMutex;
static std::queue<DecodedControlEvent> g_EventQueue;

// LED and Control States
static std::map<uint16_t, bool> g_LedStates; // Key: (status << 8) | data1
static std::map<uint16_t, uint8_t> g_ControlValues; // Key: (status << 8) | data1 -> last value
static std::atomic<uint64_t> g_TotalPacketsReceived{0};
static std::atomic<uint64_t> g_TotalEventsDecoded{0};
static std::atomic<uint64_t> g_TotalLedCommandsSent{0};

// ============================================================================
// Helper Utilities
// ============================================================================

std::string WideToUtf8(const std::wstring &wstr) {
  if (wstr.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                                 static_cast<int>(wstr.size()), nullptr, 0,
                                 nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()),
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
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::ostringstream oss;
  oss << "0x" << std::hex << errorCode << std::dec;
  if (buffer) {
    oss << " (" << WideToUtf8(buffer) << ")";
    LocalFree(buffer);
  }
  return oss.str();
}

/**
 * @brief Formats a standard 0..127 unipolar progress bar: [========|       ] 53%
 */
std::string FormatProgressBar(uint8_t value, int width = 16) {
  int filled = (value * width) / 127;
  std::string bar = "[";
  for (int i = 0; i < width; ++i) {
    if (i < filled)
      bar += "=";
    else if (i == filled)
      bar += "|";
    else
      bar += " ";
  }
  int pct = (value * 100) / 127;
  std::ostringstream oss;
  oss << bar << "] " << std::setw(3) << pct << "%";
  return oss.str();
}

/**
 * @brief Formats a bipolar progress bar (-8192..+8191): [    |====   ] +30%
 */
std::string FormatBipolarBar(int value, int maxRange = 8192, int width = 16) {
  int half = width / 2;
  int pos = (value * half) / maxRange;
  pos = std::clamp(pos, -half, half);
  std::string bar = "[";
  for (int i = -half; i <= half; ++i) {
    if (i == 0) {
      bar += "|";
    } else if ((pos < 0 && i >= pos && i < 0) || (pos > 0 && i <= pos && i > 0)) {
      bar += "=";
    } else {
      bar += " ";
    }
  }
  int pct = (value * 100) / maxRange;
  std::ostringstream oss;
  oss << bar << "] " << (pct >= 0 ? "+" : "") << std::setw(3) << pct << "%";
  return oss.str();
}

const char* GetModeName(int mode) {
  switch (mode) {
  case 0: return "Modo A (5B: [0x03, 0x00, status, d1, d2] - Linux snd-bcd2000)";
  case 1: return "Modo B (3B: [status, d1, d2] - Pure MIDI directo)";
  case 2: return "Modo C (64B: [0x03, 0x00, status, d1, d2, pad 0...])";
  case 3: return "Modo D (6B: [0x03, 0x00, 0x03, status, d1, d2])";
  default: return "Desconocido";
  }
}

// ============================================================================
// USB Transmission Engine (Endpoint 0x01 OUT)
// ============================================================================

/**
 * @brief Transmits a MIDI command to the BCD2000 hardware on Endpoint 0x01.
 * 
 * Supports 4 packet layouts to guarantee hardware compatibility:
 *  - Mode 0: 5 bytes [0x03, 0x00, status, d1, d2] (Linux kernel snd-bcd2000 standard).
 *  - Mode 1: 3 bytes [status, d1, d2] (Pure direct MIDI).
 *  - Mode 2: 64 bytes [0x03, 0x00, status, d1, d2, 0x00...] (Full endpoint buffer pad).
 *  - Mode 3: 6 bytes [0x03, 0x00, len, status, d1, d2] (Length-prefixed packet).
 */
bool SendBcdSingleMidi(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                       uint8_t status, uint8_t data1, uint8_t data2,
                       int mode = -1) {
  if (!hWinUsb || outPipeId == 0) return false;
  if (mode < 0) mode = g_LedSendMode.load();

  uint8_t frame[64] = {0};
  ULONG sendLen = 0;

  switch (mode) {
  case 0: // Linux kernel snd-bcd2000 standard: 2-byte prefix + 3-byte MIDI payload = 5 bytes
    frame[0] = 0x03;
    frame[1] = 0x00;
    frame[2] = status;
    frame[3] = data1;
    frame[4] = data2;
    sendLen = 5;
    break;

  case 1: // Direct raw MIDI: 3 bytes
    frame[0] = status;
    frame[1] = data1;
    frame[2] = data2;
    sendLen = 3;
    break;

  case 2: // Padded to full 64 bytes
    frame[0] = 0x03;
    frame[1] = 0x00;
    frame[2] = status;
    frame[3] = data1;
    frame[4] = data2;
    sendLen = 64;
    break;

  case 3: // 3-byte prefix (0x03, 0x00, len=3) + 3-byte MIDI payload = 6 bytes
    frame[0] = 0x03;
    frame[1] = 0x00;
    frame[2] = 0x03;
    frame[3] = status;
    frame[4] = data1;
    frame[5] = data2;
    sendLen = 6;
    break;

  default:
    frame[0] = 0x03;
    frame[1] = 0x00;
    frame[2] = status;
    frame[3] = data1;
    frame[4] = data2;
    sendLen = 5;
    break;
  }

  std::lock_guard<std::mutex> lock(g_UsbWriteMutex);
  ULONG bytesWritten = 0;
  BOOL ok = WinUsb_WritePipe(hWinUsb, outPipeId, frame, sendLen, &bytesWritten, nullptr);

  if (ok && bytesWritten == sendLen) {
    g_TotalLedCommandsSent.fetch_add(1);
    return true;
  }

  DWORD err = GetLastError();
  std::cerr << "  [-] Error en WinUsb_WritePipe (EP 0x" << std::hex << (int)outPipeId
            << std::dec << ", len " << sendLen << ", modo " << mode << "): "
            << FormatWin32Error(err) << "\n";
  return false;
}

/**
 * @brief Sets all LEDs on the BCD2000 controller to ON or OFF.
 */
void SetAllLedsState(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                     bool turnOn, int mode = -1) {
  uint8_t val = turnOn ? 0x7F : 0x00;

  // 1. Buttons (Note On 0x90, Channel 1: notes 0x00 to 0x27 / 0..39)
  for (uint8_t note = 0; note <= 0x27; ++note) {
    SendBcdSingleMidi(hWinUsb, outPipeId, 0x90, note, val, mode);
    g_LedStates[(0x90 << 8) | note] = turnOn;
    Sleep(1);
  }

  // 2. Buttons (Note On 0x91, Channel 2: notes 0x00 to 0x27 / 0..39)
  for (uint8_t note = 0; note <= 0x27; ++note) {
    SendBcdSingleMidi(hWinUsb, outPipeId, 0x91, note, val, mode);
    g_LedStates[(0x91 << 8) | note] = turnOn;
    Sleep(1);
  }

  // 3. Kill switches & FX Section (Control Change 0xB0: CC 0x00 to 0x27 / 0..39)
  for (uint8_t cc = 0; cc <= 0x27; ++cc) {
    SendBcdSingleMidi(hWinUsb, outPipeId, 0xB0, cc, val, mode);
    g_LedStates[(0xB0 << 8) | cc] = turnOn;
    Sleep(1);
  }
}

// ============================================================================
// BCD2000 MIDI Control Mapping Dictionary
// ============================================================================

struct ControlMapping {
  std::string type;
  std::string name;
  bool isContinuous;
};

ControlMapping LookupControl(uint8_t status, uint8_t channel, uint8_t data1) {
  uint8_t msgType = status & 0xF0;

  if (msgType == 0x90 || msgType == 0x80) { // Note On / Note Off (Buttons)
    std::string deck = (channel == 1 ? "Deck A " : (channel == 2 ? "Deck B " : "Ch" + std::to_string(channel) + " "));
    switch (data1) {
    case 0x00: return {"BUTTON", deck + "Loop In", false};
    case 0x01: return {"BUTTON", deck + "Loop Out", false};
    case 0x02: return {"BUTTON", deck + "Loop Exit / Reloop", false};
    case 0x03: return {"BUTTON", deck + "Cue 1", false};
    case 0x04: return {"BUTTON", deck + "Cue 2", false};
    case 0x05: return {"BUTTON", deck + "Cue 3", false};
    case 0x06: return {"BUTTON", deck + "Cue 4", false};
    case 0x07: return {"BUTTON", deck + "Search << (Back)", false};
    case 0x08: return {"BUTTON", deck + "Search >> (Fwd)", false};
    case 0x09: return {"BUTTON", deck + "Pitch Bend (-)", false};
    case 0x0A: return {"BUTTON", deck + "Pitch Bend (+)", false};
    case 0x0B: return {"BUTTON", deck + "Sync", false};
    case 0x0C: return {"BUTTON", deck + "Cue", false};
    case 0x0D: return {"BUTTON", deck + "Play / Pause", false};
    case 0x0E: return {"BUTTON", deck + "Headphone Cue (PFL)", false};
    case 0x0F: return {"BUTTON", deck + "Brake / FX", false};
    case 0x10: return {"BUTTON", deck + "Master Tempo (Keylock)", false};
    case 0x11: return {"BUTTON", deck + "Jog Touch Active", false};
    case 0x12: return {"BUTTON", deck + "Loop 1/2x", false};
    case 0x13: return {"BUTTON", deck + "Loop 2x", false};
    case 0x14: return {"BUTTON", deck + "Kill Switch Low", false};
    case 0x15: return {"BUTTON", deck + "Kill Switch Mid", false};
    case 0x16: return {"BUTTON", deck + "Kill Switch High", false};
    default:
      return {"BUTTON", deck + "Boton 0x" + HexByte(data1) + " (" + std::to_string(data1) + ")", false};
    }
  }

  if (msgType == 0xB0) { // Control Change (Faders, Knobs, Encoders, Kill Switches)
    switch (data1) {
    // Faders & Rotary Knobs
    case 0x00: return {"FADER", "Deck A Volume Slider", true};
    case 0x01: return {"FADER", "Crossfader Principal", true};
    case 0x02: return {"KNOB",  "Deck A EQ High", true};
    case 0x03: return {"KNOB",  "Deck A EQ Mid", true};
    case 0x04: return {"KNOB",  "Deck A EQ Low", true};
    case 0x05: return {"KNOB",  "Deck B EQ High", true};
    case 0x06: return {"KNOB",  "Deck B EQ Mid", true};
    case 0x07: return {"KNOB",  "Deck B EQ Low", true};
    case 0x08: return {"FADER", "Deck B Volume Slider", true};
    case 0x09: return {"KNOB",  "Master Level Knob", true};
    case 0x0A: return {"FADER", "Deck A Pitch Slider", true};
    case 0x0B: return {"FADER", "Deck B Pitch Slider", true};
    case 0x0C: return {"KNOB",  "Phones Level Knob", true};
    case 0x0D: return {"KNOB",  "Phones Mix (Cue/Master)", true};
    case 0x0E: return {"KNOB",  "Mic / External Input Level", true};
    case 0x0F: return {"KNOB",  "Deck A Gain Knob", true};
    case 0x10: return {"KNOB",  "Deck B Gain Knob", true};
    case 0x11: return {"KNOB",  "FX Parameter 1 Knob", true};
    case 0x12: return {"KNOB",  "FX Parameter 2 Knob", true};
    case 0x13: return {"JOG",   "Deck A Jog Wheel Rotary", false};
    case 0x14: return {"JOG",   "Deck B Jog Wheel Rotary", false};
    case 0x15: return {"BUTTON", "Deck A Kill Switch Low", false};
    case 0x16: return {"BUTTON", "Deck A Kill Switch Mid", false};
    case 0x17: return {"BUTTON", "Deck A Kill Switch High", false};
    case 0x18: return {"BUTTON", "Deck B Kill Switch Low", false};
    case 0x19: return {"BUTTON", "Deck B Kill Switch Mid", false};
    case 0x1A: return {"BUTTON", "Deck B Kill Switch High", false};
    case 0x1B: return {"FADER",  "Deck A Volume (Mixxx map)", true};
    case 0x1C: return {"BUTTON", "FX Section Button 1", false};
    case 0x1D: return {"BUTTON", "FX Section Button 2", false};
    case 0x1E: return {"BUTTON", "FX Section Button 3", false};
    case 0x1F: return {"BUTTON", "FX Section Button 4", false};
    default:
      return {"KNOB", "Control Change CC# " + std::to_string(data1) + " (0x" + HexByte(data1) + ")", true};
    }
  }

  if (msgType == 0xE0) { // Pitch Bend
    std::string deck = (channel == 1 ? "Deck A " : "Deck B ");
    return {"PITCH", deck + "High-Res Pitch Bend", true};
  }

  return {"MIDI", "Evento MIDI 0x" + HexByte(status), false};
}

// ============================================================================
// Asynchronous Endpoint 0x81 (IN) Reader Thread
// ============================================================================

void MidiReaderThreadWorker(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR inPipeId,
                            UCHAR outPipeId) {
  // Configure IN pipe timeout (100 ms) and Auto-Clear Stall
  ULONG inTimeoutMs = 100;
  WinUsb_SetPipePolicy(hWinUsb, inPipeId, PIPE_TRANSFER_TIMEOUT,
                       sizeof(inTimeoutMs), &inTimeoutMs);
  UCHAR autoClearStall = 1;
  WinUsb_SetPipePolicy(hWinUsb, inPipeId, AUTO_CLEAR_STALL,
                       sizeof(autoClearStall), &autoClearStall);

  uint8_t buffer[64];

  while (g_Running.load()) {
    ULONG bytesRead = 0;
    BOOL ok = WinUsb_ReadPipe(hWinUsb, inPipeId, buffer, sizeof(buffer),
                              &bytesRead, nullptr);

    if (ok && bytesRead > 0) {
      g_TotalPacketsReceived.fetch_add(1);

      // Build raw hex representation of the received packet
      std::ostringstream rawOss;
      for (ULONG b = 0; b < bytesRead && b < 8; ++b) {
        rawOss << HexByte(buffer[b]) << " ";
      }
      if (bytesRead > 8) rawOss << "...";
      std::string rawPacketStr = rawOss.str();

      // Flexible extraction:
      // In BCD2000, buffer[0] is typically payload length (e.g. 3, 6, 9) and buffer[1] starts MIDI.
      // Or buffer[0] == 0x03, buffer[1] == 0x00 and buffer[2] starts MIDI.
      // Or buffer starts directly with a MIDI status byte (>= 0x80).
      // If none of those match, scan the buffer for any byte >= 0x80.
      ULONG midiStart = 0;
      ULONG midiLen = 0;

      if (buffer[0] <= 63 && buffer[0] > 0 && bytesRead > 1 && (buffer[1] & 0x80) != 0) {
        midiStart = 1;
        midiLen = std::min<ULONG>(static_cast<ULONG>(buffer[0]), bytesRead - 1);
      } else if (bytesRead >= 3 && buffer[0] == 0x03 && buffer[1] == 0x00 && (buffer[2] & 0x80) != 0) {
        midiStart = 2;
        midiLen = bytesRead - 2;
      } else if ((buffer[0] & 0x80) != 0) {
        midiStart = 0;
        midiLen = bytesRead;
      } else {
        // Fallback: search for first byte with bit 7 set
        for (ULONG k = 0; k < bytesRead; ++k) {
          if ((buffer[k] & 0x80) != 0) {
            midiStart = k;
            midiLen = bytesRead - k;
            break;
          }
        }
      }

      bool anyEventDecoded = false;

      if (midiLen >= 2) {
        ULONG idx = midiStart;
        while (idx < midiStart + midiLen) {
          uint8_t status = buffer[idx];
          if ((status & 0x80) == 0) {
            idx++;
            continue;
          }

          uint8_t msgType = status & 0xF0;
          uint8_t channel = (status & 0x0F) + 1;

          // 3-byte messages: 0x80 (Note Off), 0x90 (Note On), 0xA0 (Pressure),
          //                  0xB0 (CC), 0xE0 (Pitch Bend)
          // 2-byte messages: 0xC0 (Program Change), 0xD0 (Channel Pressure)
          bool is3Byte = (msgType == 0x80 || msgType == 0x90 || msgType == 0xA0 ||
                          msgType == 0xB0 || msgType == 0xE0);

          if (is3Byte && (idx + 2 < midiStart + midiLen)) {
            uint8_t d1 = buffer[idx + 1];
            uint8_t d2 = buffer[idx + 2];
            anyEventDecoded = true;

            DecodedControlEvent ev;
            ev.timestamp = std::chrono::system_clock::now();
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

            // Continuous controller (FADER or KNOB): ALWAYS show percentage progress bar!
            if (msgType == 0xB0) {
              if (mapping.type == "JOG") {
                int delta = (d2 & 0x40) ? -(d2 & 0x3F) : (d2 & 0x3F);
                ev.detailValue = (delta >= 0 ? "+" : "") + std::to_string(delta) +
                                 " ticks (" + (delta > 0 ? "CW" : "CCW") + ")";
              } else if (mapping.type == "BUTTON") {
                bool isPressed = (d2 >= 0x40);
                ev.detailValue = isPressed ? "PRESIONADO (Down)" : "LIBERADO (Up)";
                if (isPressed) {
                  bool newLed = !g_LedStates[stateKey];
                  g_LedStates[stateKey] = newLed;
                  ev.ledToggled = true;
                  ev.newLedState = newLed;
                  SendBcdSingleMidi(hWinUsb, outPipeId, status, d1, newLed ? 0x7F : 0x00);
                }
              } else {
                // Continuous CC (Fader, Knob, EQ, Gain, etc.)
                ev.detailValue = FormatProgressBar(d2) + " [Val: " + std::to_string(d2) + "/127]";
              }
            } else if (msgType == 0x90 || msgType == 0x80) { // Button Note
              bool isPressed = (msgType == 0x90 && d2 > 0);
              ev.detailValue = isPressed ? "PRESIONADO (Down)" : "LIBERADO (Up)";
              if (isPressed) {
                bool newLed = !g_LedStates[stateKey];
                g_LedStates[stateKey] = newLed;
                ev.ledToggled = true;
                ev.newLedState = newLed;
                SendBcdSingleMidi(hWinUsb, outPipeId, 0x90, d1, newLed ? 0x7F : 0x00);
              }
            } else if (msgType == 0xE0) { // Pitch Bend
              int pitchVal = (static_cast<int>(d2) << 7) | static_cast<int>(d1);
              int normalized = pitchVal - 8192;
              ev.detailValue = FormatBipolarBar(normalized, 8192) + " [Raw: " + std::to_string(pitchVal) + "]";
            } else {
              ev.detailValue = "Data1: " + std::to_string(d1) + ", Data2: " + std::to_string(d2);
            }

            g_TotalEventsDecoded.fetch_add(1);

            {
              std::lock_guard<std::mutex> lock(g_EventMutex);
              g_EventQueue.push(ev);
            }
            idx += 3;
          } else if (!is3Byte && (idx + 1 < midiStart + midiLen)) {
            // 2-byte MIDI message
            uint8_t d1 = buffer[idx + 1];
            anyEventDecoded = true;

            DecodedControlEvent ev;
            ev.timestamp = std::chrono::system_clock::now();
            ev.status = status;
            ev.channel = channel;
            ev.data1 = d1;
            ev.data2 = 0;
            ev.rawHex = rawPacketStr;
            ev.controlType = "MIDI";
            ev.controlName = "MIDI Ch" + std::to_string(channel) + " Status 0x" + HexByte(status);
            ev.detailValue = "Data: " + std::to_string(d1);

            g_TotalEventsDecoded.fetch_add(1);

            {
              std::lock_guard<std::mutex> lock(g_EventMutex);
              g_EventQueue.push(ev);
            }
            idx += 2;
          } else {
            idx++;
          }
        }
      }

      // If bytes were received but couldn't be parsed into standard MIDI, display raw hex
      if (!anyEventDecoded && bytesRead > 0) {
        std::ostringstream oss;
        oss << "Bytes (" << bytesRead << "): ";
        for (ULONG b = 0; b < bytesRead && b < 16; ++b) {
          oss << HexByte(buffer[b]) << " ";
        }
        DecodedControlEvent rawEv;
        rawEv.timestamp = std::chrono::system_clock::now();
        rawEv.controlType = "RAW_USB";
        rawEv.controlName = "Trama USB EP 0x81";
        rawEv.detailValue = oss.str();
        rawEv.rawHex = rawPacketStr;

        std::lock_guard<std::mutex> lock(g_EventMutex);
        g_EventQueue.push(rawEv);
      }
    } else {
      DWORD err = GetLastError();
      if (err != ERROR_SEM_TIMEOUT) {
        if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE) {
          std::cerr << "\n[!] Comunicacion USB perdida: "
                    << FormatWin32Error(err) << "\n";
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
// SetupAPI Hardware Link Initializer
// ============================================================================

bool InitializeBcdDevice(HANDLE &outDeviceHandle,
                         WINUSB_INTERFACE_HANDLE &outWinUsbHandle,
                         UCHAR &outPipeId, UCHAR &inPipeId) {
  outDeviceHandle = INVALID_HANDLE_VALUE;
  outWinUsbHandle = nullptr;
  outPipeId = 0x01;
  inPipeId = 0x81;

  std::cout << "[USB] Escaneando dispositivos USB mediante SetupAPI para VID 1397 / PID 00BD...\n";

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
      if (reqSize == 0) continue;

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
    std::cerr
        << "  [-] No se encontro la interfaz WinUSB para la consola BCD2000.\n"
        << "      Verifica que el dispositivo este encendido y configurado con "
           "WinUSB en Zadig.\n";
    return false;
  }

  std::cout << "  [+] Interfaz detectada: " << WideToUtf8(targetPath) << "\n";

  outDeviceHandle =
      CreateFileW(targetPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);

  if (outDeviceHandle == INVALID_HANDLE_VALUE) {
    std::cerr << "  [-] CreateFileW fallo: " << FormatWin32Error(GetLastError())
              << "\n";
    return false;
  }

  if (!WinUsb_Initialize(outDeviceHandle, &outWinUsbHandle)) {
    std::cerr << "  [-] WinUsb_Initialize fallo: "
              << FormatWin32Error(GetLastError()) << "\n";
    CloseHandle(outDeviceHandle);
    outDeviceHandle = INVALID_HANDLE_VALUE;
    return false;
  }

  // Strictly lock Control & Telemetry Pipes: EP 0x01 (OUT) and EP 0x81 (IN)
  outPipeId = 0x01;
  inPipeId = 0x81;

  // Configure Pipe Policies on Endpoint 0x01 (OUT)
  ULONG timeoutMs = 2000;
  WinUsb_SetPipePolicy(outWinUsbHandle, outPipeId, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
  UCHAR autoClearStall = 1;
  WinUsb_SetPipePolicy(outWinUsbHandle, outPipeId, AUTO_CLEAR_STALL, sizeof(autoClearStall), &autoClearStall);

  std::cout << "  [+] WinUSB inicializado con exito.\n"
            << "      Endpoint OUT: 0x" << std::hex << (int)outPipeId
            << " (Control/MIDI) | Endpoint IN: 0x" << (int)inPipeId << " (Telemetria)" << std::dec << "\n\n";

  // Transmit 52-byte handshake sequence
  std::cout << "[HANDSHAKE] Transmitiendo trama de inicializacion de 52 bytes al microcontrolador 8051...\n";
  ULONG bytesWritten = 0;
  BOOL ok = WinUsb_WritePipe(
      outWinUsbHandle, outPipeId, const_cast<PUCHAR>(BCD2000_INIT_SEQUENCE),
      sizeof(BCD2000_INIT_SEQUENCE), &bytesWritten, nullptr);

  if (!ok || bytesWritten != sizeof(BCD2000_INIT_SEQUENCE)) {
    std::cerr << "  [-] Error en handshake: "
              << FormatWin32Error(GetLastError()) << "\n";
    WinUsb_Free(outWinUsbHandle);
    CloseHandle(outDeviceHandle);
    return false;
  }

  std::cout << "  [+] Handshake exitoso (" << bytesWritten
            << " bytes transmitidos). Microcontrolador activo.\n\n";
  Sleep(150); // Allow hardware MCU registers to settle
  return true;
}

// ============================================================================
// Main Application Routine
// ============================================================================

int main() {
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  std::cout << "======================================================================\n";
  std::cout << "   Behringer BCD2000 Telemetria de Controles y Retorno a LEDs (Hito 2)\n";
  std::cout << "   Target: VID 0x1397, PID 0x00BD | Driver: WinUSB (x64)             \n";
  std::cout << "======================================================================\n\n";

  HANDLE hDevice = INVALID_HANDLE_VALUE;
  WINUSB_INTERFACE_HANDLE hWinUsb = nullptr;
  UCHAR outPipeId = 0x01;
  UCHAR inPipeId = 0x81;

  if (!InitializeBcdDevice(hDevice, hWinUsb, outPipeId, inPipeId)) {
    return 1;
  }

  // --------------------------------------------------------------------------
  // Visual LED Greeting Sweep (Sequential Illumination Test)
  // --------------------------------------------------------------------------
  std::cout << "======================================================================\n";
  std::cout << "       VERIFICACION FISICA DE RETORNO A LEDS (SALUDO VISUAL)          \n";
  std::cout << "======================================================================\n";
  std::cout << "  Modo de transmision inicial: " << GetModeName(g_LedSendMode.load()) << "\n";
  std::cout << "  [Fase 1] Encendiendo todos los LEDs (Deck A, Deck B, Kill Switches)...\n";

  SetAllLedsState(hWinUsb, outPipeId, true, g_LedSendMode.load());
  std::cout << "  [+] Comandos LED ON transmitidos (" << g_TotalLedCommandsSent.load() << " paquetes a EP 0x01).\n";
  std::cout << "  >>> INSPECCIONA TU CONSOLA BCD2000 FISICA AHORA: <<<\n";

  for (int sec = 3; sec > 0; --sec) {
    std::cout << "      Manteniendo LEDs encendidos: " << sec << " segundos restantes...\r" << std::flush;
    Sleep(1000);
  }
  std::cout << "      Manteniendo LEDs encendidos: listo!                                \n";

  std::cout << "  [Fase 2] Apagando LEDs para iniciar monitor en vivo...\n";
  SetAllLedsState(hWinUsb, outPipeId, false, g_LedSendMode.load());
  std::cout << "  [+] Ciclo de saludo completado. Si no viste los LEDs encendidos,\n";
  std::cout << "      usa los comandos [1], [2], [3], [4] o [M] para alternar modos.\n\n";

  std::cout << "======================================================================\n";
  std::cout << "                 MODO MONITOR EN VIVO Y CONTROL BIDIRECCIONAL         \n";
  std::cout << "======================================================================\n";
  std::cout << "  INSTRUCCIONES:\n";
  std::cout << "  - Mueve Faders, Knobs o Pitch: se mostrara la BARRA DE % EN VIVO.\n";
  std::cout << "  - Gira Ruedas Jog: se mostrara el delta de ticks (CW / CCW).\n";
  std::cout << "  - Presiona cualquier Boton: se decodificara y SU LED CONMUTARA EN LA CONSOLA.\n";
  std::cout << "  --------------------------------------------------------------------\n";
  std::cout << "  COMANDOS DE TECLADO INTERACTIVOS:\n";
  std::cout << "      [L] : Disparar encendido permanente de TODOS los LEDs.\n";
  std::cout << "      [A] : Apagar TODOS los LEDs.\n";
  std::cout << "      [M] : Alternar ciclicamente el modo de envio a LEDs.\n";
  std::cout << "      [1] : Forzar Modo A (5 bytes: [0x03, 0x00, status, d1, d2] - Linux).\n";
  std::cout << "      [2] : Forzar Modo B (3 bytes: [status, d1, d2] - MIDI puro).\n";
  std::cout << "      [3] : Forzar Modo C (64 bytes: [0x03, 0x00, status, d1, d2, pad 0]).\n";
  std::cout << "      [4] : Forzar Modo D (6 bytes: [0x03, 0x00, 0x03, status, d1, d2]).\n";
  std::cout << "      [T] : Mostrar tabla resumen de estado de todos los controles activos.\n";
  std::cout << "      [C] : Limpiar pantalla de consola.\n";
  std::cout << "      [Q] o ESC : Salir ordenadamente del programa.\n";
  std::cout << "----------------------------------------------------------------------\n\n";

  // Launch background worker thread for Endpoint 0x81
  std::thread readerThread(MidiReaderThreadWorker, hWinUsb, inPipeId, outPipeId);

  // Main UI & Event Dispatch Loop
  while (g_Running.load()) {
    // 1. Process queued decoded control events
    {
      std::lock_guard<std::mutex> lock(g_EventMutex);
      while (!g_EventQueue.empty()) {
        auto ev = g_EventQueue.front();
        g_EventQueue.pop();

        std::cout << std::left
                  << std::setw(10) << ("[" + ev.controlType + "]")
                  << std::setw(30) << ev.controlName
                  << " " << std::setw(28) << ev.detailValue;

        if (ev.ledToggled) {
          std::cout << " ==> [LED " << (ev.newLedState ? "ENCENDIDO" : "APAGADO") << "]";
        }

        std::cout << "  (USB: " << ev.rawHex << ")\n";
      }
    }

    // 2. Process non-blocking keyboard commands
    if (_kbhit()) {
      int ch = _getch();
      if (ch == 'q' || ch == 'Q' || ch == 27) { // Q or ESC
        std::cout << "\n[INFO] Solicitud de salida recibida. Finalizando...\n";
        g_Running.store(false);
        break;
      } else if (ch == 'l' || ch == 'L') {
        std::cout << "\n[TEST] Encendiendo TODOS los LEDs (" << GetModeName(g_LedSendMode.load()) << ")...\n";
        SetAllLedsState(hWinUsb, outPipeId, true, g_LedSendMode.load());
        std::cout << "  [+] Comandos LED ON enviados. Permaneceran encendidos hasta que presiones [A].\n\n";
      } else if (ch == 'a' || ch == 'A') {
        std::cout << "\n[TEST] Apagando TODOS los LEDs (" << GetModeName(g_LedSendMode.load()) << ")...\n";
        SetAllLedsState(hWinUsb, outPipeId, false, g_LedSendMode.load());
        std::cout << "  [+] Comandos LED OFF enviados.\n\n";
      } else if (ch == 'm' || ch == 'M') {
        int nextMode = (g_LedSendMode.load() + 1) % 4;
        g_LedSendMode.store(nextMode);
        std::cout << "\n[MODO] Modo de envio de LEDs cambiado a: " << GetModeName(nextMode) << "\n";
        std::cout << "       Probando encendido de LEDs con el nuevo modo...\n";
        SetAllLedsState(hWinUsb, outPipeId, true, nextMode);
      } else if (ch == '1') {
        g_LedSendMode.store(0);
        std::cout << "\n[MODO] Seleccionado Modo 1: " << GetModeName(0) << "\n";
        SetAllLedsState(hWinUsb, outPipeId, true, 0);
      } else if (ch == '2') {
        g_LedSendMode.store(1);
        std::cout << "\n[MODO] Seleccionado Modo 2: " << GetModeName(1) << "\n";
        SetAllLedsState(hWinUsb, outPipeId, true, 1);
      } else if (ch == '3') {
        g_LedSendMode.store(2);
        std::cout << "\n[MODO] Seleccionado Modo 3: " << GetModeName(2) << "\n";
        SetAllLedsState(hWinUsb, outPipeId, true, 2);
      } else if (ch == '4') {
        g_LedSendMode.store(3);
        std::cout << "\n[MODO] Seleccionado Modo 4: " << GetModeName(3) << "\n";
        SetAllLedsState(hWinUsb, outPipeId, true, 3);
      } else if (ch == 'c' || ch == 'C') {
        system("cls");
        std::cout << "[INFO] Pantalla limpiada. Modo monitor en vivo activo...\n\n";
      } else if (ch == 't' || ch == 'T') {
        std::cout << "\n======================================================================\n";
        std::cout << "                    TABLA RESUMEN DE ESTADO DE CONTROLES              \n";
        std::cout << "======================================================================\n";
        std::cout << "  Eventos decodificados : " << g_TotalEventsDecoded.load() << "\n";
        std::cout << "  Tramas USB recibidas   : " << g_TotalPacketsReceived.load() << "\n";
        std::cout << "  Comandos LED enviados  : " << g_TotalLedCommandsSent.load() << "\n";
        std::cout << "  Modo de envio LED actual: " << GetModeName(g_LedSendMode.load()) << "\n";
        std::cout << "----------------------------------------------------------------------\n";
        for (const auto &kv : g_ControlValues) {
          uint8_t st = static_cast<uint8_t>(kv.first >> 8);
          uint8_t d1 = static_cast<uint8_t>(kv.first & 0xFF);
          uint8_t val = kv.second;
          auto map = LookupControl(st, (st & 0x0F) + 1, d1);
          std::cout << "  " << std::left << std::setw(32) << map.name << " = "
                    << std::setw(4) << static_cast<int>(val) << " "
                    << FormatProgressBar(val, 14) << "\n";
        }
        std::cout << "======================================================================\n\n";
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  // Clean Shutdown
  std::cout << "\n[CLEANUP] Deteniendo hilo de lectura y apagando LEDs...\n";
  if (readerThread.joinable()) {
    readerThread.join();
  }

  // Ensure all LEDs are cleanly turned off before closing
  SetAllLedsState(hWinUsb, outPipeId, false, g_LedSendMode.load());

  WinUsb_Free(hWinUsb);
  CloseHandle(hDevice);

  std::cout << "  [+] Recursos liberados cleanly. Total eventos capturados: "
            << g_TotalEventsDecoded.load() << "\n";
  std::cout << "======================================================================\n";
  std::cout << "             HITO 2: TELEMETRIA Y CONTROL MIDI COMPLETADO             \n";
  std::cout << "======================================================================\n";

  return 0;
}
