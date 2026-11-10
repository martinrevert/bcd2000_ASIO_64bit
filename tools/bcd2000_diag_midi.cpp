/**
 * @file bcd2000_diag_midi.cpp
 * @brief Behringer BCD2000 Physical Control Telemetry and Bi-Directional LED Engine
 *
 * Part of Milestone 2 ("Telemetría de Controles Físicos y Retorno a LEDs")
 * Target Hardware: Behringer BCD2000 DJ Controller (VID: 0x1397, PID: 0x00BD)
 * Driver: WinUSB (x64)
 *
 * Hardware Protocol (verified against Linux kernel sound/usb/bcd2000/bcd2000.c):
 *  1. Handshake (EP 0x01 OUT): 52-byte bcd2000_init_sequence.
 *  2. Telemetry (EP 0x81 IN): 60/64-byte interrupt packets where:
 *       - buffer[0] = payloadLen (number of valid MIDI stream bytes in packet, 1..63).
 *       - buffer[1 .. payloadLen] = raw MIDI stream bytes (uses MIDI Running Status!).
 *       - buffer[payloadLen+1 .. end] = uninitialized 8051 RAM tail (MUST be ignored).
 *  3. LED Commands (EP 0x01 OUT): Full 64-byte (BUFSIZE) packets where:
 *       - frame[0] = 0x03 (device_cmd_prefix[0])
 *       - frame[1] = 0x00 (device_cmd_prefix[1])
 *       - frame[2] = payloadLen (number of MIDI bytes, e.g. 3 or 60)
 *       - frame[3 .. 3+payloadLen-1] = MIDI bytes (0x90/0xB0, id, 0x7F/0x00)
 *       - frame[3+payloadLen .. 63] = zero-padded to 64 bytes.
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
#include <cstring>
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

/**
 * @brief Complete 52-byte hardware initialization sequence from Linux snd-bcd2000.
 */
static const uint8_t BCD2000_INIT_SEQUENCE[52] = {
    0x07, 0x00, 0x00, 0x00, 0x78, 0x48, 0x1c, 0x81,
    0xc4, 0x00, 0x00, 0x00, 0x5e, 0x53, 0x4a, 0xf7,
    0x18, 0xfa, 0x11, 0xff, 0x6c, 0xf3, 0x90, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x51, 0x48, 0x1c, 0x81,
    0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0x1c, 0x81,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// ============================================================================
// USB Device Context & Event Structures
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
  uint8_t status = 0;
  uint8_t channel = 1;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
  std::string rawHex;
  std::string controlType; // "FADER", "KNOB", "BUTTON", "JOG", "PITCH", "MIDI"
  std::string controlName;
  std::string detailValue;
  bool ledToggled = false;
  bool newLedState = false;
};

// Global synchronization and state
static std::atomic<bool> g_Running{true};
static std::mutex g_UsbWriteMutex;
static std::mutex g_EventMutex;
static std::queue<DecodedControlEvent> g_EventQueue;

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
    while (!wmsg.empty() &&
           (wmsg.back() == L'\r' || wmsg.back() == L'\n' || wmsg.back() == L' ')) {
      wmsg.pop_back();
    }
    oss << " (" << WideToUtf8(wmsg) << ")";
  }
  return oss.str();
}

/**
 * @brief Formats a 0..127 unipolar percentage progress bar: [========|       ] 53%
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
 * @brief Formats a bipolar percentage progress bar (-8192..+8191): [    |====   ] +30%
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
 *
 * Exact framing from Linux kernel sound/usb/bcd2000/bcd2000.c (bcd2000_midi_send):
 *  - Byte 0: 0x03 (device_cmd_prefix[0])
 *  - Byte 1: 0x00 (device_cmd_prefix[1])
 *  - Byte 2: payloadLen (number of MIDI bytes in this frame, 1..61)
 *  - Byte 3..(3+payloadLen-1): Raw MIDI stream bytes
 *  - Byte (3+payloadLen)..63: Zero-padded to BUFSIZE (64 bytes)
 */
bool SendBcdMidiFrame(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
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

  std::lock_guard<std::mutex> lock(g_UsbWriteMutex);
  ULONG bytesWritten = 0;
  BOOL ok = WinUsb_WritePipe(hWinUsb, outPipeId, frame, sizeof(frame),
                             &bytesWritten, nullptr);
  if (ok && bytesWritten == sizeof(frame)) {
    g_TotalLedCommandsSent.fetch_add(1);
    return true;
  }

  DWORD err = GetLastError();
  std::cerr << "  [-] Error en WinUsb_WritePipe (EP 0x" << std::hex
            << static_cast<int>(outPipeId) << std::dec << "): "
            << FormatWin32Error(err) << "\n";
  return false;
}

