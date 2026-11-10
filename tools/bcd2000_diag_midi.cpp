/**
 * @file bcd2000_diag_midi.cpp
 * @brief Behringer BCD2000 Physical Control Telemetry and Bi-Directional LED
 * Engine
 *
 * Part of Milestone 2 ("Telemetría de Controles Físicos y Retorno a LEDs")
 * Target Hardware: Behringer BCD2000 DJ Controller (VID: 0x1397, PID: 0x00BD)
 *
 * Capabilities:
 *  1. Initializes WinUSB connection and sends 52-byte handshake sequence to
 * wake MCU.
 *  2. Background Reader Thread: Continuously captures real-time control events
 * on Endpoint 0x81 (IN).
 *  3. Human-Readable Decoders:
 *     - Faders and Potentiometers (Volume, Crossfader, EQs, Master, Phones,
 * Pitch).
 *     - Jog Wheels (Scratch / Pitch Bend / Rotary direction & ticks).
 *     - Push Buttons (Play/Pause, Cue, Loops, Kill switches, FX).
 *  4. Bi-Directional LED Engine (Endpoint 0x01 OUT):
 *     - Maintains internal toggle states for all button LEDs.
 *     - Instant hardware feedback: pressing a physical button toggles its LED
 * on the hardware.
 *  5. Interactive Console Commands:
 *     - [L]: Sweep illumination test across all LEDs.
 *     - [A]: Turn off all LEDs.
 *     - [T]: Print current state table of all faders, knobs, and buttons.
 *     - [C]: Clear screen.
 *     - [Q] / [ESC]: Clean exit, clearing LEDs and freeing handles.
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
  std::string controlType; // "FADER", "KNOB", "BUTTON", "JOG", "PITCH"
  std::string controlName;
  std::string detailValue;
  bool ledToggled = false;
  bool newLedState = false;
};

// Global flags and synchronization
static std::atomic<bool> g_Running{true};
static std::mutex g_UsbWriteMutex;
static std::mutex g_EventMutex;
static std::queue<DecodedControlEvent> g_EventQueue;

// LED and Control States
static std::map<uint16_t, bool> g_LedStates; // Key: (status << 8) | data1
static std::map<uint16_t, uint8_t>
    g_ControlValues; // Key: (status << 8) | data1 -> last value
static std::atomic<uint64_t> g_TotalPacketsReceived{0};
static std::atomic<uint64_t> g_TotalEventsDecoded{0};

// ============================================================================
// Helper Utilities
// ============================================================================

std::string WideToUtf8(const std::wstring &wstr) {
  if (wstr.empty())
    return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                                 static_cast<int>(wstr.size()), nullptr, 0,
                                 nullptr, nullptr);
  if (size <= 0)
    return {};
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
  bar += "] " + std::to_string((value * 100) / 127) + "%";
  return bar;
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

// ============================================================================
// USB Transmission Engine (Endpoint 0x01 OUT)
// ============================================================================

/**
 * @brief Sends a formatted BCD2000 command packet to Endpoint 0x01.
 * 
 * Protocol specification (Linux kernel snd-bcd2000):
 *  Frame layout: [0x03, 0x00, MIDI_BYTES..., pad to 64 bytes]
 *  - Byte 0: 0x03 (device_cmd_prefix[0])
 *  - Byte 1: 0x00 (device_cmd_prefix[1])
 *  - Byte 2: MIDI Byte 1 (Status, e.g. 0x90 Note On or 0xB0 CC)
 *  - Byte 3: MIDI Byte 2 (Data1, e.g. Note number or CC ID)
 *  - Byte 4: MIDI Byte 3 (Data2, e.g. Velocity 0x7F/0x00 or CC Value)
 *  - Padded to exactly 64 bytes (MaximumPacketSize) with zeros.
 *  Note: There is NO length byte between prefix and MIDI bytes.
 */
