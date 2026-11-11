/**
 * @file bcd2000_asio_driver.cpp
 * @brief Implementation of the 64-bit ASIO Driver (IASIO) for Behringer BCD2000
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

#include "bcd2000_asio_driver.h"

#include <avrt.h>
#include <initguid.h>
#include <setupapi.h>
#include <usb.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

// Generic WinUSB Device Interface GUID (Zadig / WinUSB default)
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB_BCD2000, 0xdee824ef, 0x729b, 0x4a0e, 0x9c,
            0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);

// Complete 52-byte initialization handshake from Linux kernel sound/usb/bcd2000/bcd2000.c
static const uint8_t kBcd2000InitSequence[52] = {
    0x07, 0x00, 0x00, 0x00, 0x78, 0x48, 0x1c, 0x81, 0xc4, 0x00, 0x00,
    0x00, 0x5e, 0x53, 0x4a, 0xa7, 0x18, 0xfa, 0x11, 0xff, 0x6c, 0xf3,
    0x90, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x18,
    0xfa, 0x11, 0xff, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf2, 0x34, 0x4a, 0xf7, 0x18, 0xfa, 0x11, 0xff};

// Hardware LED Control Change numbers (0xB0, CC 0x00..0x19)
constexpr uint8_t kLedPlayA = 0x12;
constexpr uint8_t kLedPlayB = 0x0A;
constexpr uint8_t kLedPflA  = 0x02;
constexpr uint8_t kLedPflB  = 0x01;

namespace {

bool ContainsInsensitiveW(const std::wstring &text, const std::wstring &target) {
    auto it = std::search(
        text.begin(), text.end(), target.begin(), target.end(),
        [](wchar_t ch1, wchar_t ch2) { return towlower(ch1) == towlower(ch2); });
    return it != text.end();
}

} // namespace

struct Bcd2000AsioDriver::MidiPipeClient {
    HANDLE hPipe{INVALID_HANDLE_VALUE};
    std::mutex writeMutex;
    std::atomic<bool> alive{true};
    ~MidiPipeClient() {
        if (hPipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(hPipe, nullptr);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            hPipe = INVALID_HANDLE_VALUE;
        }
    }
};

// ============================================================================
// Constructor & Destructor
// ============================================================================

Bcd2000AsioDriver::Bcd2000AsioDriver() {
    g_ActiveComObjects.fetch_add(1);
    QueryPerformanceFrequency(&m_qpcFreq);
    std::snprintf(m_errorMessage, sizeof(m_errorMessage), "No error.");
    m_outRingBuffer.resize(kRingCapacityFrames * kNumOutputs, 0);
}

Bcd2000AsioDriver::~Bcd2000AsioDriver() {
    stop();
    disposeBuffers();
    CloseUsbHardware();
    if (m_sharedState) {
        m_sharedState->driverActive = 0;
        m_sharedState->isRunning = 0;
        bcd2000::CloseSharedState(m_hSharedMem, m_sharedState);
        m_sharedState = nullptr;
        m_hSharedMem = nullptr;
    }
    g_ActiveComObjects.fetch_sub(1);
}

// ============================================================================
// IUnknown Implementation
// ============================================================================

STDMETHODIMP Bcd2000AsioDriver::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    // Steinberg ASIO hosts pass the driver's CLSID as the IID in CoCreateInstance!
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_BCD2000_ASIO)) {
        *ppv = static_cast<IASIO *>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) Bcd2000AsioDriver::AddRef() {
    return m_refCount.fetch_add(1) + 1;
}

STDMETHODIMP_(ULONG) Bcd2000AsioDriver::Release() {
    ULONG count = m_refCount.fetch_sub(1) - 1;
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// WinUSB Hardware Management
// ============================================================================

bool Bcd2000AsioDriver::OpenUsbHardware() {
    CloseUsbHardware();
    bcd2000::TraceLog("ASIO", "OpenUsbHardware()");

    const std::wstring targetPath = bcd2000::FindBcd2000DevicePath();
    if (targetPath.empty()) {
        std::snprintf(m_errorMessage, sizeof(m_errorMessage),
                      "BCD2000 (1397:00BD) WinUSB device not found.");
        bcd2000::TraceLog("ASIO", "FindBcd2000DevicePath returned empty");
        return false;
    }

    m_hDevice = CreateFileW(targetPath.c_str(), GENERIC_WRITE | GENERIC_READ,
                            FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                            nullptr);

    if (m_hDevice == INVALID_HANDLE_VALUE) {
        const DWORD openErr = GetLastError();
        bcd2000::TraceLog("ASIO", "Initial CreateFileW failed err=%lu", openErr);
        if (openErr == ERROR_ACCESS_DENIED || openErr == ERROR_SHARING_VIOLATION) {
            // Standalone bcd2000_midi.dll may hold WinUSB; request graceful handoff
            for (int waitAttempt = 0; waitAttempt < 3 && m_hDevice == INVALID_HANDLE_VALUE; ++waitAttempt) {
                if (WaitNamedPipeW(bcd2000::kMidiCtrlPipeName, 120)) {
                    HANDLE hCtrl = CreateFileW(bcd2000::kMidiCtrlPipeName,
                                               GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                               OPEN_EXISTING, 0, nullptr);
                    if (hCtrl != INVALID_HANDLE_VALUE) {
                        const char cmd = 'R';
                        DWORD written = 0;
                        WriteFile(hCtrl, &cmd, 1, &written, nullptr);
                        char ack[4]{};
                        DWORD read = 0;
                        ReadFile(hCtrl, ack, sizeof(ack), &read, nullptr);
                        CloseHandle(hCtrl);
                        bcd2000::TraceLog("ASIO", "CtrlPipe handoff ack='%c' read=%lu",
                                          ack[0] ? ack[0] : '?', read);
                    }
                    for (int attempt = 0; attempt < 15 && m_hDevice == INVALID_HANDLE_VALUE; ++attempt) {
                        Sleep(20);
                        m_hDevice = CreateFileW(targetPath.c_str(),
                                                GENERIC_WRITE | GENERIC_READ,
                                                FILE_SHARE_WRITE | FILE_SHARE_READ,
                                                nullptr, OPEN_EXISTING,
                                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                                nullptr);
                    }
                    break;
                }
                Sleep(30);
            }
        }
    }

    if (m_hDevice == INVALID_HANDLE_VALUE) {
        DWORD finalErr = GetLastError();
        std::snprintf(m_errorMessage, sizeof(m_errorMessage),
                      "CreateFileW failed on BCD2000 (err=%lu).",
                      finalErr);
        bcd2000::TraceLog("ASIO", "OpenUsbHardware failed finalErr=%lu", finalErr);
        return false;
    }

    if (!WinUsb_Initialize(m_hDevice, &m_hInterface0)) {
        std::snprintf(m_errorMessage, sizeof(m_errorMessage),
                      "WinUsb_Initialize failed (err=%lu).", GetLastError());
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
        return false;
    }

    m_allInterfaces.push_back(m_hInterface0);
    for (UCHAR assocIdx = 0; assocIdx < 4; ++assocIdx) {
        WINUSB_INTERFACE_HANDLE hAssoc = nullptr;
        if (WinUsb_GetAssociatedInterface(m_hInterface0, assocIdx, &hAssoc)) {
            m_allInterfaces.push_back(hAssoc);
        }
    }

    bool foundAudioOut = false;
    for (size_t ifIdx = 0; ifIdx < m_allInterfaces.size(); ++ifIdx) {
        WINUSB_INTERFACE_HANDLE hIf = m_allInterfaces[ifIdx];
        for (UCHAR altSetting = 0; altSetting < 4; ++altSetting) {
            USB_INTERFACE_DESCRIPTOR ifDesc{};
            if (!WinUsb_QueryInterfaceSettings(hIf, altSetting, &ifDesc)) {
                break;
            }
            for (UCHAR pIdx = 0; pIdx < ifDesc.bNumEndpoints; ++pIdx) {
                WINUSB_PIPE_INFORMATION_EX pipeInfoEx{};
                WINUSB_PIPE_INFORMATION pipeInfo{};
                if (!WinUsb_QueryPipeEx(hIf, altSetting, pIdx, &pipeInfoEx)) {
                    if (!WinUsb_QueryPipe(hIf, altSetting, pIdx, &pipeInfo)) {
                        continue;
                    }
                    pipeInfoEx.PipeType = pipeInfo.PipeType;
                    pipeInfoEx.PipeId = pipeInfo.PipeId;
                    pipeInfoEx.MaximumPacketSize = pipeInfo.MaximumPacketSize;
                    pipeInfoEx.Interval = pipeInfo.Interval;
                    pipeInfoEx.MaximumBytesPerInterval =
                        pipeInfo.MaximumPacketSize;
                }

                bool isIn = USB_ENDPOINT_DIRECTION_IN(pipeInfoEx.PipeId);
                if (pipeInfoEx.PipeId == 0x01) {
                    m_hMidiInterface = hIf;
                    m_midiOutPipe = 0x01;
                } else if (pipeInfoEx.PipeId == 0x81) {
                    m_hMidiInterface = hIf;
                    m_midiInPipe = 0x81;
                } else if (pipeInfoEx.PipeId == 0x02 ||
                           (!isIn &&
                            pipeInfoEx.PipeType == UsbdPipeTypeIsochronous)) {
                    if (!foundAudioOut || pipeInfoEx.MaximumPacketSize > 0) {
                        m_hAudioInterface = hIf;
                        m_audioOutPipe = pipeInfoEx.PipeId;
                        m_audioAltSetting = altSetting;
                        m_audioOutPipeType = pipeInfoEx.PipeType;
                        m_audioOutBytesPerInterval =
                            (pipeInfoEx.MaximumBytesPerInterval > 0)
                                ? pipeInfoEx.MaximumBytesPerInterval
                                : pipeInfoEx.MaximumPacketSize;
                        foundAudioOut = true;
                    }
                } else if (pipeInfoEx.PipeId == 0x83) {
                    m_audioInPipe = 0x83;
                }
            }
        }
    }

    if (!m_hMidiInterface) {
        m_hMidiInterface = m_hInterface0;
    }
    if (!m_hAudioInterface) {
        m_hAudioInterface = m_hInterface0;
    }
    if (m_audioOutBytesPerInterval == 0) {
        m_audioOutBytesPerInterval = 360;
    }

    WinUsb_SetCurrentAlternateSetting(m_hAudioInterface, m_audioAltSetting);

    ULONG timeoutMs = 400;
    WinUsb_SetPipePolicy(m_hMidiInterface, m_midiOutPipe, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeoutMs), &timeoutMs);
    UCHAR autoClearStall = 1;
    WinUsb_SetPipePolicy(m_hMidiInterface, m_midiOutPipe, AUTO_CLEAR_STALL,
                         sizeof(autoClearStall), &autoClearStall);
    WinUsb_SetPipePolicy(m_hMidiInterface, m_midiInPipe, AUTO_CLEAR_STALL,
                         sizeof(autoClearStall), &autoClearStall);

    // Send 52-byte 8051 initialization handshake to Endpoint 0x01
    ULONG bytesWritten = 0;
    WinUsb_WritePipe(m_hMidiInterface, m_midiOutPipe,
                     const_cast<PUCHAR>(kBcd2000InitSequence),
                     sizeof(kBcd2000InitSequence), &bytesWritten, nullptr);

    Sleep(50);

    // Drain any pending startup bytes from Endpoint 0x81
    ULONG drainTimeoutMs = 25;
    WinUsb_SetPipePolicy(m_hMidiInterface, m_midiInPipe, PIPE_TRANSFER_TIMEOUT,
                         sizeof(drainTimeoutMs), &drainTimeoutMs);
    uint8_t drainBuf[64];
    for (int i = 0; i < 6; ++i) {
        ULONG bytesRead = 0;
        if (!WinUsb_ReadPipe(m_hMidiInterface, m_midiInPipe, drainBuf,
                             sizeof(drainBuf), &bytesRead, nullptr)) {
            break;
        }
    }

    // Restore infinite timeout on EP 0x81 IN before starting overlapped MIDI reader
    ULONG infTimeoutMs = 0;
    WinUsb_SetPipePolicy(m_hMidiInterface, m_midiInPipe, PIPE_TRANSFER_TIMEOUT,
                         sizeof(infTimeoutMs), &infTimeoutMs);

    m_hardwareConnected = true;
    bcd2000::TraceLog("ASIO", "OpenUsbHardware OK (outPipe=0x%02X inPipe=0x%02X pkt=%lu)",
                      m_audioOutPipe, m_audioInPipe, m_audioOutBytesPerInterval);
    StartMidiBridge();
    return true;
}

void Bcd2000AsioDriver::CloseUsbHardware() {
    StopMidiBridge();

    if (m_hardwareConnected && !HasConnectedMidiClients()) {
        SetStatusLeds(false);
    }
    for (size_t i = m_allInterfaces.size(); i > 0; --i) {
        if (m_allInterfaces[i - 1]) {
            WinUsb_Free(m_allInterfaces[i - 1]);
        }
    }
    m_allInterfaces.clear();
    m_hInterface0 = nullptr;
    m_hMidiInterface = nullptr;
    m_hAudioInterface = nullptr;

    if (m_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
    }
    m_hardwareConnected = false;
}

bool Bcd2000AsioDriver::SendRawMidiToUsb(const uint8_t *data, size_t len) {
    if (!m_hMidiInterface || m_midiOutPipe == 0 || !data || len == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_midiWriteMutex);
    return bcd2000::SendBcdMidiOverlapped(m_hMidiInterface, m_midiOutPipe, data, len);
}

bool Bcd2000AsioDriver::SendLedCommand(uint8_t cc, uint8_t value) {
    const uint8_t midi[3] = {0xB0, cc, value};
    return SendRawMidiToUsb(midi, sizeof(midi));
}

void Bcd2000AsioDriver::SetStatusLeds(bool turnOn) {
    if (!m_hMidiInterface || m_midiOutPipe == 0) {
        return;
    }
    const uint8_t val = turnOn ? 0x7F : 0x00;
    const uint8_t leds[4] = {kLedPlayA, kLedPlayB, kLedPflA, kLedPflB};

    // Pack all 4 LED updates into a single 64-byte frame with zero Sleep()
    // so ASIO start()/stop() never block the host audio thread.
    uint8_t midi[12]{};
    for (size_t i = 0; i < 4; ++i) {
        midi[i * 3 + 0] = 0xB0;
        midi[i * 3 + 1] = leds[i];
        midi[i * 3 + 2] = val;
    }
    SendRawMidiToUsb(midi, sizeof(midi));
}

void Bcd2000AsioDriver::FlashHardwareLeds() {
    if (!m_hMidiInterface || m_midiOutPipe == 0) {
        return;
    }
    // Launch detached helper thread to sweep all 26 LEDs ON, wait 350 ms, then OFF
    std::thread([this]() {
        for (uint8_t cc = 0x00; cc <= 0x19; ++cc) {
            SendLedCommand(cc, 0x7F);
        }
        Sleep(350);
        for (uint8_t cc = 0x00; cc <= 0x19; ++cc) {
            SendLedCommand(cc, 0x00);
        }
        SetStatusLeds(m_streamingActive.load(std::memory_order_relaxed));
    }).detach();
}

bool Bcd2000AsioDriver::HasConnectedMidiClients() {
    std::lock_guard<std::mutex> lock(m_midiClientsMutex);
    return !m_midiClients.empty();
}

void Bcd2000AsioDriver::StartMidiBridge() {
    if (m_midiBridgeRunning.load(std::memory_order_acquire) ||
        !m_hMidiInterface || m_midiInPipe == 0) {
        return;
    }
    m_hMidiStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_midiBridgeRunning.store(true, std::memory_order_release);
    m_midiInThread = std::thread(&Bcd2000AsioDriver::MidiUsbInThreadMain, this);
    m_midiPipeThread = std::thread(&Bcd2000AsioDriver::MidiPipeServerThreadMain, this);
}

void Bcd2000AsioDriver::StopMidiBridge() {
    if (!m_midiBridgeRunning.load(std::memory_order_acquire)) {
        return;
    }
    m_midiBridgeRunning.store(false, std::memory_order_release);
    if (m_hMidiStopEvent) {
        SetEvent(m_hMidiStopEvent);
    }
    if (m_hMidiInterface && m_midiInPipe != 0) {
        WinUsb_AbortPipe(m_hMidiInterface, m_midiInPipe);
    }
    {
        std::lock_guard<std::mutex> lock(m_midiClientsMutex);
        for (auto &c : m_midiClients) {
            c->alive.store(false, std::memory_order_release);
            if (c->hPipe != INVALID_HANDLE_VALUE) {
                CancelIoEx(c->hPipe, nullptr);
                DisconnectNamedPipe(c->hPipe);
            }
        }
    }
    if (m_midiInThread.joinable()) {
        m_midiInThread.join();
    }
    if (m_midiPipeThread.joinable()) {
        m_midiPipeThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_midiClientsMutex);
        m_midiClients.clear();
    }
    if (m_hMidiStopEvent) {
        CloseHandle(m_hMidiStopEvent);
        m_hMidiStopEvent = nullptr;
    }
}

void Bcd2000AsioDriver::MidiUsbInThreadMain() {
    UCHAR rawIo = 1;
    WinUsb_SetPipePolicy(m_hMidiInterface, m_midiInPipe, RAW_IO, sizeof(rawIo), &rawIo);

    HANDLE hReadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hReadEvent) {
        return;
    }

    bcd2000::MidiRunningStatusParser parser;
    uint8_t buf[64]{};

    while (m_midiBridgeRunning.load(std::memory_order_acquire)) {
        OVERLAPPED ov{};
        ov.hEvent = hReadEvent;
        ResetEvent(hReadEvent);

        ULONG bytesRead = 0;
        BOOL ok = WinUsb_ReadPipe(m_hMidiInterface, m_midiInPipe, buf, sizeof(buf),
                                  &bytesRead, &ov);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            HANDLE waits[2] = {m_hMidiStopEvent, hReadEvent};
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 || !m_midiBridgeRunning.load(std::memory_order_acquire)) {
                WinUsb_AbortPipe(m_hMidiInterface, m_midiInPipe);
                WaitForSingleObject(hReadEvent, 100);
                break;
            }
            ok = WinUsb_GetOverlappedResult(m_hMidiInterface, &ov, &bytesRead, FALSE);
        }

        if (ok && bytesRead > 0) {
            parser.FeedUsbPacket(buf, bytesRead, [&](DWORD packedMsg) {
                std::vector<std::shared_ptr<MidiPipeClient>> snapshot;
                {
                    std::lock_guard<std::mutex> lock(m_midiClientsMutex);
                    snapshot = m_midiClients;
                }
                for (auto &client : snapshot) {
                    if (!client->alive.load(std::memory_order_relaxed) ||
                        client->hPipe == INVALID_HANDLE_VALUE) {
                        continue;
                    }
                    std::lock_guard<std::mutex> wLock(client->writeMutex);
                    OVERLAPPED wOv{};
                    wOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    if (wOv.hEvent) {
                        DWORD written = 0;
                        BOOL wOk = WriteFile(client->hPipe, &packedMsg, sizeof(packedMsg),
                                             &written, &wOv);
                        if (!wOk && GetLastError() == ERROR_IO_PENDING) {
                            if (WaitForSingleObject(wOv.hEvent, 20) == WAIT_OBJECT_0) {
                                GetOverlappedResult(client->hPipe, &wOv, &written, FALSE);
                            } else {
                                CancelIoEx(client->hPipe, &wOv);
                            }
                        }
                        CloseHandle(wOv.hEvent);
                    }
                }
            });
        } else if (!ok) {
            if (!m_midiBridgeRunning.load(std::memory_order_acquire)) {
                break;
            }
            Sleep(10);
        }
    }

    CloseHandle(hReadEvent);
}

void Bcd2000AsioDriver::MidiPipeServerThreadMain() {
    HANDLE hConnEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hConnEvent) {
        return;
    }

    std::vector<std::thread> clientReaders;

    while (m_midiBridgeRunning.load(std::memory_order_acquire)) {
        HANDLE hPipe = CreateNamedPipeW(
            bcd2000::kAsioMidiPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(50);
            continue;
        }

        OVERLAPPED connOv{};
        connOv.hEvent = hConnEvent;
        ResetEvent(hConnEvent);
        BOOL connected = ConnectNamedPipe(hPipe, &connOv);
        DWORD connErr = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && connErr == ERROR_IO_PENDING) {
            HANDLE waits[2] = {m_hMidiStopEvent, hConnEvent};
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 || !m_midiBridgeRunning.load(std::memory_order_acquire)) {
                CancelIoEx(hPipe, &connOv);
                CloseHandle(hPipe);
                break;
            }
            DWORD dummy = 0;
            connected = GetOverlappedResult(hPipe, &connOv, &dummy, FALSE);
        } else if (!connected && connErr == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }

        if (!connected) {
            CloseHandle(hPipe);
            continue;
        }

        auto client = std::make_shared<MidiPipeClient>();
        client->hPipe = hPipe;
        {
            std::lock_guard<std::mutex> lock(m_midiClientsMutex);
            m_midiClients.push_back(client);
        }

        clientReaders.emplace_back([this, client]() {
            HANDLE hEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (hEv) {
                uint8_t msg[64]{};
                while (m_midiBridgeRunning.load(std::memory_order_acquire) &&
                       client->alive.load(std::memory_order_acquire)) {
                    OVERLAPPED rOv{};
                    rOv.hEvent = hEv;
                    ResetEvent(hEv);
                    DWORD read = 0;
                    BOOL rOk = ReadFile(client->hPipe, msg, sizeof(msg), &read, &rOv);
                    DWORD rErr = rOk ? ERROR_SUCCESS : GetLastError();
                    if (!rOk && rErr == ERROR_IO_PENDING) {
                        HANDLE waits[2] = {m_hMidiStopEvent, hEv};
                        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                        if (w == WAIT_OBJECT_0 ||
                            !m_midiBridgeRunning.load(std::memory_order_acquire)) {
                            CancelIoEx(client->hPipe, &rOv);
                            break;
                        }
                        rOk = GetOverlappedResult(client->hPipe, &rOv, &read, FALSE);
                    }
                    if (!rOk || read < 2) {
                        break;
                    }
                    const size_t payloadLen = std::min<size_t>(msg[0], read - 1);
                    if (payloadLen > 0) {
                        SendRawMidiToUsb(&msg[1], payloadLen);
                    }
                }
                CloseHandle(hEv);
            }
            client->alive.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(m_midiClientsMutex);
                m_midiClients.erase(
                    std::remove(m_midiClients.begin(), m_midiClients.end(), client),
                    m_midiClients.end());
            }
        });
    }

    for (auto &t : clientReaders) {
        if (t.joinable()) {
            t.join();
        }
    }
    CloseHandle(hConnEvent);
}

// ============================================================================
// IASIO Core Methods
// ============================================================================

ASIOBool Bcd2000AsioDriver::init(void *sysHandle) {
    m_sysHandle = sysHandle;
    bcd2000::TraceLog("ASIO", "init(state=%d)", static_cast<int>(m_state));

    if (m_state != DriverState::Loaded) {
        return ASIOTrue;
    }

    // Open or create shared memory state for control panel IPC
    m_sharedState = bcd2000::OpenOrCreateSharedState(&m_hSharedMem, true);
    if (m_sharedState) {
        m_sharedState->driverActive = 1;
        m_sharedState->activePid = GetCurrentProcessId();

        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH) > 0) {
            const wchar_t *fname = std::wcsrchr(path, L'\\');
            if (fname) fname++; else fname = path;
            std::wcsncpy(m_sharedState->dawProcessName, fname, 63);
        }

        m_bufferSize = m_sharedState->bufferSize;
        m_preferredBufSize = m_bufferSize;
    }

    // Open the physical BCD2000 via WinUSB (falls back gracefully to timer clock
    // if run in an environment without the USB hardware attached)
    OpenUsbHardware();

    if (m_sharedState) {
        m_sharedState->hardwareConnected = m_hardwareConnected ? 1 : 0;
    }

    m_state = DriverState::Initialized;
    return ASIOTrue;
}

void Bcd2000AsioDriver::getDriverName(char *name) {
    if (name) {
        std::snprintf(name, 32, "%s", kBcd2000AsioDriverNameA);
    }
}

long Bcd2000AsioDriver::getDriverVersion() {
    return 0x00020003L; // ASIO 2.3
}

void Bcd2000AsioDriver::getErrorMessage(char *string) {
    if (string) {
        std::snprintf(string, 124, "%s", m_errorMessage);
    }
}

ASIOError Bcd2000AsioDriver::getChannels(long *numInputChannels,
                                         long *numOutputChannels) {
    if (!numInputChannels || !numOutputChannels) {
        return ASE_InvalidParameter;
    }
    *numInputChannels = kNumInputs;
    *numOutputChannels = kNumOutputs;
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::getLatencies(long *inputLatency,
                                          long *outputLatency) {
    if (!inputLatency || !outputLatency) {
        return ASE_InvalidParameter;
    }
    // Input latency: 1 ASIO buffer; Output latency: 2 ASIO buffers (~5.8 ms at 256 samples)
    *inputLatency = m_bufferSize;
    *outputLatency = m_bufferSize * 2;
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::getBufferSize(long *minSize, long *maxSize,
                                           long *preferredSize,
                                           long *granularity) {
    if (!minSize || !maxSize || !preferredSize || !granularity) {
        return ASE_InvalidParameter;
    }
    *minSize = kMinBufferSize;         // 64 samples (~1.45 ms)
    *maxSize = kMaxBufferSize;         // 4096 samples (~92.88 ms)
    *preferredSize = m_preferredBufSize; // Dynamically returns user-configured preferred size
    *granularity = kBufferGranularity; // 64 samples
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::canSampleRate(ASIOSampleRate sampleRate) {
    // Support all theoretical sample rates of the BCD2000 audio architecture
    // (TI TUSB3200 USB controller + Cirrus Logic CS4220 24-bit/96kHz codec):
    // 32000 Hz, 44100 Hz, 48000 Hz, 88200 Hz, and 96000 Hz.
    static constexpr double kSupportedRates[] = {
        32000.0, 44100.0, 48000.0, 88200.0, 96000.0
    };
    for (double rate : kSupportedRates) {
        if (sampleRate >= (rate - 1.0) && sampleRate <= (rate + 1.0)) {
            return ASE_OK;
        }
    }
    return ASE_NoClock;
}

ASIOError Bcd2000AsioDriver::getSampleRate(ASIOSampleRate *sampleRate) {
    if (!sampleRate) {
        return ASE_InvalidParameter;
    }
    *sampleRate = m_sampleRate;
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::setSampleRate(ASIOSampleRate sampleRate) {
    static constexpr double kSupportedRates[] = {
        32000.0, 44100.0, 48000.0, 88200.0, 96000.0
    };
    double matchedRate = 0.0;
    for (double rate : kSupportedRates) {
        if (sampleRate >= (rate - 1.0) && sampleRate <= (rate + 1.0)) {
            matchedRate = rate;
            break;
        }
    }
    if (matchedRate <= 0.0) {
        return ASE_NoClock;
    }

    const bool changed = (m_sampleRate != matchedRate);
    m_sampleRate = matchedRate;
    bcd2000::TraceLog("ASIO", "setSampleRate(%.0f) changed=%d", m_sampleRate, changed ? 1 : 0);
    if (changed && m_callbacks.sampleRateDidChange) {
        m_callbacks.sampleRateDidChange(m_sampleRate);
    }
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::getClockSources(ASIOClockSource *clocks,
                                             long *numSources) {
    if (!clocks || !numSources || *numSources < 1) {
        return ASE_InvalidParameter;
    }
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    std::snprintf(clocks[0].name, sizeof(clocks[0].name),
                  "BCD2000 Internal (32k-96kHz)");
    *numSources = 1;
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::setClockSource(long reference) {
    if (reference == 0) {
        return ASE_OK;
    }
    return ASE_InvalidParameter;
}

ASIOError Bcd2000AsioDriver::getSamplePosition(ASIOSamples *sPos,
                                               ASIOTimeStamp *tStamp) {
    if (!sPos || !tStamp) {
        return ASE_InvalidParameter;
    }
    Uint64ToAsioSamples(m_samplePosition.load(std::memory_order_acquire), *sPos);
    Uint64ToAsioTimeStamp(m_systemTimeNs.load(std::memory_order_acquire),
                          *tStamp);
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::getChannelInfo(ASIOChannelInfo *info) {
    if (!info) {
        return ASE_InvalidParameter;
    }
    if (info->channel < 0 ||
        (info->isInput && info->channel >= kNumInputs) ||
        (!info->isInput && info->channel >= kNumOutputs)) {
        return ASE_InvalidParameter;
    }

    info->type = ASIOSTInt16LSB;
    info->channelGroup = 0;

    if (info->isInput) {
        info->isActive = m_inputActive[info->channel] ? ASIOTrue : ASIOFalse;
        static const char *kInputNames[kNumInputs] = {
            "In 1: Deck A / Mic Left",
            "In 2: Deck A / Mic Right",
            "In 3: Deck B Left",
            "In 4: Deck B Right"};
        std::snprintf(info->name, sizeof(info->name), "%s",
                      kInputNames[info->channel]);
    } else {
        info->isActive = m_outputActive[info->channel] ? ASIOTrue : ASIOFalse;
        static const char *kOutputNames[kNumOutputs] = {
            "Out 1: Master Left (RCA)",
            "Out 2: Master Right (RCA)",
            "Out 3: Phones Left (Cue)",
            "Out 4: Phones Right (Cue)"};
        std::snprintf(info->name, sizeof(info->name), "%s",
                      kOutputNames[info->channel]);
    }

    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::createBuffers(ASIOBufferInfo *bufferInfos,
                                           long numChannels, long bufferSize,
                                           ASIOCallbacks *callbacks) {
    if (!bufferInfos || !callbacks || !callbacks->bufferSwitch ||
        numChannels <= 0 || bufferSize < 32 || bufferSize > 4096) {
        return ASE_InvalidParameter;
    }

    if (m_state == DriverState::Running) {
        stop();
    }
    disposeBuffers();

    m_bufferSize = bufferSize;
    m_callbacks = *callbacks;

    // Allocate double buffers for all channels
    for (long c = 0; c < kNumInputs; ++c) {
        m_inputStorage[c][0].assign(m_bufferSize, 0);
        m_inputStorage[c][1].assign(m_bufferSize, 0);
        m_inputActive[c] = false;
    }
    for (long c = 0; c < kNumOutputs; ++c) {
        m_outputStorage[c][0].assign(m_bufferSize, 0);
        m_outputStorage[c][1].assign(m_bufferSize, 0);
        m_outputActive[c] = false;
    }

    for (long i = 0; i < numChannels; ++i) {
        ASIOBufferInfo &bi = bufferInfos[i];
        if (bi.isInput) {
            if (bi.channelNum < 0 || bi.channelNum >= kNumInputs) {
                disposeBuffers();
                return ASE_InvalidParameter;
            }
            m_inputActive[bi.channelNum] = true;
            bi.buffers[0] = m_inputStorage[bi.channelNum][0].data();
            bi.buffers[1] = m_inputStorage[bi.channelNum][1].data();
        } else {
            if (bi.channelNum < 0 || bi.channelNum >= kNumOutputs) {
                disposeBuffers();
                return ASE_InvalidParameter;
            }
            m_outputActive[bi.channelNum] = true;
            bi.buffers[0] = m_outputStorage[bi.channelNum][0].data();
            bi.buffers[1] = m_outputStorage[bi.channelNum][1].data();
        }
    }

    // Check if the ASIO host supports bufferSwitchTimeInfo
    m_supportsTimeInfo = false;
    if (m_callbacks.asioMessage && m_callbacks.bufferSwitchTimeInfo) {
        if (m_callbacks.asioMessage(kAsioSelectorSupported,
                                    kAsioSupportsTimeInfo, nullptr,
                                    nullptr) == 1) {
            if (m_callbacks.asioMessage(kAsioSupportsTimeInfo, 0, nullptr,
                                        nullptr) == 1) {
                m_supportsTimeInfo = true;
            }
        }
    }

    m_state = DriverState::Prepared;
    bcd2000::TraceLog("ASIO",
                      "createBuffers numCh=%ld bufSize=%ld rate=%.0f timeInfo=%d outActive=[%d,%d,%d,%d]",
                      numChannels, bufferSize, m_sampleRate, m_supportsTimeInfo ? 1 : 0,
                      m_outputActive[0] ? 1 : 0, m_outputActive[1] ? 1 : 0,
                      m_outputActive[2] ? 1 : 0, m_outputActive[3] ? 1 : 0);
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::disposeBuffers() {
    if (m_state == DriverState::Running) {
        stop();
    }

    for (long c = 0; c < kNumInputs; ++c) {
        m_inputStorage[c][0].clear();
        m_inputStorage[c][1].clear();
        m_inputActive[c] = false;
    }
    for (long c = 0; c < kNumOutputs; ++c) {
        m_outputStorage[c][0].clear();
        m_outputStorage[c][1].clear();
        m_outputActive[c] = false;
    }

    std::memset(&m_callbacks, 0, sizeof(m_callbacks));
    if (m_state == DriverState::Prepared) {
        m_state = DriverState::Initialized;
    }
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::start() {
    if (m_state != DriverState::Prepared || !m_callbacks.bufferSwitch) {
        return ASE_InvalidMode;
    }

    // If hardware wasn't opened during init(), try once more now
    if (!m_hardwareConnected) {
        OpenUsbHardware();
    }
    if (m_hardwareConnected && !HasConnectedMidiClients()) {
        SetStatusLeds(true);
    }

    m_toggleIndex = 0;
    m_samplePosition.store(0, std::memory_order_release);
    const uint64_t initialNowNs = QuerySystemTimeNanoseconds();
    m_systemTimeNs.store(initialNowNs, std::memory_order_release);
    m_smoothedSystemTimeNs = static_cast<double>(initialNowNs);
    m_ringWriteFrames.store(0, std::memory_order_release);
    m_ringReadFrames.store(0, std::memory_order_release);
    m_resamplePhase = 0.0;
    std::memset(m_resampleHistory, 0, sizeof(m_resampleHistory));
    std::fill(m_outRingBuffer.begin(), m_outRingBuffer.end(), static_cast<int16_t>(0));

    m_hAsioTriggerEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_usbRateReady.store(false, std::memory_order_release);
    m_exactModeActive.store(true, std::memory_order_release);
    m_usbTargetRate.store(kDefaultSampleRate, std::memory_order_release);
    m_prefillReady.store(false, std::memory_order_release);
    m_streamingActive.store(true, std::memory_order_release);
    m_state = DriverState::Running;

    if (m_sharedState) {
        m_sharedState->isRunning = 1;
        m_sharedState->sampleRate = m_sampleRate;
        m_sharedState->bufferSize = static_cast<int32_t>(m_bufferSize);
        m_sharedState->hardwareConnected = m_hardwareConnected ? 1 : 0;
    }

    // Launch decoupled real-time threads:
    // 1. AsioHostThread: drives DAW host bufferSwitch callbacks and fills the ring buffer
    // 2. UsbStreamingThread: drives continuous USB Isochronous Endpoint 0x02 OUT transfers
    bcd2000::TraceLog("ASIO", "start() hw=%d rate=%.0f buf=%ld midiClients=%d",
                      m_hardwareConnected ? 1 : 0, m_sampleRate, m_bufferSize,
                      HasConnectedMidiClients() ? 1 : 0);
    m_asioThread = std::thread(&Bcd2000AsioDriver::AsioHostThreadMain, this);
    m_usbThread = std::thread(&Bcd2000AsioDriver::UsbStreamingThreadMain, this);

    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::stop() {
    if (m_state != DriverState::Running) {
        return ASE_OK;
    }

    if (m_sharedState) {
        m_sharedState->isRunning = 0;
    }

    m_streamingActive.store(false, std::memory_order_release);
    if (m_hAsioTriggerEvent) {
        SetEvent(m_hAsioTriggerEvent);
    }

    if (m_asioThread.joinable()) {
        m_asioThread.join();
    }
    if (m_usbThread.joinable()) {
        m_usbThread.join();
    }

    if (m_hAsioTriggerEvent) {
        CloseHandle(m_hAsioTriggerEvent);
        m_hAsioTriggerEvent = nullptr;
    }

    if (m_hardwareConnected && !HasConnectedMidiClients()) {
        SetStatusLeds(false);
    }

    m_state = DriverState::Prepared;
    bcd2000::TraceLog("ASIO", "stop() complete");
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::controlPanel() {
    bcd2000::ShowBcd2000ControlPanel(static_cast<HWND>(m_sysHandle), false, this);
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::future(long /*selector*/, void * /*opt*/) {
    return ASE_InvalidParameter;
}

