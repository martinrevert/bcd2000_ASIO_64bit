/**
 * @file bcd2000_usb_protocol.h
 * @brief Shared USB protocol constants, MIDI Running Status parser, and IPC pipe
 *        definitions for Behringer BCD2000 (VID 0x1397, PID 0x00BD).
 */

#ifndef BCD2000_USB_PROTOCOL_H
#define BCD2000_USB_PROTOCOL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <usb.h>
#include <winusb.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bcd2000 {

// Generic WinUSB Device Interface GUID (Zadig / WinUSB default)
// {DEE824EF-729B-4A0E-9C14-B7117D33A817}
inline constexpr GUID kWinUsbGenericGuid = {
    0xdee824ef, 0x729b, 0x4a0e,
    {0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17}};

// Bidirectional Named Pipe hosted by bcd2000_asio.dll when owning WinUSB.
// ASIO -> MIDI sends 4-byte DWORD packed MIDI short messages (status | d1<<8 | d2<<16).
// MIDI -> ASIO sends [uint8_t len (1..61), uint8_t rawMidiBytes[len]].
inline constexpr wchar_t kAsioMidiPipeName[] = L"\\\\.\\pipe\\BCD2000_ASIO_MIDI_V1";

// Control Named Pipe hosted by standalone bcd2000_midi.dll so bcd2000_asio.dll
// can request immediate release of the exclusive WinUSB handle when ASIO starts.
inline constexpr wchar_t kMidiCtrlPipeName[] = L"\\\\.\\pipe\\BCD2000_MIDI_CTRL_V1";

// Complete 52-byte initialization handshake from Linux kernel sound/usb/bcd2000/bcd2000.c
inline constexpr uint8_t kInitSequence[52] = {
    0x07, 0x00, 0x00, 0x00, 0x78, 0x48, 0x1c, 0x81, 0xc4, 0x00, 0x00,
    0x00, 0x5e, 0x53, 0x4a, 0xa7, 0x18, 0xfa, 0x11, 0xff, 0x6c, 0xf3,
    0x90, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x18,
    0xfa, 0x11, 0xff, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf2, 0x34, 0x4a, 0xf7, 0x18, 0xfa, 0x11, 0xff};

inline bool ContainsInsensitiveW(const std::wstring &text, const std::wstring &target) {
    auto it = std::search(
        text.begin(), text.end(), target.begin(), target.end(),
        [](wchar_t ch1, wchar_t ch2) { return towlower(ch1) == towlower(ch2); });
    return it != text.end();
}

/**
 * @brief Locates the WinUSB device path for Behringer BCD2000 (VID 1397 / PID 00BD).
 */
