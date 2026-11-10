/**
 * @file bcd2000_diag_midi.cpp
 * @brief Behringer BCD2000 Physical Control Telemetry and Bi-Directional LED Engine
 *
 * Part of Milestone 2 ("Telemetría de Controles Físicos y Retorno a LEDs")
 * Target Hardware: Behringer BCD2000 DJ Controller (VID: 0x1397, PID: 0x00BD)
 * Driver: WinUSB (x64)
 *
 * Architecture:
 *  - Dynamically discovers all WinUSB interfaces (Interface 0 and Associated Interfaces).
 *  - Correctly routes MIDI/LED traffic to the interface owning Interrupt Endpoints 0x01 / 0x81.
 *  - Correctly identifies Audio Streaming traffic on Isochronous Endpoints 0x02 / 0x83.
 *  - Transmits 52-byte 8051 Handshake sequence on the proper MIDI OUT pipe.
 *  - Continuously captures real-time control events on Endpoint 0x81 (IN).
 *  - Real-time progress bars: [========|       ] 50% for all faders and knobs.
 *  - Bi-directional hardware LED feedback for buttons and kill switches on Endpoint 0x01 (OUT).
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
// USB Device Context & Topology
// ============================================================================

struct BcdUsbContext {
  HANDLE hDevice = INVALID_HANDLE_VALUE;
  WINUSB_INTERFACE_HANDLE hInterface0 = nullptr;
  WINUSB_INTERFACE_HANDLE hMidiInterface = nullptr;
  WINUSB_INTERFACE_HANDLE hAudioInterface = nullptr;
  std::vector<WINUSB_INTERFACE_HANDLE> allInterfaces;

  UCHAR midiOutPipe = 0x01;
  UCHAR midiInPipe = 0x81;
  UCHAR audioOutPipe = 0x02;
  UCHAR audioInPipe = 0x83;
};

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
static std::map<uint16_t, bool> g_LedStates;
static std::map<uint16_t, uint8_t> g_ControlValues;
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
// USB Transmission Engine (Endpoint 0x01 OUT on MIDI Interface)
// ============================================================================

bool SendBcdSingleMidi(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                       uint8_t status, uint8_t data1, uint8_t data2,
                       int mode = -1) {
  if (!hWinUsb || outPipeId == 0) return false;
  if (mode < 0) mode = g_LedSendMode.load();

  uint8_t frame[64] = {0};
  ULONG sendLen = 0;

  switch (mode) {
  case 0: // Linux kernel snd-bcd2000: 2-byte prefix + 3-byte MIDI = 5 bytes
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

  case 3: // 3-byte prefix (0x03, 0x00, len=3) + 3-byte MIDI = 6 bytes
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
 * @brief Sets all LEDs on the BCD2000 controller to ON or OFF with gentle timing.
 */