ASIOError Bcd2000AsioDriver::outputReady() {
    return ASE_OK;
}

// ============================================================================
// Real-Time Callback & Isochronous Streaming Engine
// ============================================================================

uint64_t Bcd2000AsioDriver::QuerySystemTimeNanoseconds() const {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    if (m_qpcFreq.QuadPart <= 0) {
        return 0;
    }
    return static_cast<uint64_t>(
        (static_cast<double>(counter.QuadPart) * 1.0e9) /
        static_cast<double>(m_qpcFreq.QuadPart));
}

void Bcd2000AsioDriver::InvokeHostBufferSwitch() {
    const long bufIdx = m_toggleIndex;
    const uint64_t rawNowNs = QuerySystemTimeNanoseconds();
    const uint64_t curPos = m_samplePosition.load(std::memory_order_relaxed);

    // Filter USB URB completion timing jitter from ASIOTimeInfo.systemTime so
    // DAW/DJ transport PLLs see a smooth, strictly monotonic audio clock.
    // Keep wall-clock time anchored during startup prefill bursts.
    const double nominalStepNs =
        (static_cast<double>(m_bufferSize) * 1.0e9) / m_sampleRate;
    if (curPos == 0 || m_smoothedSystemTimeNs <= 0.0 ||
        !m_prefillReady.load(std::memory_order_acquire)) {
        m_smoothedSystemTimeNs = static_cast<double>(rawNowNs);
    } else {
        m_smoothedSystemTimeNs += nominalStepNs;
        const double driftNs =
            static_cast<double>(rawNowNs) - m_smoothedSystemTimeNs;
        m_smoothedSystemTimeNs += driftNs * 0.02;
    }
    const uint64_t nowNs = static_cast<uint64_t>(m_smoothedSystemTimeNs);
    m_systemTimeNs.store(nowNs, std::memory_order_release);

    // Zero input buffers for this half-buffer before handing to host
    for (long c = 0; c < kNumInputs; ++c) {
        if (m_inputActive[c]) {
            std::memset(m_inputStorage[c][bufIdx].data(), 0,
                        static_cast<size_t>(m_bufferSize) * sizeof(int16_t));
        }
    }

    if (m_supportsTimeInfo && m_callbacks.bufferSwitchTimeInfo) {
        std::memset(&m_asioTime, 0, sizeof(m_asioTime));
        m_asioTime.timeInfo.speed = 1.0;
        m_asioTime.timeInfo.sampleRate = m_sampleRate;
        Uint64ToAsioTimeStamp(nowNs, m_asioTime.timeInfo.systemTime);
        Uint64ToAsioSamples(curPos, m_asioTime.timeInfo.samplePosition);
        m_asioTime.timeInfo.flags =
            kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;

        m_callbacks.bufferSwitchTimeInfo(&m_asioTime, bufIdx, ASIOTrue);
    } else if (m_callbacks.bufferSwitch) {
        m_callbacks.bufferSwitch(bufIdx, ASIOTrue);
    }

    // Process control panel triggers & commands
    if (m_sharedState) {
        if (m_sharedState->resetGlitchCounter) {
            m_xrunCount.store(0, std::memory_order_relaxed);
            m_sharedState->xrunCount = 0;
            m_sharedState->resetGlitchCounter = 0;
        }
        if (m_sharedState->flashLedsTrigger) {
            m_sharedState->flashLedsTrigger = 0;
            FlashHardwareLeds();
        }
        if (m_sharedState->requestedBufferSize != 0) {
            long req = static_cast<long>(m_sharedState->requestedBufferSize);
            m_sharedState->requestedBufferSize = 0;
            if (req >= kMinBufferSize && req <= kMaxBufferSize && req != m_bufferSize) {
                m_preferredBufSize = req;
                if (m_callbacks.asioMessage) {
                    m_callbacks.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
                }
            }
        }
    }

    const int routingMode = m_sharedState ? m_sharedState->routingMode : 0;
    const bool monoDownmix = m_sharedState ? (m_sharedState->monoDownmix != 0) : false;
    const int trimDb = m_sharedState ? m_sharedState->outputTrimDb : 0;
    const bool masterMute = m_sharedState ? (m_sharedState->masterMute != 0) : false;
    const bool phonesMute = m_sharedState ? (m_sharedState->phonesMute != 0) : false;

    // Fixed-point scale factor for digital trim (0, -3, -6, -12 dB)
    int32_t trimMult = 32768; // 0 dB unity
    if (trimDb == -3) trimMult = 23170;       // -3.0 dB (~0.707)
    else if (trimDb == -6) trimMult = 16422;  // -6.0 dB (~0.501)
    else if (trimDb == -12) trimMult = 8232;  // -12.0 dB (~0.251)

    // Interleave the 4 non-interleaved 16-bit ASIO output channels (ASIOSTInt16LSB)
    // into m_outRingBuffer (4-channel interleaved S16_LE).
    const int16_t *ch0 = m_outputActive[0] ? m_outputStorage[0][bufIdx].data() : nullptr;
    const int16_t *ch1 = m_outputActive[1] ? m_outputStorage[1][bufIdx].data() : nullptr;
    const int16_t *ch2 = m_outputActive[2] ? m_outputStorage[2][bufIdx].data() : nullptr;
    const int16_t *ch3 = m_outputActive[3] ? m_outputStorage[3][bufIdx].data() : nullptr;

    int16_t maxPk0 = 0, maxPk1 = 0, maxPk2 = 0, maxPk3 = 0;

    auto ProcessOutputFrame = [&](long s, int16_t &o0, int16_t &o1, int16_t &o2, int16_t &o3) {
        int16_t in0 = ch0 ? ch0[s] : 0;
        int16_t in1 = ch1 ? ch1[s] : 0;
        int16_t in2 = ch2 ? ch2[s] : 0;
        int16_t in3 = ch3 ? ch3[s] : 0;

        if (routingMode == bcd2000::Routing_Swap) {
            o0 = in2; o1 = in3;
            o2 = in0; o3 = in1;
        } else if (routingMode == bcd2000::Routing_Mirror) {
            o0 = in0; o1 = in1;
            o2 = in0; o3 = in1;
        } else {
            o0 = in0; o1 = in1;
            o2 = in2; o3 = in3;
        }

        if (monoDownmix) {
            int32_t m = (static_cast<int32_t>(o0) + static_cast<int32_t>(o1)) / 2;
            o0 = static_cast<int16_t>(m);
            o1 = static_cast<int16_t>(m);
        }

        if (trimMult < 32768) {
            o0 = static_cast<int16_t>((static_cast<int32_t>(o0) * trimMult) >> 15);
            o1 = static_cast<int16_t>((static_cast<int32_t>(o1) * trimMult) >> 15);
            o2 = static_cast<int16_t>((static_cast<int32_t>(o2) * trimMult) >> 15);
            o3 = static_cast<int16_t>((static_cast<int32_t>(o3) * trimMult) >> 15);
        }

        if (masterMute) {
            o0 = 0; o1 = 0;
        }
        if (phonesMute) {
            o2 = 0; o3 = 0;
        }

        int16_t a0 = (o0 < 0) ? -o0 : o0;
        int16_t a1 = (o1 < 0) ? -o1 : o1;
        int16_t a2 = (o2 < 0) ? -o2 : o2;
        int16_t a3 = (o3 < 0) ? -o3 : o3;
        if (a0 > maxPk0) maxPk0 = a0;
        if (a1 > maxPk1) maxPk1 = a1;
        if (a2 > maxPk2) maxPk2 = a2;
        if (a3 > maxPk3) maxPk3 = a3;
    };

    const size_t mask = kRingCapacityFrames - 1;
    uint64_t writeIdx = m_ringWriteFrames.load(std::memory_order_relaxed);

    // Match production rate to the active USB transport rate determined by UsbStreamingThreadMain.
    // In exact crystal-locked 44.1 kHz mode, ASIO callbacks naturally pace to the BCD2000 DAC crystal
    // via m_ringReadFrames headroom (~44,098 Hz nominal 44.1k), allowing bit-exact 1:1 copy without
    // resampling when host sample rate is 44.1 kHz. Non-44.1k host rates resample against live EP 0x83 IN rate.
    const double usbTargetRate = m_usbTargetRate.load(std::memory_order_acquire);
    const bool exactMode = m_exactModeActive.load(std::memory_order_acquire);
    const bool exactRateMatch = exactMode
                                    ? (std::abs(m_sampleRate - kDefaultSampleRate) < 0.01)
                                    : (std::abs(m_sampleRate - usbTargetRate) < 0.01);

    if (exactRateMatch) {
        for (long s = 0; s < m_bufferSize; ++s) {
            const size_t slot = static_cast<size_t>(writeIdx & mask) * 4;
            int16_t o0, o1, o2, o3;
            ProcessOutputFrame(s, o0, o1, o2, o3);
            m_outRingBuffer[slot + 0] = o0;
            m_outRingBuffer[slot + 1] = o1;
            m_outRingBuffer[slot + 2] = o2;
            m_outRingBuffer[slot + 3] = o3;
            writeIdx++;
        }
    } else {
        const double step = m_sampleRate / usbTargetRate;

        for (long s = 0; s < m_bufferSize; ++s) {
            int16_t o0, o1, o2, o3;
            ProcessOutputFrame(s, o0, o1, o2, o3);

            // Shift 4-sample history window and push current input sample
            for (int i = 0; i < 3; ++i) {
                for (int c = 0; c < kNumOutputs; ++c) {
                    m_resampleHistory[i][c] = m_resampleHistory[i + 1][c];
                }
            }
            m_resampleHistory[3][0] = o0;
            m_resampleHistory[3][1] = o1;
            m_resampleHistory[3][2] = o2;
            m_resampleHistory[3][3] = o3;

            while (m_resamplePhase < 1.0) {
                const double t = m_resamplePhase;
                const size_t slot = static_cast<size_t>(writeIdx & mask) * 4;
                for (int c = 0; c < kNumOutputs; ++c) {
                    const double y0 = static_cast<double>(m_resampleHistory[0][c]);
                    const double y1 = static_cast<double>(m_resampleHistory[1][c]);
                    const double y2 = static_cast<double>(m_resampleHistory[2][c]);
                    const double y3 = static_cast<double>(m_resampleHistory[3][c]);

                    // 4-point, 3rd-order Hermite interpolation (Catmull-Rom spline)
                    const double c0 = y1;
                    const double c1 = 0.5 * (y2 - y0);
                    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
                    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
                    const double val = ((c3 * t + c2) * t + c1) * t + c0;

                    const int32_t iv = static_cast<int32_t>(val >= 0.0 ? (val + 0.5) : (val - 0.5));
                    m_outRingBuffer[slot + c] =
                        static_cast<int16_t>(std::clamp(iv, -32768, 32767));
                }
                writeIdx++;
                m_resamplePhase += step;
            }
            m_resamplePhase -= 1.0;
        }
    }

    if (m_sharedState) {
        int16_t old0 = (m_sharedState->peakMasterL * 9) / 10;
        int16_t old1 = (m_sharedState->peakMasterR * 9) / 10;
        int16_t old2 = (m_sharedState->peakPhonesL * 9) / 10;
        int16_t old3 = (m_sharedState->peakPhonesR * 9) / 10;
        m_sharedState->peakMasterL = (std::max)(maxPk0, old0);
        m_sharedState->peakMasterR = (std::max)(maxPk1, old1);
        m_sharedState->peakPhonesL = (std::max)(maxPk2, old2);
        m_sharedState->peakPhonesR = (std::max)(maxPk3, old3);
    }

    m_ringWriteFrames.store(writeIdx, std::memory_order_release);
    m_samplePosition.store(curPos + static_cast<uint64_t>(m_bufferSize),
                           std::memory_order_release);
    m_toggleIndex ^= 1;
}