bool SendBcdMidiFrame(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                      const uint8_t *midiPayload, uint8_t payloadLen) {
  if (!hWinUsb || outPipeId == 0 || !midiPayload || payloadLen == 0 ||
      payloadLen > 62) {
    return false;
  }

  uint8_t frame[64] = {0};
  frame[0] = 0x03;
  frame[1] = 0x00;
  memcpy(&frame[2], midiPayload, payloadLen);

  std::lock_guard<std::mutex> lock(g_UsbWriteMutex);
  ULONG bytesWritten = 0;
  BOOL ok = WinUsb_WritePipe(hWinUsb, outPipeId, frame, sizeof(frame),
                             &bytesWritten, nullptr);
  return (ok && bytesWritten == sizeof(frame));
}

bool SendBcdSingleMidi(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                       uint8_t status, uint8_t data1, uint8_t data2) {
  uint8_t msg[3] = {status, data1, data2};
  return SendBcdMidiFrame(hWinUsb, outPipeId, msg, sizeof(msg));
}

void SetAllLedsState(WINUSB_INTERFACE_HANDLE hWinUsb, UCHAR outPipeId,
                     bool turnOn) {
  uint8_t val = turnOn ? 0x7F : 0x00;

  // 1. Buttons (Note On 0x90, notes 0 to 19 / 0x13)
  for (uint8_t i = 0; i <= 0x13; ++i) {
    SendBcdSingleMidi(hWinUsb, outPipeId, 0x90, i, val);
    g_LedStates[(0x90 << 8) | i] = turnOn;
    Sleep(2);
  }

  // 2. Kill switches & FX Section (Control Change 0xB0, CC 0x0A to 0x1F)
  for (uint8_t i = 0x0A; i <= 0x1F; ++i) {
    SendBcdSingleMidi(hWinUsb, outPipeId, 0xB0, i, val);
    g_LedStates[(0xB0 << 8) | i] = turnOn;
    Sleep(2);
  }
}

// ============================================================================
// BCD2000 MIDI Control Mapping Dictionary
// ============================================================================

struct ControlMapping {
  std::string type;
  std::string name;
  bool hasLed;
};