void SetAllLedsState(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                     bool turnOn, int mode = -1) {
  uint8_t val = turnOn ? 0x7F : 0x00;

  // Deck A & Transport Buttons (Note On 0x90, notes 0 to 19 / 0x13)
  for (uint8_t note = 0; note <= 0x13; ++note) {
    SendBcdSingleMidi(hWinUsb, outPipeId, 0x90, note, val, mode);
    g_LedStates[(0x90 << 8) | note] = turnOn;
    Sleep(3);
  }

  // Deck B Buttons (Note On 0x91, notes 0 to 19 / 0x13)
  for (uint8_t note = 0; note <= 0x13; ++note) {
    SendBcdSingleMidi(hWinUsb, outPipeId, 0x91, note, val, mode);
    g_LedStates[(0x91 << 8) | note] = turnOn;
    Sleep(3);
  }

  // Kill switches & FX (Control Change 0xB0, CC 0x10 to 0x1F)
  for (uint8_t cc = 0x10; cc <= 0x1F; ++cc) {
    SendBcdSingleMidi(hWinUsb, outPipeId, 0xB0, cc, val, mode);
    g_LedStates[(0xB0 << 8) | cc] = turnOn;
    Sleep(3);
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
    default:
      return {"BUTTON", deck + "Boton 0x" + HexByte(data1) + " (" + std::to_string(data1) + ")", false};
    }
  }

  if (msgType == 0xB0) { // Control Change (Faders, Knobs, Encoders, Kill Switches)
    switch (data1) {
    case 0x00: return {"FADER",  "Deck A Volume Slider", true};
    case 0x01: return {"FADER",  "Crossfader Principal", true};
    case 0x02: return {"KNOB",   "Deck A EQ High", true};
    case 0x03: return {"KNOB",   "Deck A EQ Mid", true};
    case 0x04: return {"KNOB",   "Deck A EQ Low", true};
    case 0x05: return {"KNOB",   "Deck B EQ High", true};
    case 0x06: return {"KNOB",   "Deck B EQ Mid", true};
    case 0x07: return {"KNOB",   "Deck B EQ Low", true};
    case 0x08: return {"FADER",  "Deck B Volume Slider", true};
    case 0x09: return {"KNOB",   "Master Level Knob", true};
    case 0x0A: return {"FADER",  "Deck A Pitch Slider", true};
    case 0x0B: return {"FADER",  "Deck B Pitch Slider", true};
    case 0x0C: return {"KNOB",   "Phones Level Knob", true};
    case 0x0D: return {"KNOB",   "Phones Mix (Cue/Master)", true};
    case 0x0E: return {"KNOB",   "Mic / External Input Level", true};
    case 0x0F: return {"KNOB",   "Deck A Gain Knob", true};
    case 0x10: return {"KNOB",   "Deck B Gain Knob", true};
    case 0x11: return {"KNOB",   "FX Parameter 1 Knob", true};
    case 0x12: return {"KNOB",   "FX Parameter 2 Knob", true};
    case 0x13: return {"JOG",    "Deck A Jog Wheel Rotary", false};
    case 0x14: return {"JOG",    "Deck B Jog Wheel Rotary", false};
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

void MidiReaderThreadWorker(WINUSB_INTERFACE_HANDLE hWinUsbMidi, UCHAR inPipeId,
                            UCHAR outPipeId) {
  ULONG inTimeoutMs = 100;
  WinUsb_SetPipePolicy(hWinUsbMidi, inPipeId, PIPE_TRANSFER_TIMEOUT,
                       sizeof(inTimeoutMs), &inTimeoutMs);
  UCHAR autoClearStall = 1;
  WinUsb_SetPipePolicy(hWinUsbMidi, inPipeId, AUTO_CLEAR_STALL,
                       sizeof(autoClearStall), &autoClearStall);

  uint8_t buffer[64];

  while (g_Running.load()) {
    ULONG bytesRead = 0;
    BOOL ok = WinUsb_ReadPipe(hWinUsbMidi, inPipeId, buffer, sizeof(buffer),
                              &bytesRead, nullptr);

    if (ok && bytesRead > 0) {
      g_TotalPacketsReceived.fetch_add(1);

      // Build raw hex representation
      std::ostringstream rawOss;
      for (ULONG b = 0; b < bytesRead && b < 12; ++b) {
        rawOss << HexByte(buffer[b]) << " ";
      }
      if (bytesRead > 12) rawOss << "...";
      std::string rawPacketStr = rawOss.str();

      // Framing determination:
      // Linux snd-bcd2000: buffer[0] is length, buffer[1..] are MIDI bytes
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
        // Search for any byte with bit 7 set
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
          bool is3Byte = (msgType == 0x80 || msgType == 0x90 || msgType == 0xA0 ||
                          msgType == 0xB0 || msgType == 0xE0);

          if (is3Byte) {
            if (idx + 2 < midiStart + midiLen) {
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
                    SendBcdSingleMidi(hWinUsbMidi, outPipeId, status, d1, newLed ? 0x7F : 0x00);
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
                  SendBcdSingleMidi(hWinUsbMidi, outPipeId, 0x90, d1, newLed ? 0x7F : 0x00);
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
              continue;
            }
          } else {
            if (idx + 1 < midiStart + midiLen) {
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
              continue;
            }
          }
          idx++;
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
        rawEv.controlName = "Trama USB EP 0x" + HexByte(inPipeId);
        rawEv.detailValue = oss.str();
        rawEv.rawHex = rawPacketStr;

        std::lock_guard<std::mutex> lock(g_EventMutex);
        g_EventQueue.push(rawEv);
      }
    } else {
      DWORD err = GetLastError();
      if (err != ERROR_SEM_TIMEOUT) {
        if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE || err == ERROR_DEV_NOT_EXIST) {
          std::cerr << "\n[!] Comunicacion USB perdida en EP 0x" << std::hex << (int)inPipeId
                    << ": " << FormatWin32Error(err) << "\n";
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
// Robust SetupAPI & WinUSB Multi-Interface Initializer
// ============================================================================

bool InitializeBcdDevice(BcdUsbContext &ctx) {
  ctx.hDevice = INVALID_HANDLE_VALUE;
  ctx.hInterface0 = nullptr;
  ctx.hMidiInterface = nullptr;
  ctx.hAudioInterface = nullptr;
  ctx.allInterfaces.clear();

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
      SetupDiGetDeviceInterfaceDetailW(hIntfInfo, &ifData, nullptr, 0, &reqSize, nullptr);
      if (reqSize == 0) continue;

      std::vector<BYTE> buf(reqSize);
      auto detailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
      detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

      if (SetupDiGetDeviceInterfaceDetailW(hIntfInfo, &ifData, detailData, reqSize, nullptr, nullptr)) {
        std::wstring path(detailData->DevicePath);
        if (ContainsInsensitive(path, vidStr) && ContainsInsensitive(path, pidStr)) {
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
        << "      Verifica que el dispositivo este encendido y configurado con WinUSB en Zadig.\n";
    return false;
  }

  std::cout << "  [+] Interfaz detectada: " << WideToUtf8(targetPath) << "\n";

  ctx.hDevice = CreateFileW(targetPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);

  if (ctx.hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "  [-] CreateFileW fallo: " << FormatWin32Error(GetLastError()) << "\n";
    return false;
  }

  // 1. Initialize Interface 0
  if (!WinUsb_Initialize(ctx.hDevice, &ctx.hInterface0)) {
    std::cerr << "  [-] WinUsb_Initialize fallo: " << FormatWin32Error(GetLastError()) << "\n";
    CloseHandle(ctx.hDevice);
    ctx.hDevice = INVALID_HANDLE_VALUE;
    return false;
  }
  ctx.allInterfaces.push_back(ctx.hInterface0);

  // 2. Discover Associated Interfaces (Interface 1, Interface 2, etc.)
  std::vector<WINUSB_INTERFACE_HANDLE> interfaceHandles;
  interfaceHandles.push_back(ctx.hInterface0);

  WINUSB_INTERFACE_HANDLE hAssoc = nullptr;
  UCHAR assocIdx = 0;
  while (WinUsb_GetAssociatedInterface(ctx.hInterface0, assocIdx, &hAssoc)) {
    interfaceHandles.push_back(hAssoc);
    ctx.allInterfaces.push_back(hAssoc);
    assocIdx++;
  }

  std::cout << "\n======================================================================\n";
  std::cout << "              TOPOLOGIA DE INTERFACES Y PIPES WINUSB                  \n";
  std::cout << "======================================================================\n";
  std::cout << "  Total interfaces WinUSB disponibles: " << interfaceHandles.size() << "\n\n";

  // 3. Scan all pipes on each interface
  for (size_t intfIdx = 0; intfIdx < interfaceHandles.size(); ++intfIdx) {
    auto hIntf = interfaceHandles[intfIdx];
    USB_INTERFACE_DESCRIPTOR ifDesc{};
    if (!WinUsb_QueryInterfaceSettings(hIntf, 0, &ifDesc)) {
      std::cerr << "  [-] No se pudo consultar configuración de interfaz #" << intfIdx << "\n";
      continue;
    }

    std::cout << "  [+] Interfaz WinUSB #" << intfIdx
              << " (bInterfaceNumber: " << (int)ifDesc.bInterfaceNumber
              << ", Endpoints: " << (int)ifDesc.bNumEndpoints << "):\n";

    for (UCHAR p = 0; p < ifDesc.bNumEndpoints; ++p) {
      WINUSB_PIPE_INFORMATION pi{};
      if (WinUsb_QueryPipe(hIntf, 0, p, &pi)) {
        bool isInput = (USB_ENDPOINT_DIRECTION_IN(pi.PipeId) != 0);
        std::string typeStr;
        switch (pi.PipeType) {
        case UsbdPipeTypeControl: typeStr = "Control"; break;
        case UsbdPipeTypeIsochronous: typeStr = "Isochronous"; break;
        case UsbdPipeTypeBulk: typeStr = "Bulk"; break;
        case UsbdPipeTypeInterrupt: typeStr = "Interrupt"; break;
        }

        std::cout << "      Pipe 0x" << std::hex << std::setw(2) << std::setfill('0')
                  << (int)pi.PipeId << std::dec << " [" << (isInput ? "IN " : "OUT")
                  << " | " << typeStr << " | " << pi.MaximumPacketSize << " bytes | Interval: "
                  << (int)pi.Interval << "]\n";

        // Route MIDI Control pipes (prioritize 0x01 and 0x81, or Interrupt pipes)
        if (pi.PipeId == 0x01) {
          ctx.hMidiInterface = hIntf;
          ctx.midiOutPipe = 0x01;
        } else if (pi.PipeId == 0x81) {
          ctx.midiInPipe = 0x81;
          if (!ctx.hMidiInterface) ctx.hMidiInterface = hIntf;
        } else if (pi.PipeType == UsbdPipeTypeInterrupt) {
          if (!isInput && ctx.midiOutPipe != 0x01) {
            ctx.hMidiInterface = hIntf;
            ctx.midiOutPipe = pi.PipeId;
          } else if (isInput && ctx.midiInPipe != 0x81) {
            ctx.midiInPipe = pi.PipeId;
          }
        }

        // Route Audio pipes (prioritize 0x02 and 0x83, or Isochronous pipes)
        if (pi.PipeId == 0x02) {
          ctx.hAudioInterface = hIntf;
          ctx.audioOutPipe = 0x02;
        } else if (pi.PipeId == 0x83) {
          ctx.audioInPipe = 0x83;
          if (!ctx.hAudioInterface) ctx.hAudioInterface = hIntf;
        }
      }
    }
  }

  // Fallbacks if not uniquely mapped
  if (!ctx.hMidiInterface) ctx.hMidiInterface = ctx.hInterface0;
  if (!ctx.hAudioInterface) ctx.hAudioInterface = ctx.hInterface0;

  int midiIntfNum = 0;
  for (size_t i = 0; i < interfaceHandles.size(); ++i) {
    if (interfaceHandles[i] == ctx.hMidiInterface) {
      midiIntfNum = static_cast<int>(i);
      break;
    }
  }

  std::cout << "\n  [ENRUTAMIENTO WINUSB]\n"
            << "    * MIDI / Control OUT : Interfaz #" << midiIntfNum
            << " -> Pipe 0x" << std::hex << (int)ctx.midiOutPipe << std::dec << "\n"
            << "    * Telemetria IN      : Interfaz #" << midiIntfNum
            << " -> Pipe 0x" << std::hex << (int)ctx.midiInPipe << std::dec << "\n"
            << "    * Audio Iso OUT      : Pipe 0x" << std::hex << (int)ctx.audioOutPipe << std::dec << "\n"
            << "    * Audio Iso IN       : Pipe 0x" << std::hex << (int)ctx.audioInPipe << std::dec << "\n";
  std::cout << "======================================================================\n\n";

  // Configure Policies on MIDI Pipes
  ULONG timeoutMs = 2000;
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiOutPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
  UCHAR autoClearStall = 1;
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiOutPipe, AUTO_CLEAR_STALL, sizeof(autoClearStall), &autoClearStall);

  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiInPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiInPipe, AUTO_CLEAR_STALL, sizeof(autoClearStall), &autoClearStall);

  // 4. Transmit 52-byte handshake sequence to the correct MIDI OUT interface handle
  std::cout << "[HANDSHAKE] Transmitiendo trama de inicializacion de 52 bytes al microcontrolador 8051...\n";
  ULONG bytesWritten = 0;
  BOOL ok = WinUsb_WritePipe(
      ctx.hMidiInterface, ctx.midiOutPipe, const_cast<PUCHAR>(BCD2000_INIT_SEQUENCE),
      sizeof(BCD2000_INIT_SEQUENCE), &bytesWritten, nullptr);

  if (!ok || bytesWritten != sizeof(BCD2000_INIT_SEQUENCE)) {
    DWORD writeErr = GetLastError();
    std::cerr << "  [-] Error en handshake en Pipe 0x" << std::hex << (int)ctx.midiOutPipe
              << ": " << FormatWin32Error(writeErr) << "\n";

    // If failed on MIDI interface, attempt fallback on Interface 0
    if (ctx.hMidiInterface != ctx.hInterface0) {
      std::cout << "  [?] Reintentando handshake en Interfaz #0...\n";
      ok = WinUsb_WritePipe(ctx.hInterface0, ctx.midiOutPipe, const_cast<PUCHAR>(BCD2000_INIT_SEQUENCE),
                            sizeof(BCD2000_INIT_SEQUENCE), &bytesWritten, nullptr);
      if (ok && bytesWritten == sizeof(BCD2000_INIT_SEQUENCE)) {
        std::cout << "  [+] Handshake exitoso en Interfaz #0!\n";
        ctx.hMidiInterface = ctx.hInterface0;
      } else {
        std::cerr << "  [-] Reintento en Interfaz #0 tambien fallo: " << FormatWin32Error(GetLastError()) << "\n";
      }
    }
  } else {
    std::cout << "  [+] Handshake exitoso (" << bytesWritten << " bytes transmitidos). Microcontrolador activo.\n\n";
  }

  Sleep(150); // Allow MCU to settle
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

  BcdUsbContext ctx{};

  if (!InitializeBcdDevice(ctx)) {
    return 1;
  }

  // --------------------------------------------------------------------------
  // Visual LED Greeting Sweep
  // --------------------------------------------------------------------------
  std::cout << "======================================================================\n";
  std::cout << "       VERIFICACION FISICA DE RETORNO A LEDS (SALUDO VISUAL)          \n";
  std::cout << "======================================================================\n";
  std::cout << "  Modo de transmision inicial: " << GetModeName(g_LedSendMode.load()) << "\n";
  std::cout << "  [Fase 1] Encendiendo LEDs principales...\n";

  SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true, g_LedSendMode.load());
  std::cout << "  [+] Comandos LED ON transmitidos (" << g_TotalLedCommandsSent.load() << " paquetes a EP 0x"
            << std::hex << (int)ctx.midiOutPipe << std::dec << ").\n";
  std::cout << "  >>> INSPECCIONA TU CONSOLA BCD2000 FISICA AHORA: <<<\n";

  for (int sec = 3; sec > 0; --sec) {
    std::cout << "      Manteniendo LEDs encendidos: " << sec << " segundos restantes...\r" << std::flush;
    Sleep(1000);
  }
  std::cout << "      Manteniendo LEDs encendidos: listo!                                \n";

  std::cout << "  [Fase 2] Apagando LEDs para iniciar monitor en vivo...\n";
  SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, false, g_LedSendMode.load());
  std::cout << "  [+] Ciclo de saludo completado.\n\n";

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

  // Launch background worker thread on the correct MIDI interface handle
  std::thread readerThread(MidiReaderThreadWorker, ctx.hMidiInterface, ctx.midiInPipe, ctx.midiOutPipe);

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
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true, g_LedSendMode.load());
        std::cout << "  [+] Comandos LED ON enviados. Permaneceran encendidos hasta que presiones [A].\n\n";
      } else if (ch == 'a' || ch == 'A') {
        std::cout << "\n[TEST] Apagando TODOS los LEDs (" << GetModeName(g_LedSendMode.load()) << ")...\n";
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, false, g_LedSendMode.load());
        std::cout << "  [+] Comandos LED OFF enviados.\n\n";
      } else if (ch == 'm' || ch == 'M') {
        int nextMode = (g_LedSendMode.load() + 1) % 4;
        g_LedSendMode.store(nextMode);
        std::cout << "\n[MODO] Modo de envio de LEDs cambiado a: " << GetModeName(nextMode) << "\n";
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true, nextMode);
      } else if (ch == '1') {
        g_LedSendMode.store(0);
        std::cout << "\n[MODO] Seleccionado Modo 1: " << GetModeName(0) << "\n";
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true, 0);
      } else if (ch == '2') {
        g_LedSendMode.store(1);
        std::cout << "\n[MODO] Seleccionado Modo 2: " << GetModeName(1) << "\n";
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true, 1);
      } else if (ch == '3') {
        g_LedSendMode.store(2);
        std::cout << "\n[MODO] Seleccionado Modo 3: " << GetModeName(2) << "\n";
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true, 2);
      } else if (ch == '4') {
        g_LedSendMode.store(3);
        std::cout << "\n[MODO] Seleccionado Modo 4: " << GetModeName(3) << "\n";
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true, 3);
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
  SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, false, g_LedSendMode.load());

  // Free all interface handles in reverse order
  for (auto it = ctx.allInterfaces.rbegin(); it != ctx.allInterfaces.rend(); ++it) {
    if (*it) WinUsb_Free(*it);
  }
  if (ctx.hDevice != INVALID_HANDLE_VALUE) {
    CloseHandle(ctx.hDevice);
  }

  std::cout << "  [+] Recursos liberados cleanly. Total eventos capturados: "
            << g_TotalEventsDecoded.load() << "\n";
  std::cout << "======================================================================\n";
  std::cout << "             HITO 2: TELEMETRIA Y CONTROL MIDI COMPLETADO             \n";
  std::cout << "======================================================================\n";

  return 0;
}