void Bcd2000AsioDriver::AsioHostThreadMain() {
    DWORD mmcssTaskIdx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIdx);
    if (hMmcss) {
        AvSetMmThreadPriority(hMmcss, AVRT_PRIORITY_CRITICAL);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    // Wait until UsbStreamingThreadMain determines the active USB transport rate
    for (int w = 0; w < 50 && !m_usbRateReady.load(std::memory_order_acquire) &&
                    m_streamingActive.load(std::memory_order_acquire);
         ++w) {
        Sleep(10);
    }

    // Pre-fill the ring buffer with 3 full ASIO buffers before releasing the USB thread
    for (int i = 0; i < 3 && m_streamingActive.load(std::memory_order_relaxed); ++i) {
        InvokeHostBufferSwitch();
    }
    m_prefillReady.store(true, std::memory_order_release);

    while (m_streamingActive.load(std::memory_order_acquire)) {
        const uint64_t writeIdx = m_ringWriteFrames.load(std::memory_order_acquire);
        const uint64_t readIdx = m_ringReadFrames.load(std::memory_order_acquire);
        const size_t buffered = (writeIdx >= readIdx) ? static_cast<size_t>(writeIdx - readIdx) : 0;

        int hrMultiplier = m_sharedState ? m_sharedState->safetyHeadroom : 3;
        if (hrMultiplier < 1) hrMultiplier = 1;
        if (hrMultiplier > 5) hrMultiplier = 5;
        const size_t targetHeadroomFrames =
            std::max<size_t>(static_cast<size_t>(512 * hrMultiplier),
                             static_cast<size_t>(m_bufferSize * hrMultiplier));

        if (buffered < targetHeadroomFrames) {
            InvokeHostBufferSwitch();
        } else {
            // Wait for USB streaming thread to consume frames, or 5 ms timeout
            WaitForSingleObject(m_hAsioTriggerEvent, 5);
        }
    }

    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }
}