ControlMapping LookupControl(uint8_t status, uint8_t channel, uint8_t data1) {
  uint8_t msgType = status & 0xF0;

  if (msgType == 0x90 || msgType == 0x80) { // Note On / Note Off (Buttons)
    std::string deck =
        (channel == 1
             ? "Deck A"
             : (channel == 2 ? "Deck B" : "Ch" + std::to_string(channel)));
    switch (data1) {
    case 0x00:
      return {"BUTTON", deck + " Search << (Back)", true};
    case 0x01:
      return {"BUTTON", deck + " Search >> (Fwd)", true};
    case 0x02:
      return {"BUTTON", deck + " Pitch Bend (-)", true};
    case 0x03:
      return {"BUTTON", deck + " Pitch Bend (+)", true};
    case 0x04:
      return {"BUTTON", deck + " Sync", true};
    case 0x05:
      return {"BUTTON", deck + " Cue 1", true};
    case 0x06:
      return {"BUTTON", deck + " Cue 2", true};
    case 0x07:
      return {"BUTTON", deck + " Cue 3", true};
    case 0x08:
      return {"BUTTON", deck + " Cue 4", true};
    case 0x09:
      return {"BUTTON", deck + " Loop In", true};
    case 0x0A:
      return {"BUTTON", deck + " Loop Out", true};
    case 0x0B:
      return {"BUTTON", deck + " Loop Exit / Reloop", true};
    case 0x0C:
      return {"BUTTON", deck + " Cue", true};
    case 0x0D:
      return {"BUTTON", deck + " Play / Pause", true};
    case 0x0E:
      return {"BUTTON", deck + " Headphone Cue (PFL)", true};
    case 0x0F:
      return {"BUTTON", deck + " Brake / FX", true};
    case 0x10:
      return {"BUTTON", deck + " Loop 1/2x", true};
    case 0x11:
      return {"BUTTON", deck + " Loop 2x", true};
    case 0x12:
      return {"BUTTON", deck + " Master Tempo (Keylock)", true};
    case 0x13:
      return {"BUTTON", deck + " Jog Wheel Touch Active", false};
    default:
      return {"BUTTON", deck + " Button 0x" + std::to_string(data1), true};
    }
  }

  if (msgType ==
      0xB0) { // Control Change (Faders, Knobs, Encoders, Kill Switches)
    switch (data1) {
    // Faders & Global Knobs
    case 0x00:
      return {"FADER", "Deck A Volume Fader", false};
    case 0x01:
      return {"FADER", "Deck B Volume Fader", false};
    case 0x02:
      return {"KNOB", "Deck A EQ High", false};
    case 0x03:
      return {"KNOB", "Deck A EQ Mid", false};
    case 0x04:
      return {"KNOB", "Deck A EQ Low", false};
    case 0x05:
      return {"KNOB", "Deck B EQ High", false};
    case 0x06:
      return {"KNOB", "Deck B EQ Mid", false};
    case 0x07:
      return {"KNOB", "Deck B EQ Low", false};
    case 0x08:
      return {"FADER", "Crossfader", false};
    case 0x09:
      return {"KNOB", "Master Level", false};
    case 0x0A:
      return {"FADER", "Deck A Pitch Slider", false};
    case 0x0B:
      return {"FADER", "Deck B Pitch Slider", false};
    case 0x0C:
      return {"KNOB", "Phones Level", false};
    case 0x0D:
      return {"KNOB", "Phones Mix (Cue/Master)", false};
    case 0x0E:
      return {"KNOB", "Mic / External Input Level", false};
    case 0x0F:
      return {"KNOB", "Deck A Gain", false};

    // Kill Switches (Toggles with LEDs)
    case 0x10:
      return {"BUTTON", "Deck A EQ Low Kill Switch", true};
    case 0x11:
      return {"BUTTON", "Deck A EQ Mid Kill Switch", true};
    case 0x12:
      return {"BUTTON", "Deck A EQ High Kill Switch", true};
    case 0x13:
      return {"JOG", "Deck A Jog Wheel Rotary Tick", false};
    case 0x14:
      return {"JOG", "Deck B Jog Wheel Rotary Tick", false};
    case 0x15:
      return {"BUTTON", "Deck B EQ Low Kill Switch", true};
    case 0x16:
      return {"BUTTON", "Deck B EQ Mid Kill Switch", true};
    case 0x17:
      return {"BUTTON", "Deck B EQ High Kill Switch", true};

    // FX Section
    case 0x18:
      return {"KNOB", "FX Section Parameter 1", false};
    case 0x19:
      return {"KNOB", "FX Section Parameter 2", false};
    case 0x1A:
      return {"KNOB", "FX Section Parameter 3", false};
    case 0x1B:
      return {"KNOB", "FX Section Parameter 4", false};
    case 0x1C:
      return {"BUTTON", "FX Section Button 1", true};
    case 0x1D:
      return {"BUTTON", "FX Section Button 2", true};
    case 0x1E:
      return {"BUTTON", "FX Section Button 3", true};
    case 0x1F:
      return {"BUTTON", "FX Section Button 4", true};

    default:
      return {"KNOB", "Control Change CC 0x" + std::to_string(data1), false};
    }
  }

  if (msgType == 0xE0) {
    std::string deck = (channel == 1 ? "Deck A" : "Deck B");
    return {"PITCH", deck + " High-Res Pitch Bend", false};
  }

  return {"MIDI", "Generic MIDI Event 0x" + std::to_string(status), false};
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

      // Determine framing:
      // Linux snd-bcd2000 protocol:
      // Byte 0 = payload length (e.g. 3, 6, 9)
      // Byte 1.. = raw MIDI stream [status, data1, data2, ...]
      ULONG midiStart = 0;
      ULONG midiLen = bytesRead;

      if (buffer[0] <= 63 && buffer[0] > 0 && bytesRead > 1 && (buffer[1] & 0x80) != 0) {
        // Standard BCD2000 hardware packet format: buffer[0] = length
        midiStart = 1;
        midiLen = std::min<ULONG>(static_cast<ULONG>(buffer[0]), bytesRead - 1);
      } else if (bytesRead >= 3 && buffer[0] == 0x03 && buffer[1] == 0x00 && (buffer[2] & 0x80) != 0) {
        // Echoed 2-byte command header [0x03, 0x00]
        midiStart = 2;
        midiLen = bytesRead - 2;
      } else if ((buffer[0] & 0x80) != 0) {
        // Direct raw MIDI stream starting at index 0
        midiStart = 0;
        midiLen = bytesRead;
      }

      bool anyEventDecoded = false;

      // Process 3-byte MIDI events within the extracted payload
      for (ULONG i = midiStart; i + 2 < midiStart + midiLen; i += 3) {
        uint8_t status = buffer[i];
        uint8_t d1 = buffer[i + 1];
        uint8_t d2 = buffer[i + 2];

        // Valid MIDI status bytes always have bit 7 set
        if ((status & 0x80) == 0) {
          continue;
        }

        anyEventDecoded = true;
        uint8_t msgType = status & 0xF0;
        uint8_t channel = (status & 0x0F) + 1;

        DecodedControlEvent ev;
        ev.timestamp = std::chrono::system_clock::now();
        ev.status = status;
        ev.channel = channel;
        ev.data1 = d1;
        ev.data2 = d2;

        ControlMapping mapping = LookupControl(status, channel, d1);
        ev.controlType = mapping.type;
        ev.controlName = mapping.name;

        uint16_t stateKey = (static_cast<uint16_t>(status) << 8) | d1;
        g_ControlValues[stateKey] = d2;

        // Format human-readable detail value
        if (mapping.type == "FADER" || mapping.type == "KNOB") {
          ev.detailValue =
              FormatProgressBar(d2) + " [Raw: " + std::to_string(d2) + "]";
        } else if (mapping.type == "JOG") {
          // Jog wheel delta
          int delta = (d2 & 0x40) ? -(d2 & 0x3F) : (d2 & 0x3F);
          ev.detailValue = (delta >= 0 ? "+" : "") + std::to_string(delta) +
                           " ticks (" + (delta > 0 ? "CW" : "CCW") + ")";
        } else if (mapping.type == "PITCH") {
          int pitchVal = (static_cast<int>(d2) << 7) | static_cast<int>(d1);
          int normalized = pitchVal - 8192;
          ev.detailValue = "Value: " + std::to_string(pitchVal) +
                           " (Offset: " + (normalized >= 0 ? "+" : "") +
                           std::to_string(normalized) + ")";
        } else if (mapping.type == "BUTTON") {
          bool isPressed =
              (msgType == 0x90 && d2 > 0) || (msgType == 0xB0 && d2 >= 0x40);
          ev.detailValue = isPressed ? "PRESIONADO (Down)" : "LIBERADO (Up)";

          // Instant physical feedback: toggle LED on press action
          if (mapping.hasLed && isPressed) {
            bool currentLed = g_LedStates[stateKey];
            bool newLed = !currentLed;
            g_LedStates[stateKey] = newLed;

            ev.ledToggled = true;
            ev.newLedState = newLed;

            // Transmit updated LED state to Endpoint 0x01
            SendBcdSingleMidi(hWinUsb, outPipeId,
                              (msgType == 0xB0 ? 0xB0 : 0x90), d1,
                              newLed ? 0x7F : 0x00);
          }
        } else {
          ev.detailValue =
              "Data1: " + std::to_string(d1) + ", Data2: " + std::to_string(d2);
        }

        g_TotalEventsDecoded.fetch_add(1);

        std::lock_guard<std::mutex> lock(g_EventMutex);
        g_EventQueue.push(ev);
      }

      // If bytes were received but couldn't be parsed into 3-byte MIDI, display raw hex
      if (!anyEventDecoded) {
        std::ostringstream oss;
        oss << "Bytes (" << bytesRead << "): ";
        for (ULONG b = 0; b < bytesRead && b < 16; ++b) {
          oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
              << (int)buffer[b] << " ";
        }
        DecodedControlEvent rawEv;
        rawEv.timestamp = std::chrono::system_clock::now();
        rawEv.controlType = "RAW_USB";
        rawEv.controlName = "Trama USB Sin Mapear";
        rawEv.detailValue = oss.str();

        std::lock_guard<std::mutex> lock(g_EventMutex);
        g_EventQueue.push(rawEv);
      }
    } else {
      DWORD err = GetLastError();
      if (err != ERROR_SEM_TIMEOUT) {
        // If device unplugged or severe pipe error
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

  // Select MIDI / Control Pipes (Target: EP 0x01 OUT, EP 0x81 IN)
  outPipeId = 0x01;
  inPipeId = 0x81;

  USB_INTERFACE_DESCRIPTOR ifDesc{};
  if (WinUsb_QueryInterfaceSettings(outWinUsbHandle, 0, &ifDesc)) {
    for (UCHAR i = 0; i < ifDesc.bNumEndpoints; ++i) {
      WINUSB_PIPE_INFORMATION pi{};
      if (WinUsb_QueryPipe(outWinUsbHandle, 0, i, &pi)) {
        // Prioritize Endpoint 0x01 (Control/MIDI OUT) and Endpoint 0x81 (Control/MIDI IN)
        if (pi.PipeId == 0x01) {
          outPipeId = 0x01;
        } else if (pi.PipeId == 0x81) {
          inPipeId = 0x81;
        } else if (pi.PipeType == UsbdPipeTypeInterrupt) {
          if (USB_ENDPOINT_DIRECTION_IN(pi.PipeId) && inPipeId == 0x81) {
            inPipeId = pi.PipeId;
          } else if (!USB_ENDPOINT_DIRECTION_IN(pi.PipeId) && outPipeId == 0x01) {
            outPipeId = pi.PipeId;
          }
        }
      }
    }
  }

  // Configure Pipe Policies on Endpoint 0x01 (OUT)
  ULONG timeoutMs = 2000;
  WinUsb_SetPipePolicy(outWinUsbHandle, outPipeId, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
  UCHAR autoClearStall = 1;
  WinUsb_SetPipePolicy(outWinUsbHandle, outPipeId, AUTO_CLEAR_STALL, sizeof(autoClearStall), &autoClearStall);

  std::cout << "  [+] WinUSB inicializado con exito.\n"
            << "      Endpoint OUT: 0x" << std::hex << (int)outPipeId
            << " (Control/MIDI) | Endpoint IN: 0x" << (int)inPipeId << " (Telemetria)" << std::dec << "\n\n";

  // Transmit 52-byte handshake sequence
  std::cout << "[HANDSHAKE] Transmitiendo trama de inicializacion de 52 bytes "
               "al microcontrolador 8051...\n";
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

  std::cout << "==============================================================="
               "=======\n";
  std::cout << "   Behringer BCD2000 Telemetria de Controles y Retorno a LEDs "
               "(Hito 2)\n";
  std::cout << "   Target: VID 0x1397, PID 0x00BD | Driver: WinUSB (x64)       "
               "      \n";
  std::cout << "==============================================================="
               "=======\n\n";

  HANDLE hDevice = INVALID_HANDLE_VALUE;
  WINUSB_INTERFACE_HANDLE hWinUsb = nullptr;
  UCHAR outPipeId = 0x01;
  UCHAR inPipeId = 0x81;

  if (!InitializeBcdDevice(hDevice, hWinUsb, outPipeId, inPipeId)) {
    return 1;
  }

  // Quick initial LED greeting sweep (turns on transport LEDs for 500ms)
  std::cout
      << "[LED INIT] Verificando retorno fisico de LEDs (Saludo inicial)...\n";
  SetAllLedsState(hWinUsb, outPipeId, true);
  Sleep(500);
  SetAllLedsState(hWinUsb, outPipeId, false);
  std::cout << "  [+] LEDs verificados y listos para interactuar.\n\n";

  std::cout << "==============================================================="
               "=======\n";
  std::cout << "                 MODO MONITOR EN VIVO Y CONTROL BIDIRECCIONAL  "
               "      \n";
  std::cout << "==============================================================="
               "=======\n";
  std::cout << "  INSTRUCCIONES:\n";
  std::cout << "  - Mueve cualquier Fader, Knob o Jog Wheel para ver el valor "
               "y barra.\n";
  std::cout << "  - Presiona cualquier Boton: se decodificara y SU LED "
               "CONMUTARA ON/OFF.\n";
  std::cout << "  - Comandos de teclado interactivos:\n";
  std::cout << "      [L] : Disparar barrido general de encendido de todos los "
               "LEDs.\n";
  std::cout << "      [A] : Apagar todos los LEDs.\n";
  std::cout << "      [T] : Mostrar tabla de estado de controles activos.\n";
  std::cout << "      [C] : Limpiar pantalla.\n";
  std::cout << "      [Q] o ESC : Salir ordenadamente del programa.\n";
  std::cout << "---------------------------------------------------------------"
               "-------\n\n";

  // Launch background worker thread for Endpoint 0x81
  std::thread readerThread(MidiReaderThreadWorker, hWinUsb, inPipeId,
                           outPipeId);

  // Main UI & Event Dispatch Loop
  while (g_Running.load()) {
    // 1. Process queued decoded control events
    {
      std::lock_guard<std::mutex> lock(g_EventMutex);
      while (!g_EventQueue.empty()) {
        auto ev = g_EventQueue.front();
        g_EventQueue.pop();

        std::cout << std::left << std::setw(8) << ("[" + ev.controlType + "]")
                  << std::setw(32) << ev.controlName << " " << ev.detailValue;

        if (ev.ledToggled) {
          std::cout << "  ==> [LED "
                    << (ev.newLedState ? "ENCENDIDO" : "APAGADO") << "]";
        }
        std::cout << "\n";
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
        std::cout << "\n[TEST] Encendiendo todos los LEDs de la consola...\n";
        SetAllLedsState(hWinUsb, outPipeId, true);
      } else if (ch == 'a' || ch == 'A') {
        std::cout << "\n[TEST] Apagando todos los LEDs de la consola...\n";
        SetAllLedsState(hWinUsb, outPipeId, false);
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
        std::cout << "  Eventos decodificados : " << g_TotalEventsDecoded.load()
                  << "\n";
        std::cout << "  Tramas USB recibidas   : "
                  << g_TotalPacketsReceived.load() << "\n";
        std::cout << "---------------------------------------------------------"
                     "-------------\n";
        for (const auto &kv : g_ControlValues) {
          uint8_t st = static_cast<uint8_t>(kv.first >> 8);
          uint8_t d1 = static_cast<uint8_t>(kv.first & 0xFF);
          uint8_t val = kv.second;
          auto map = LookupControl(st, (st & 0x0F) + 1, d1);
          std::cout << "  " << std::left << std::setw(32) << map.name << " = "
                    << std::setw(4) << static_cast<int>(val) << " "
                    << FormatProgressBar(val, 12) << "\n";
        }
        std::cout << "========================================================="
                     "=============\n\n";
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
  SetAllLedsState(hWinUsb, outPipeId, false);

  WinUsb_Free(hWinUsb);
  CloseHandle(hDevice);

  std::cout << "  [+] Recursos liberados cleanly. Total eventos capturados: "
            << g_TotalEventsDecoded.load() << "\n";
  std::cout << "==============================================================="
               "=======\n";
  std::cout << "             HITO 2: TELEMETRIA Y CONTROL MIDI COMPLETADO      "
               "       \n";
  std::cout << "==============================================================="
               "=======\n";

  return 0;
}