inline std::wstring FindBcd2000DevicePath() {
    HDEVINFO hIntfInfo = SetupDiGetClassDevsW(
        &kWinUsbGenericGuid, nullptr, nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (hIntfInfo == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::wstring targetPath;
    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(
             hIntfInfo, nullptr, &kWinUsbGenericGuid, idx, &ifData);
         ++idx) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hIntfInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0) {
            continue;
        }

        std::vector<BYTE> buf(reqSize);
        auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(hIntfInfo, &ifData, detail, reqSize,
                                             nullptr, nullptr)) {
            std::wstring path(detail->DevicePath);
            if (ContainsInsensitiveW(path, L"vid_1397") &&
                ContainsInsensitiveW(path, L"pid_00bd")) {
                targetPath = std::move(path);
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(hIntfInfo);
    return targetPath;
}

/**
 * @brief Stateful MIDI Running Status parser for EP 0x81 IN 64-byte packets.
 */
class MidiRunningStatusParser {
public:
    void Reset() {
        m_runningStatus = 0;
        m_expectedDataBytes = 0;
        m_collectedDataBytes = 0;
        m_pendingD1 = 0;
    }

    template <typename Callback>
    void FeedUsbPacket(const uint8_t *buffer, ULONG bytesRead, Callback &&onShortMsg) {
        if (!buffer || bytesRead < 2) {
            return;
        }
        const uint8_t payloadLen = buffer[0];
        if (payloadLen == 0 || payloadLen > 63 ||
            static_cast<ULONG>(payloadLen) >= bytesRead) {
            return;
        }

        for (uint8_t i = 1; i <= payloadLen; ++i) {
            const uint8_t b = buffer[i];
            if ((b & 0x80) != 0) {
                if (b >= 0xF8) {
                    continue; // Real-time messages do not alter Running Status
                }
                if (b >= 0xF0) {
                    m_runningStatus = 0;
                    m_expectedDataBytes = 0;
                    m_collectedDataBytes = 0;
                    continue;
                }
                m_runningStatus = b;
                m_collectedDataBytes = 0;
                const uint8_t msgType = b & 0xF0;
                m_expectedDataBytes = (msgType == 0xC0 || msgType == 0xD0) ? 1 : 2;
            } else {
                if (m_runningStatus == 0 || m_expectedDataBytes == 0) {
                    continue;
                }
                if (m_expectedDataBytes == 1) {
                    const DWORD packed = static_cast<DWORD>(m_runningStatus) |
                                         (static_cast<DWORD>(b) << 8);
                    onShortMsg(packed);
                    m_collectedDataBytes = 0;
                } else {
                    if (m_collectedDataBytes == 0) {
                        m_pendingD1 = b;
                        m_collectedDataBytes = 1;
                    } else {
                        const DWORD packed = static_cast<DWORD>(m_runningStatus) |
                                             (static_cast<DWORD>(m_pendingD1) << 8) |
                                             (static_cast<DWORD>(b) << 16);
                        m_collectedDataBytes = 0;
                        onShortMsg(packed);
                    }
                }
            }
        }
    }

private:
    uint8_t m_runningStatus{0};
    int m_expectedDataBytes{0};
    int m_collectedDataBytes{0};
    uint8_t m_pendingD1{0};
};

/**
 * @brief Wraps raw MIDI payload bytes (1..61) in the BCD2000 64-byte EP 0x01 frame
 *        and sends asynchronously using overlapped I/O so concurrent EP 0x81 reads
 *        never block outgoing LED/MIDI updates.
 */
inline bool SendBcdMidiOverlapped(WINUSB_INTERFACE_HANDLE hMidiIf, UCHAR outPipe,
                                  const uint8_t *midiPayload, size_t payloadLen) {
    if (!hMidiIf || outPipe == 0 || !midiPayload || payloadLen == 0) {
        return false;
    }

    size_t offset = 0;
    while (offset < payloadLen) {
        const size_t chunk = std::min<size_t>(payloadLen - offset, 61);
        uint8_t frame[64] = {0};
        frame[0] = 0x03;
        frame[1] = 0x00;
        frame[2] = static_cast<uint8_t>(chunk);
        std::memcpy(&frame[3], midiPayload + offset, chunk);

        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            return false;
        }

        ULONG sent = 0;
        BOOL ok = WinUsb_WritePipe(hMidiIf, outPipe, frame, sizeof(frame), &sent, &ov);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 300) == WAIT_OBJECT_0) {
                ok = WinUsb_GetOverlappedResult(hMidiIf, &ov, &sent, FALSE);
            } else {
                WinUsb_AbortPipe(hMidiIf, outPipe);
                ok = FALSE;
            }
        }
        CloseHandle(ov.hEvent);

        if (!ok) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

/**
 * @brief Converts a WinMM packed short MIDI DWORD into 1..3 raw MIDI bytes.
 */
inline size_t UnpackShortMidiMessage(DWORD packedMsg, uint8_t outBytes[3]) {
    const uint8_t status = static_cast<uint8_t>(packedMsg & 0xFF);
    const uint8_t d1 = static_cast<uint8_t>((packedMsg >> 8) & 0xFF);
    const uint8_t d2 = static_cast<uint8_t>((packedMsg >> 16) & 0xFF);

    if ((status & 0x80) == 0) {
        return 0;
    }
    outBytes[0] = status;

    if (status >= 0xF8) {
        return 1; // Real-Time message
    }
    if (status == 0xF1 || status == 0xF3 || (status & 0xF0) == 0xC0 ||
        (status & 0xF0) == 0xD0) {
        outBytes[1] = d1;
        return 2;
    }
    if (status == 0xF2 || status < 0xF0) {
        outBytes[1] = d1;
        outBytes[2] = d2;
        return 3;
    }
    return 1;
}

} // namespace bcd2000

#endif // BCD2000_USB_PROTOCOL_H