void Bcd2000AsioDriver::StreamingThreadMain() {
    UsbStreamingThreadMain();
}

void Bcd2000AsioDriver::UsbStreamingThreadMain() {
    DWORD mmcssTaskIdx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIdx);
    if (hMmcss) {
        AvSetMmThreadPriority(hMmcss, AVRT_PRIORITY_CRITICAL);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    // If hardware is not connected, run high-precision fallback timer loop
    if (!m_hardwareConnected || !m_hAudioInterface) {
        bcd2000::TraceLog("ASIO", "UsbStreamingThreadMain: TIMER FALLBACK (no hardware!)");
        m_usbTargetRate.store(m_sampleRate, std::memory_order_release);
        m_usbRateReady.store(true, std::memory_order_release);
        const double periodSec = static_cast<double>(m_bufferSize) / m_sampleRate;
        auto nextWake = std::chrono::steady_clock::now();
        while (m_streamingActive.load(std::memory_order_acquire)) {
            nextWake += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(periodSec));
            std::this_thread::sleep_until(nextWake);
            m_ringReadFrames.fetch_add(m_bufferSize, std::memory_order_release);
            if (m_hAsioTriggerEvent) {
                SetEvent(m_hAsioTriggerEvent);
            }
        }
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        return;
    }

    const ULONG packetSize = (m_audioOutBytesPerInterval >= 8) ? m_audioOutBytesPerInterval : 384;
    const bool isIsoch = (m_audioOutPipeType == UsbdPipeTypeIsochronous);

    // Probe whether WinUSB allows single-packet 352-byte transfers so we can deliver
    // crystal-locked ~44.1 kHz (352/360 B per 1 ms) using Implicit Feedback from EP 0x83 IN
    // to match the BCD2000 TUSB3200 / CS4220 crystal without drift or clicks.
    bool useExact44100 = (isIsoch && packetSize >= 360);
    size_t numUrbs = useExact44100 ? 64 : 6;
    ULONG slotStride = useExact44100 ? packetSize : static_cast<ULONG>(16 * packetSize);
    ULONG totalDmaBytes = static_cast<ULONG>(numUrbs * slotStride);

    std::vector<uint8_t> dmaBuffer(totalDmaBytes, 0);
    WINUSB_ISOCH_BUFFER_HANDLE hIsochBuffer = nullptr;

    WinUsb_ResetPipe(m_hAudioInterface, m_audioOutPipe);
    if (isIsoch) {
        if (!WinUsb_RegisterIsochBuffer(m_hAudioInterface, m_audioOutPipe,
                                        dmaBuffer.data(), totalDmaBytes,
                                        &hIsochBuffer)) {
            hIsochBuffer = nullptr;
        }
    }

    if (useExact44100) {
        bool probeAccepted = false;
        if (hIsochBuffer) {
            OVERLAPPED probeOv{};
            probeOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (probeOv.hEvent) {
                BOOL ok = WinUsb_WriteIsochPipeAsap(hIsochBuffer, 0, 352, FALSE, &probeOv);
                DWORD err = ok ? ERROR_SUCCESS : GetLastError();
                if (ok || err == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(probeOv.hEvent, 50) == WAIT_OBJECT_0) {
                        DWORD xfer = 0;
                        if (WinUsb_GetOverlappedResult(m_hAudioInterface, &probeOv, &xfer, FALSE)) {
                            probeAccepted = true;
                        }
                    } else {
                        WinUsb_AbortPipe(m_hAudioInterface, m_audioOutPipe);
                    }
                }
                CloseHandle(probeOv.hEvent);
            }
        }

        if (!probeAccepted) {
            useExact44100 = false;
            if (hIsochBuffer) {
                WinUsb_UnregisterIsochBuffer(hIsochBuffer);
                hIsochBuffer = nullptr;
            }
            WinUsb_ResetPipe(m_hAudioInterface, m_audioOutPipe);
            numUrbs = 6;
            slotStride = static_cast<ULONG>(16 * packetSize);
            totalDmaBytes = static_cast<ULONG>(numUrbs * slotStride);
            dmaBuffer.assign(totalDmaBytes, 0);
            if (!WinUsb_RegisterIsochBuffer(m_hAudioInterface, m_audioOutPipe,
                                            dmaBuffer.data(), totalDmaBytes,
                                            &hIsochBuffer)) {
                hIsochBuffer = nullptr;
            }
        }
    }

    // Start EP 0x83 IN implicit clock tracker when running in exact 44.1 kHz mode.
    // Reading packet byte counts on EP 0x83 IN measures the physical BCD2000 DAC crystal
    // clock directly in USB frames, eliminating ~2-3 Hz crystal drift clicks permanently.
    std::atomic<bool> inSyncRunning{false};
    std::atomic<bool> inClockLocked{false};
    std::atomic<double> inFramesPerMs{44.1};
    std::thread inSyncThread;

    if (useExact44100 && m_audioInPipe != 0) {
        inSyncRunning.store(true, std::memory_order_release);
        inSyncThread = std::thread([&]() {
            DWORD inTaskIdx = 0;
            HANDLE hInMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &inTaskIdx);
            if (hInMmcss) {
                AvSetMmThreadPriority(hInMmcss, AVRT_PRIORITY_CRITICAL);
            } else {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            }

            constexpr size_t kInUrbs = 4;
            constexpr size_t kInPktsPerUrb = 16;
            const ULONG inSlotBytes = static_cast<ULONG>(kInPktsPerUrb * packetSize);
            std::vector<uint8_t> inBuf(kInUrbs * inSlotBytes, 0);
            WINUSB_ISOCH_BUFFER_HANDLE hInIsoch = nullptr;

            WinUsb_ResetPipe(m_hAudioInterface, m_audioInPipe);
            if (!WinUsb_RegisterIsochBuffer(m_hAudioInterface, m_audioInPipe,
                                            inBuf.data(),
                                            static_cast<ULONG>(inBuf.size()),
                                            &hInIsoch)) {
                if (hInMmcss) AvRevertMmThreadCharacteristics(hInMmcss);
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
                    ok = WinUsb_ReadIsochPipeAsap(
                        hInIsoch, s.offset, inSlotBytes, FALSE,
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
            while (inSyncRunning.load(std::memory_order_acquire) &&
                   m_streamingActive.load(std::memory_order_acquire)) {
                InUrb &s = inUrbs[reapIdx];
                if (!s.inFlight) {
                    break;
                }
                if (WaitForSingleObject(s.ov.hEvent, 200) != WAIT_OBJECT_0) {
                    break;
                }
                DWORD dummy = 0;
                BOOL ok = WinUsb_GetOverlappedResult(m_hAudioInterface, &s.ov,
                                                     &dummy, FALSE);
                s.inFlight = false;
                if (!inSyncRunning.load(std::memory_order_acquire) ||
                    !m_streamingActive.load(std::memory_order_acquire)) {
                    break;
                }
                if (ok) {
                    for (size_t p = 0; p < kInPktsPerUrb; ++p) {
                        const ULONG len = s.pkts[p].Length;
                        if (s.pkts[p].Status == 0 && len > 0) {
                            accFrames += (len / 8);
                            accPkts++;
                        }
                    }
                    if (accPkts >= 200) {
                        const double fpm = static_cast<double>(accFrames) /
                                           static_cast<double>(accPkts);
                        inFramesPerMs.store(fpm, std::memory_order_release);
                        m_usbTargetRate.store(fpm * 1000.0, std::memory_order_release);
                        inClockLocked.store(true, std::memory_order_release);
                        if (m_sharedState) {
                            m_sharedState->liveDacRate = fpm * 1000.0;
                            m_sharedState->clockLocked = 1;
                            double diff = (fpm * 1000.0) - m_sampleRate;
                            m_sharedState->clockDriftPpm = (diff / m_sampleRate) * 1.0e6;
                        }
                    }
                } else {
                    continueIn = false;
                }

                if (!SubmitIn(reapIdx)) {
                    break;
                }
                reapIdx = (reapIdx + 1) % kInUrbs;
            }

            WinUsb_AbortPipe(m_hAudioInterface, m_audioInPipe);
            for (size_t i = 0; i < kInUrbs; ++i) {
                if (inUrbs[i].inFlight) {
                    WaitForSingleObject(inUrbs[i].ov.hEvent, 100);
                }
                if (inUrbs[i].ov.hEvent) {
                    CloseHandle(inUrbs[i].ov.hEvent);
                }
            }
            WinUsb_UnregisterIsochBuffer(hInIsoch);
            if (hInMmcss) AvRevertMmThreadCharacteristics(hInMmcss);
        });

        for (int w = 0; w < 25 && !inClockLocked.load(std::memory_order_acquire) &&
                        m_streamingActive.load(std::memory_order_acquire);
             ++w) {
            Sleep(10);
        }
    }

    const double actualUsbRate =
        useExact44100
            ? (inClockLocked.load(std::memory_order_acquire)
                   ? (inFramesPerMs.load(std::memory_order_acquire) * 1000.0)
                   : 44100.0)
            : (static_cast<double>(packetSize / 8) * 1000.0);
    m_exactModeActive.store(useExact44100, std::memory_order_release);
    m_usbTargetRate.store(actualUsbRate, std::memory_order_release);
    m_usbRateReady.store(true, std::memory_order_release);

    if (m_sharedState) {
        m_sharedState->liveDacRate = actualUsbRate;
        m_sharedState->clockLocked = (inClockLocked.load(std::memory_order_acquire) || !useExact44100) ? 1 : 0;
        double diff = actualUsbRate - m_sampleRate;
        m_sharedState->clockDriftPpm = (diff / m_sampleRate) * 1.0e6;
    }

    struct UrbSlot {
        OVERLAPPED ov{};
        ULONG offset = 0;
        ULONG length = 0;
        bool inFlight = false;
    };

    std::vector<UrbSlot> urbs(numUrbs);
    for (size_t i = 0; i < numUrbs; ++i) {
        urbs[i].offset = static_cast<ULONG>(i * slotStride);
        urbs[i].length = slotStride;
        urbs[i].ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    // Lead with 45-frame (360 B) packet via 0.9 initial accumulator for positive FIFO headroom
    double exactFrameAccumulator = 0.9;
    const size_t maxFramesPerPacket = packetSize / 8;

    bool continueStream = false;
    bool useExplicitFrame = false;
    ULONG nextFrameNumber = 0;

    auto SubmitUrbBuffer = [&](UrbSlot &slot) -> bool {
        BOOL ok = FALSE;
        DWORD err = ERROR_SUCCESS;

        if (isIsoch && hIsochBuffer) {
            if (!useExplicitFrame) {
                ok = WinUsb_WriteIsochPipeAsap(
                    hIsochBuffer, slot.offset, slot.length,
                    continueStream ? TRUE : FALSE, &slot.ov);
                err = ok ? ERROR_SUCCESS : GetLastError();

                if (!ok && err != ERROR_IO_PENDING && continueStream) {
                    ResetEvent(slot.ov.hEvent);
                    ok = WinUsb_WriteIsochPipeAsap(
                        hIsochBuffer, slot.offset, slot.length, FALSE,
                        &slot.ov);
                    err = ok ? ERROR_SUCCESS : GetLastError();
                }

                if (!ok && err != ERROR_IO_PENDING) {
                    LARGE_INTEGER ts{};
                    ULONG curFrame = 0;
                    if (WinUsb_GetCurrentFrameNumber(m_hAudioInterface,
                                                     &curFrame, &ts)) {
                        useExplicitFrame = true;
                        nextFrameNumber = curFrame + 4;
                        ResetEvent(slot.ov.hEvent);
                        ok = WinUsb_WriteIsochPipe(
                            hIsochBuffer, slot.offset, slot.length,
                            &nextFrameNumber, &slot.ov);
                        err = ok ? ERROR_SUCCESS : GetLastError();
                    }
                } else {
                    continueStream = true;
                }
            } else {
                ok = WinUsb_WriteIsochPipe(hIsochBuffer, slot.offset,
                                           slot.length, &nextFrameNumber,
                                           &slot.ov);
                err = ok ? ERROR_SUCCESS : GetLastError();
            }
        } else {
            ok = WinUsb_WritePipe(m_hAudioInterface, m_audioOutPipe,
                                  dmaBuffer.data() + slot.offset,
                                  slot.length, nullptr, &slot.ov);
            err = ok ? ERROR_SUCCESS : GetLastError();
        }

        if (ok || err == ERROR_IO_PENDING) {
            slot.inFlight = true;
            return true;
        }
        slot.inFlight = false;
        return false;
    };

    auto HarvestAndFillUrb = [&](size_t idx) -> bool {
        UrbSlot &slot = urbs[idx];
        ResetEvent(slot.ov.hEvent);

        if (useExact44100) {
            const double fpm =
                inClockLocked.load(std::memory_order_acquire)
                    ? inFramesPerMs.load(std::memory_order_relaxed)
                    : 44.1;
            exactFrameAccumulator += fpm;
            size_t pktFrames = static_cast<size_t>(std::floor(exactFrameAccumulator));
            exactFrameAccumulator -= static_cast<double>(pktFrames);
            pktFrames = std::clamp<size_t>(pktFrames, 1, maxFramesPerPacket);
            slot.length = static_cast<ULONG>(pktFrames * 8);
        } else {
            slot.length = slotStride;
        }

        int16_t *dst = reinterpret_cast<int16_t *>(dmaBuffer.data() + slot.offset);
        const size_t framesInSlot = slot.length / 8;
        const size_t mask = kRingCapacityFrames - 1;
        const uint64_t writeIdx = m_ringWriteFrames.load(std::memory_order_acquire);
        const uint64_t readIdx = m_ringReadFrames.load(std::memory_order_relaxed);
        const size_t available = (writeIdx >= readIdx) ? static_cast<size_t>(writeIdx - readIdx) : 0;

        if (available >= framesInSlot) {
            for (size_t f = 0; f < framesInSlot; ++f) {
                const size_t ringSlot = static_cast<size_t>((readIdx + f) & mask) * 4;
                dst[f * 4 + 0] = m_outRingBuffer[ringSlot + 0];
                dst[f * 4 + 1] = m_outRingBuffer[ringSlot + 1];
                dst[f * 4 + 2] = m_outRingBuffer[ringSlot + 2];
                dst[f * 4 + 3] = m_outRingBuffer[ringSlot + 3];
            }
            m_ringReadFrames.store(readIdx + framesInSlot, std::memory_order_release);
        } else {
            // Buffer starvation protection: copy available frames, fill rest with silence
            m_xrunCount.fetch_add(1, std::memory_order_relaxed);
            if (m_sharedState) {
                m_sharedState->xrunCount = m_xrunCount.load(std::memory_order_relaxed);
            }
            for (size_t f = 0; f < available; ++f) {
                const size_t ringSlot = static_cast<size_t>((readIdx + f) & mask) * 4;
                dst[f * 4 + 0] = m_outRingBuffer[ringSlot + 0];
                dst[f * 4 + 1] = m_outRingBuffer[ringSlot + 1];
                dst[f * 4 + 2] = m_outRingBuffer[ringSlot + 2];
                dst[f * 4 + 3] = m_outRingBuffer[ringSlot + 3];
            }
            std::memset(dst + available * 4, 0, (framesInSlot - available) * 8);
            m_ringReadFrames.store(readIdx + available, std::memory_order_release);
        }

        if (m_sharedState) {
            m_sharedState->ringFillPercent =
                static_cast<uint32_t>((available * 100) / kRingCapacityFrames);
        }

        // Signal ASIO host callback thread to replenish ring buffer
        if (m_hAsioTriggerEvent) {
            SetEvent(m_hAsioTriggerEvent);
        }

        return SubmitUrbBuffer(slot);
    };

    // Wait up to 1000 ms for ASIO host thread to complete prefill
    for (int w = 0; w < 100 && !m_prefillReady.load(std::memory_order_acquire); ++w) {
        Sleep(10);
    }

    // Prime the USB isochronous pipeline with all numUrbs transfers
    bool pipelineActive = true;
    for (size_t i = 0; i < numUrbs; ++i) {
        if (!HarvestAndFillUrb(i)) {
            bcd2000::TraceLog("ASIO", "UsbStreamingThreadMain: Prime failed at urb=%zu err=%lu",
                              i, GetLastError());
            pipelineActive = false;
            break;
        }
    }

    bcd2000::TraceLog("ASIO", "UsbStreamingThreadMain: pipelineActive=%d useExact44100=%d inLocked=%d",
                      pipelineActive ? 1 : 0, useExact44100 ? 1 : 0,
                      inClockLocked.load() ? 1 : 0);

    size_t nextReap = 0;
    uint64_t urbsCompleted = 0;
    while (pipelineActive && m_streamingActive.load(std::memory_order_acquire)) {
        UrbSlot &slot = urbs[nextReap];
        if (!slot.inFlight) {
            bcd2000::TraceLog("ASIO", "UsbStreamingThreadMain: slot not inFlight at urb=%zu", nextReap);
            break;
        }
        DWORD waitRes = WaitForSingleObject(slot.ov.hEvent, 1000);
        if (waitRes != WAIT_OBJECT_0) {
            bcd2000::TraceLog("ASIO", "UsbStreamingThreadMain: Wait timeout waitRes=%lu err=%lu",
                              waitRes, GetLastError());
            break;
        }
        DWORD bytesTransferred = 0;
        BOOL gOk = WinUsb_GetOverlappedResult(m_hAudioInterface, &slot.ov, &bytesTransferred, FALSE);
        if (!gOk) {
            bcd2000::TraceLog("ASIO", "UsbStreamingThreadMain: GetOverlappedResult err=%lu", GetLastError());
        }
        slot.inFlight = false;
        ++urbsCompleted;

        if (!m_streamingActive.load(std::memory_order_acquire)) {
            break;
        }

        if (!HarvestAndFillUrb(nextReap)) {
            bcd2000::TraceLog("ASIO", "UsbStreamingThreadMain: HarvestAndFillUrb failed err=%lu", GetLastError());
            break;
        }
        nextReap = (nextReap + 1) % numUrbs;
    }
    bcd2000::TraceLog("ASIO", "UsbStreamingThreadMain: loop exit urbsCompleted=%llu active=%d",
                      static_cast<unsigned long long>(urbsCompleted),
                      m_streamingActive.load() ? 1 : 0);

    // Stop EP 0x83 IN implicit clock tracker cleanly before tearing down pipes
    if (inSyncThread.joinable()) {
        inSyncRunning.store(false, std::memory_order_release);
        WinUsb_AbortPipe(m_hAudioInterface, m_audioInPipe);
        inSyncThread.join();
    }

    // Cleanly drain in-flight URBs
    for (size_t i = 0; i < numUrbs; ++i) {
        if (urbs[i].inFlight) {
            WaitForSingleObject(urbs[i].ov.hEvent, 150);
            DWORD dummy = 0;
            WinUsb_GetOverlappedResult(m_hAudioInterface, &urbs[i].ov, &dummy, FALSE);
            urbs[i].inFlight = false;
        }
        if (urbs[i].ov.hEvent) {
            CloseHandle(urbs[i].ov.hEvent);
            urbs[i].ov.hEvent = nullptr;
        }
    }

    if (isIsoch && hIsochBuffer) {
        WinUsb_UnregisterIsochBuffer(hIsochBuffer);
    }

    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }
}