/**
 * @brief Sends a single 3-byte MIDI message wrapped in the 64-byte BCD2000 frame.
 */
bool SendBcdSingleMidi(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                       uint8_t status, uint8_t data1, uint8_t data2) {
  uint8_t msg[3] = {status, data1, data2};
  return SendBcdMidiFrame(hWinUsb, outPipeId, msg, sizeof(msg));
}

/**
 * @brief Illuminates or turns off all physical LEDs on the BCD2000 using the
 *        exact batch + individual 64-byte framing verified in Milestone 1.
 */
void SetAllLedsState(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                     bool turnOn) {
  uint8_t val = turnOn ? 0x7F : 0x00;

  // 1. Batch Frame A: Transport, Cues & Loops (Note On 0x90, notes 0x00..0x13)
  uint8_t noteBatch[60];
  for (uint8_t i = 0; i < 20; ++i) {
    noteBatch[i * 3 + 0] = 0x90;
    noteBatch[i * 3 + 1] = i;
    noteBatch[i * 3 + 2] = val;
  }
  SendBcdMidiFrame(hWinUsb, outPipeId, noteBatch, sizeof(noteBatch));
  Sleep(10);

  // 2. Batch Frame B: Kill switches, EQ & FX (CC 0xB0, 0x0A..0x1D)
  uint8_t ccBatch[60];
  for (uint8_t i = 0; i < 20; ++i) {
    ccBatch[i * 3 + 0] = 0xB0;
    ccBatch[i * 3 + 1] = 0x0A + i;
    ccBatch[i * 3 + 2] = val;
  }
  SendBcdMidiFrame(hWinUsb, outPipeId, ccBatch, sizeof(ccBatch));
  Sleep(10);

  // 3. Individual 64-byte frames across all Note and CC LED addresses
  for (uint8_t midino = 0x00; midino <= 0x25; ++midino) {
    SendBcdSingleMidi(hWinUsb, outPipeId, 0x90, midino, val);
    g_LedStates[(0x90 << 8) | midino] = turnOn;
    Sleep(5);
  }
  for (uint8_t midino = 0x0A; midino <= 0x20; ++midino) {
    SendBcdSingleMidi(hWinUsb, outPipeId, 0xB0, midino, val);
    g_LedStates[(0xB0 << 8) | midino] = turnOn;
    Sleep(5);
  }
}

// ============================================================================
// BCD2000 MIDI Control Mapping Dictionary
// ============================================================================

struct ControlMapping {
  std::string type;
  std::string name;
};

ControlMapping LookupControl(uint8_t status, uint8_t channel, uint8_t data1) {
  uint8_t msgType = status & 0xF0;

  if (msgType == 0x90 || msgType == 0x80) { // Note On / Note Off (Physical Buttons)
    std::string deck =
        (channel == 1 ? "Deck A "
                      : (channel == 2 ? "Deck B "
                                      : "Ch" + std::to_string(channel) + " "));
    switch (data1) {
    case 0x00: return {"BUTTON", deck + "Boton 0x00 (Loop / Control)"};
    case 0x01: return {"BUTTON", deck + "Boton 0x01 (Play / Control)"};
    case 0x02: return {"BUTTON", deck + "Boton 0x02 (Cue / Control)"};
    case 0x03: return {"BUTTON", deck + "Boton 0x03"};
    case 0x04: return {"BUTTON", deck + "Boton 0x04"};
    case 0x05: return {"BUTTON", deck + "Boton 0x05"};
    case 0x06: return {"BUTTON", deck + "Boton 0x06"};
    case 0x07: return {"BUTTON", deck + "Boton 0x07 (Search <<)"};
    case 0x08: return {"BUTTON", deck + "Boton 0x08 (Search >>)"};
    case 0x09: return {"BUTTON", deck + "Boton 0x09 (Pitch Bend -)"};
    case 0x0A: return {"BUTTON", deck + "Boton 0x0A (Pitch Bend +)"};
    case 0x0B: return {"BUTTON", deck + "Boton 0x0B (Sync)"};
    case 0x0C: return {"BUTTON", deck + "Boton 0x0C (Cue)"};
    case 0x0D: return {"BUTTON", deck + "Boton 0x0D (Play / Pause)"};
    case 0x0E: return {"BUTTON", deck + "Boton 0x0E (Headphone Cue)"};
    case 0x0F: return {"BUTTON", deck + "Boton 0x0F (FX / Brake)"};
    case 0x10: return {"BUTTON", deck + "Boton 0x10 (Keylock)"};
    case 0x11: return {"BUTTON", deck + "Boton 0x11 (Scratch / Jog)"};
    case 0x12: return {"BUTTON", deck + "Boton 0x12 (Kill High A)"};
    case 0x13: return {"BUTTON", deck + "Boton 0x13 (Kill Mid A)"};
    case 0x14: return {"BUTTON", deck + "Boton 0x14 (Kill Low A)"};
    case 0x15: return {"BUTTON", deck + "Boton 0x15 (Kill High B)"};
    case 0x16: return {"BUTTON", deck + "Boton 0x16 (Kill Mid B)"};
    case 0x17: return {"BUTTON", deck + "Boton 0x17 (Kill Low B)"};
    default:
      return {"BUTTON",
              deck + "Boton Note 0x" + HexByte(data1) + " (" +
                  std::to_string(data1) + ")"};
    }
  }

  if (msgType == 0xB0) { // Control Change (Faders, Knobs, Jog Wheels)
    switch (data1) {
    case 0x00: return {"FADER", "CC 0x00 (Deck A Volume / Control)"};
    case 0x01: return {"FADER", "CC 0x01 (Crossfader / Control)"};
    case 0x02: return {"KNOB",  "CC 0x02 (Deck A EQ High)"};
    case 0x03: return {"KNOB",  "CC 0x03 (Deck A EQ Mid)"};
    case 0x04: return {"KNOB",  "CC 0x04 (Deck A EQ Low)"};
    case 0x05: return {"KNOB",  "CC 0x05 (Deck A Gain)"};
    case 0x06: return {"KNOB",  "CC 0x06 (Deck B EQ High)"};
    case 0x07: return {"KNOB",  "CC 0x07 (Deck B EQ Mid)"};
    case 0x08: return {"KNOB",  "CC 0x08 (Deck B EQ Low)"};
    case 0x09: return {"KNOB",  "CC 0x09 (Deck B Gain)"};
    case 0x0A: return {"FADER", "CC 0x0A (Deck A Volume Slider)"};
    case 0x0B: return {"FADER", "CC 0x0B (Deck B Volume Slider)"};
    case 0x0C: return {"FADER", "CC 0x0C (Crossfader / Phones)"};
    case 0x0D: return {"FADER", "CC 0x0D (Deck A Pitch / Phones Mix)"};
    case 0x0E: return {"FADER", "CC 0x0E (Deck B Pitch / Master)"};
    case 0x0F: return {"KNOB",  "CC 0x0F (FX Knob 1)"};
    case 0x10: return {"KNOB",  "CC 0x10 (FX Knob 2)"};
    case 0x11: return {"KNOB",  "CC 0x11 (FX Knob 3)"};
    case 0x12: return {"KNOB",  "CC 0x12 (FX Knob 4)"};
    case 0x13: return {"JOG",   "CC 0x13 (Deck A Jog Wheel)"};
    case 0x14: return {"JOG",   "CC 0x14 (Deck B Jog Wheel)"};
    default:
      return {"FADER",
              "Control Change CC 0x" + HexByte(data1) + " (#" +
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

void EmitDecodedMidiMessage(WINUSB_INTERFACE_HANDLE hWinUsbMidi, UCHAR outPipeId,
                            uint8_t status, uint8_t d1, uint8_t d2,
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
      int delta = static_cast<int>(d2) - 64;
      barStr += " (" + std::string(delta >= 0 ? "+" : "") +
                std::to_string(delta) + ")";
    }
    ev.detailValue = barStr;
  } else if (msgType == 0x90 || msgType == 0x80) {
    // Physical Button Note On / Note Off
    bool isPressed = (msgType == 0x90 && d2 > 0);
    ev.detailValue = isPressed ? "PRESIONADO (Down)" : "LIBERADO (Up)";

    if (isPressed) {
      uint16_t ledKey = (0x90 << 8) | d1;
      bool newLed = !g_LedStates[ledKey];
      g_LedStates[ledKey] = newLed;
      ev.ledToggled = true;
      ev.newLedState = newLed;

      uint8_t ledVel = newLed ? 0x7F : 0x00;
      // Send 64-byte LED update on Note 0x90 (and CC 0xB0 if mapped to kill/FX range)
      SendBcdSingleMidi(hWinUsbMidi, outPipeId, 0x90, d1, ledVel);
      if (d1 >= 0x0A && d1 <= 0x20) {
        SendBcdSingleMidi(hWinUsbMidi, outPipeId, 0xB0, d1, ledVel);
      }
    }
  } else if (msgType == 0xE0) {
    int pitchVal = (static_cast<int>(d2) << 7) | static_cast<int>(d1);
    int normalized = pitchVal - 8192;
    ev.detailValue = FormatBipolarBar(normalized, 8192) + " [Raw: " +
                     std::to_string(pitchVal) + "]";
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
  ULONG inTimeoutMs = 100;
  WinUsb_SetPipePolicy(hWinUsbMidi, inPipeId, PIPE_TRANSFER_TIMEOUT,
                       sizeof(inTimeoutMs), &inTimeoutMs);
  UCHAR autoClearStall = 1;
  WinUsb_SetPipePolicy(hWinUsbMidi, inPipeId, AUTO_CLEAR_STALL,
                       sizeof(autoClearStall), &autoClearStall);

  uint8_t buffer[64];

  // Persistent MIDI Running Status State Machine across USB packets
  uint8_t runningStatus = 0;
  int expectedDataBytes = 0;
  int collectedDataBytes = 0;
  uint8_t pendingD1 = 0;

  while (g_Running.load()) {
    ULONG bytesRead = 0;
    BOOL ok = WinUsb_ReadPipe(hWinUsbMidi, inPipeId, buffer, sizeof(buffer),
                              &bytesRead, nullptr);

    if (ok && bytesRead > 0) {
      g_TotalPacketsReceived.fetch_add(1);

      // Linux snd-bcd2000 protocol (bcd2000_input_complete):
      // buffer[0] is the exact number of valid MIDI bytes in this packet (1..63).
      // Bytes after buffer[payloadLen] are uninitialized 8051 RAM garbage and MUST be ignored.
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
        if (i < payloadLen) rawOss << " ";
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
            EmitDecodedMidiMessage(hWinUsbMidi, outPipeId, runningStatus, b, 0,
                                   rawPacketStr);
            collectedDataBytes = 0; // Keep runningStatus active!
          } else {
            if (collectedDataBytes == 0) {
              pendingD1 = b;
              collectedDataBytes = 1;
            } else {
              uint8_t d1 = pendingD1;
              uint8_t d2 = b;
              collectedDataBytes = 0; // Keep runningStatus active for Running Status!
              EmitDecodedMidiMessage(hWinUsbMidi, outPipeId, runningStatus, d1,
                                     d2, rawPacketStr);
            }
          }
        }
      }
    } else {
      DWORD err = GetLastError();
      if (err != ERROR_SEM_TIMEOUT) {
        if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE ||
            err == ERROR_DEV_NOT_EXIST) {
          std::cerr << "\n[!] Comunicacion USB perdida en EP 0x" << std::hex
                    << static_cast<int>(inPipeId) << std::dec << ": "
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
// SetupAPI & WinUSB Multi-Interface Initializer
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
        << "      Verifica que el dispositivo este encendido y configurado con WinUSB en Zadig.\n";
    return false;
  }

  std::cout << "  [+] Interfaz detectada: " << WideToUtf8(targetPath) << "\n";

  ctx.hDevice =
      CreateFileW(targetPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);

  if (ctx.hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "  [-] CreateFileW fallo: " << FormatWin32Error(GetLastError())
              << "\n";
    return false;
  }

  if (!WinUsb_Initialize(ctx.hDevice, &ctx.hInterface0)) {
    std::cerr << "  [-] WinUsb_Initialize fallo: "
              << FormatWin32Error(GetLastError()) << "\n";
    CloseHandle(ctx.hDevice);
    ctx.hDevice = INVALID_HANDLE_VALUE;
    return false;
  }
  ctx.allInterfaces.push_back(ctx.hInterface0);

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

  for (size_t intfIdx = 0; intfIdx < interfaceHandles.size(); ++intfIdx) {
    auto hIntf = interfaceHandles[intfIdx];
    USB_INTERFACE_DESCRIPTOR ifDesc{};
    if (!WinUsb_QueryInterfaceSettings(hIntf, 0, &ifDesc)) {
      continue;
    }

    std::cout << "  [+] Interfaz WinUSB #" << intfIdx
              << " (bInterfaceNumber: " << static_cast<int>(ifDesc.bInterfaceNumber)
              << ", Endpoints: " << static_cast<int>(ifDesc.bNumEndpoints) << "):\n";

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

        std::cout << "      Pipe 0x" << HexByte(pi.PipeId) << " ["
                  << (isInput ? "IN " : "OUT") << " | " << typeStr << " | "
                  << pi.MaximumPacketSize << " bytes | Interval: "
                  << static_cast<int>(pi.Interval) << "]\n";

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

  // Always ensure fill character is space on std::cout
  std::cout << std::dec << std::setfill(' ');

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
            << " -> Pipe 0x" << HexByte(ctx.midiOutPipe) << "\n"
            << "    * Telemetria IN      : Interfaz #" << midiIntfNum
            << " -> Pipe 0x" << HexByte(ctx.midiInPipe) << "\n"
            << "    * Audio Iso OUT      : Pipe 0x" << HexByte(ctx.audioOutPipe) << "\n"
            << "    * Audio Iso IN       : Pipe 0x" << HexByte(ctx.audioInPipe) << "\n";
  std::cout << "======================================================================\n\n";

  ULONG timeoutMs = 2000;
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiOutPipe,
                       PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
  UCHAR autoClearStall = 1;
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiOutPipe, AUTO_CLEAR_STALL,
                       sizeof(autoClearStall), &autoClearStall);

  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiInPipe,
                       PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
  WinUsb_SetPipePolicy(ctx.hMidiInterface, ctx.midiInPipe, AUTO_CLEAR_STALL,
                       sizeof(autoClearStall), &autoClearStall);

  std::cout << "[HANDSHAKE] Transmitiendo trama de inicializacion completa de 52 bytes al microcontrolador 8051...\n";
  ULONG bytesWritten = 0;
  BOOL ok = WinUsb_WritePipe(ctx.hMidiInterface, ctx.midiOutPipe,
                             const_cast<PUCHAR>(BCD2000_INIT_SEQUENCE),
                             sizeof(BCD2000_INIT_SEQUENCE), &bytesWritten,
                             nullptr);

  if (!ok || bytesWritten != sizeof(BCD2000_INIT_SEQUENCE)) {
    DWORD writeErr = GetLastError();
    std::cerr << "  [-] Error en handshake en Pipe 0x"
              << HexByte(ctx.midiOutPipe) << ": " << FormatWin32Error(writeErr)
              << "\n";
    return false;
  }

  std::cout << "  [+] Handshake exitoso (" << bytesWritten
            << " bytes transmitidos). Microcontrolador 8051 activo.\n\n";

  Sleep(150);
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
  // Visual LED Greeting Sweep (Identical 64-byte protocol as Milestone 1)
  // --------------------------------------------------------------------------
  std::cout << "======================================================================\n";
  std::cout << "       VERIFICACION FISICA DE RETORNO A LEDS (SALUDO VISUAL)          \n";
  std::cout << "======================================================================\n";
  std::cout << "  Formato de trama: [0x03, 0x00, len, MIDI_BYTES..., pad 64 bytes]\n";
  std::cout << "  [Fase 1] Encendiendo todos los LEDs fisicos de la BCD2000...\n";

  SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true);
  std::cout << "  [+] Comandos LED ON transmitidos ("
            << g_TotalLedCommandsSent.load() << " tramas de 64B a EP 0x"
            << HexByte(ctx.midiOutPipe) << ").\n";
  std::cout << "  >>> ¡TODOS LOS LEDS DEBEN ESTAR VISIBLEMENTE ILUMINADOS AHORA! <<<\n";

  for (int sec = 3; sec > 0; --sec) {
    std::cout << "      Manteniendo iluminacion activa: " << sec
              << " segundos restantes...\r" << std::flush;
    Sleep(1000);
  }
  std::cout << "      Manteniendo iluminacion activa: completado!                      \n";

  std::cout << "  [Fase 2] Apagando LEDs para iniciar monitor interactivo...\n";
  SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, false);
  std::cout << "  [+] Ciclo de saludo completado.\n\n";

  std::cout << "======================================================================\n";
  std::cout << "                 MODO MONITOR EN VIVO Y CONTROL BIDIRECCIONAL         \n";
  std::cout << "======================================================================\n";
  std::cout << "  INSTRUCCIONES:\n";
  std::cout << "  - Mueve Faders, Knobs o Pitch: veras la BARRA DE % EN TIEMPO REAL.\n";
  std::cout << "  - Gira Ruedas Jog: veras la barra y el desplazamiento relativo.\n";
  std::cout << "  - Presiona cualquier Boton: su LED fisico CONMUTARA EN LA CONSOLA.\n";
  std::cout << "  --------------------------------------------------------------------\n";
  std::cout << "  COMANDOS DE TECLADO INTERACTIVOS:\n";
  std::cout << "      [L] : Encender TODOS los LEDs permanentemente.\n";
  std::cout << "      [A] : Apagar TODOS los LEDs.\n";
  std::cout << "      [T] : Mostrar tabla resumen con el estado actual de cada control.\n";
  std::cout << "      [C] : Limpiar pantalla de consola.\n";
  std::cout << "      [Q] o ESC : Salir ordenadamente del programa.\n";
  std::cout << "----------------------------------------------------------------------\n\n";

  std::thread readerThread(MidiReaderThreadWorker, ctx.hMidiInterface,
                           ctx.midiInPipe, ctx.midiOutPipe);

  while (g_Running.load()) {
    // 1. Process queued decoded control events
    {
      std::lock_guard<std::mutex> lock(g_EventMutex);
      while (!g_EventQueue.empty()) {
        auto ev = g_EventQueue.front();
        g_EventQueue.pop();

        std::cout << std::setfill(' ') << std::left
                  << std::setw(10) << ("[" + ev.controlType + "]")
                  << std::setw(35) << ev.controlName << " "
                  << std::setw(38) << ev.detailValue;

        if (ev.ledToggled) {
          std::cout << " ==> [LED "
                    << (ev.newLedState ? "ENCENDIDO" : "APAGADO") << "]";
        }

        std::cout << "  (" << ev.rawHex << ")\n";
      }
    }

    // 2. Process non-blocking keyboard commands
    if (_kbhit()) {
      int ch = _getch();
      if (ch == 'q' || ch == 'Q' || ch == 27) {
        std::cout << "\n[INFO] Solicitud de salida recibida. Finalizando...\n";
        g_Running.store(false);
        break;
      } else if (ch == 'l' || ch == 'L') {
        std::cout << "\n[TEST] Encendiendo TODOS los LEDs (tramas de 64 bytes)...\n";
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, true);
        std::cout << "  [+] Todos los LEDs encendidos. Presiona [A] para apagarlos.\n\n";
      } else if (ch == 'a' || ch == 'A') {
        std::cout << "\n[TEST] Apagando TODOS los LEDs (tramas de 64 bytes)...\n";
        SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, false);
        std::cout << "  [+] Todos los LEDs apagados.\n\n";
      } else if (ch == 'c' || ch == 'C') {
        system("cls");
        std::cout << "[INFO] Pantalla limpiada. Modo monitor en vivo activo...\n\n";
      } else if (ch == 't' || ch == 'T') {
        std::cout << "\n======================================================================\n";
        std::cout << "                    TABLA RESUMEN DE ESTADO DE CONTROLES              \n";
        std::cout << "======================================================================\n";
        std::cout << "  Eventos MIDI decodificados : " << g_TotalEventsDecoded.load() << "\n";
        std::cout << "  Tramas USB recibidas       : " << g_TotalPacketsReceived.load() << "\n";
        std::cout << "  Tramas LED 64B enviadas    : " << g_TotalLedCommandsSent.load() << "\n";
        std::cout << "----------------------------------------------------------------------\n";
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
        std::cout << "======================================================================\n\n";
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::cout << "\n[CLEANUP] Deteniendo hilo de lectura y apagando LEDs...\n";
  if (readerThread.joinable()) {
    readerThread.join();
  }

  SetAllLedsState(ctx.hMidiInterface, ctx.midiOutPipe, false);

  for (auto it = ctx.allInterfaces.rbegin(); it != ctx.allInterfaces.rend();
       ++it) {
    if (*it) WinUsb_Free(*it);
  }
  if (ctx.hDevice != INVALID_HANDLE_VALUE) {
    CloseHandle(ctx.hDevice);
  }

  std::cout << "  [+] Recursos liberados. Total eventos MIDI decodificados: "
            << g_TotalEventsDecoded.load() << "\n";
  std::cout << "======================================================================\n";
  std::cout << "             HITO 2: TELEMETRIA Y CONTROL MIDI COMPLETADO             \n";
  std::cout << "======================================================================\n";

  return 0;
}
