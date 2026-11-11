/**
 * @file bcd2000_midi_driver.cpp
 * @brief 64-bit User-Mode WinMM MME MIDI Driver (IN & OUT) for Behringer BCD2000.
 *
 * Implements Windows Multimedia Extensions entry points:
 *   - DriverProc (installable MME driver lifecycle)
 *   - midMessage (MIDI IN port: faders, knobs, jog wheels, buttons via EP 0x81 IN)
 *   - modMessage (MIDI OUT port: LEDs & SysEx via EP 0x01 OUT)
 *   - DllRegisterServer / DllUnregisterServer (HKLM Drivers32 midi2..midi9 slot)
 *
 * Dual-mode WinUSB sharing with bcd2000_asio.dll:
 *   - Mode A (ASIO Pipe Client): Connects to \\.\pipe\BCD2000_ASIO_MIDI_V1 when
 *     bcd2000_asio.dll already owns the exclusive WinUSB handle.
 *   - Mode B (Standalone WinUSB): Opens WinUSB directly when ASIO is idle, and
 *     serves \\.\pipe\BCD2000_MIDI_CTRL_V1 so bcd2000_asio.dll can request
 *     immediate USB handoff ('R' -> 'K') and promote this driver to Mode A
 *     seamlessly without closing open WinMM MIDI handles.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#include <winusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/bcd2000_usb_protocol.h"

namespace {

HMODULE g_hModule = nullptr;

constexpr wchar_t kMidiDeviceNameW[] = L"Behringer BCD2000 MIDI";
constexpr char kMidiDeviceNameA[] = "Behringer BCD2000 MIDI";
constexpr wchar_t kMidiDllFileNameW[] = L"bcd2000_midi.dll";
constexpr wchar_t kDrivers32KeyPath[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32";
constexpr wchar_t kDriversDescKeyPath[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\drivers.desc";

// =============================================================================
// WinMM Client Handle Structures
// =============================================================================

struct MidiInClient {
    HMIDIIN hMidi = nullptr;
    DWORD_PTR dwCallback = 0;
    DWORD_PTR dwInstance = 0;
    DWORD dwFlags = 0;
    std::atomic<bool> started{false};
    DWORD startTickMs = 0;

    std::mutex sysexMutex;
    std::vector<LPMIDIHDR> pendingBuffers;
};

struct MidiOutClient {
    HMIDIOUT hMidi = nullptr;
    DWORD_PTR dwCallback = 0;
    DWORD_PTR dwInstance = 0;
    DWORD dwFlags = 0;
};

inline void InvokeDriverCallback(DWORD_PTR dwCallback,
                                 DWORD dwFlags,
                                 HDRVR hDev,
                                 UINT uMsg,
                                 DWORD_PTR dwInstance,
                                 DWORD_PTR dwParam1,
                                 DWORD_PTR dwParam2) {
    if (dwCallback == 0 && HIWORD(dwFlags) == 0) {
        return;
    }
    DriverCallback(dwCallback,
                   static_cast<DWORD>(HIWORD(dwFlags)),
                   hDev,
                   uMsg,
                   dwInstance,
                   dwParam1,
                   dwParam2);
}

// =============================================================================
// Shared Dual-Mode Transport (ASIO Pipe Client <-> Standalone WinUSB)
// =============================================================================

class Bcd2000MidiTransport {
public:
    Bcd2000MidiTransport() = default;
    ~Bcd2000MidiTransport() {
        ForceShutdown();
    }

    bool AcquireClient() {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        if (m_openClients == 0) {
            StartTransportLocked();
        }
        ++m_openClients;
        return true;
    }

    void ReleaseClient() {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        if (m_openClients > 0) {
            --m_openClients;
        }
        if (m_openClients == 0) {
            StopTransportLocked();
        }
    }

    void RegisterInClient(const std::shared_ptr<MidiInClient> &client) {
        std::lock_guard<std::mutex> lock(m_inClientsMutex);
        m_inClients.push_back(client);
    }

    void UnregisterInClient(const std::shared_ptr<MidiInClient> &client) {
        std::lock_guard<std::mutex> lock(m_inClientsMutex);
        m_inClients.erase(
            std::remove(m_inClients.begin(), m_inClients.end(), client),
            m_inClients.end());
    }

    void DispatchShortMidiIn(DWORD packedMsg) {
        std::vector<std::shared_ptr<MidiInClient>> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_inClientsMutex);
            snapshot = m_inClients;
        }
        const DWORD nowMs = GetTickCount();
        for (const auto &client : snapshot) {
            if (!client || !client->started.load(std::memory_order_acquire)) {
                continue;
            }
            const DWORD elapsedMs = nowMs - client->startTickMs;
            InvokeDriverCallback(
                client->dwCallback,
                client->dwFlags,
                reinterpret_cast<HDRVR>(client->hMidi),
                MIM_DATA,
                client->dwInstance,
                static_cast<DWORD_PTR>(packedMsg),
                static_cast<DWORD_PTR>(elapsedMs));
        }
    }

    bool SendMidiBytes(const uint8_t *data, size_t len) {
        if (!data || len == 0) {
            return true;
        }

        // Brief wait if a WinUSB -> ASIO Pipe handoff is mid-flight
        for (int waitIdx = 0; waitIdx < 20; ++waitIdx) {
            if (!m_handoffInProgress.load(std::memory_order_acquire)) {
                break;
            }
            Sleep(5);
        }

        std::lock_guard<std::mutex> writeLock(m_writeMutex);

        if (m_hAsioPipe != INVALID_HANDLE_VALUE) {
            // Frame format for bcd2000_asio.dll: [payloadLen, payload...] <= 62 bytes
            size_t offset = 0;
            while (offset < len) {
                const size_t chunk = std::min<size_t>(61, len - offset);
                uint8_t msg[64]{};
                msg[0] = static_cast<uint8_t>(chunk);
                std::memcpy(&msg[1], data + offset, chunk);

                OVERLAPPED wOv{};
                wOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!wOv.hEvent) {
                    return false;
                }
                DWORD written = 0;
                const DWORD totalBytes = static_cast<DWORD>(1 + chunk);
                BOOL ok = WriteFile(m_hAsioPipe, msg, totalBytes, &written, &wOv);
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(wOv.hEvent, 100) == WAIT_OBJECT_0) {
                        ok = GetOverlappedResult(m_hAsioPipe, &wOv, &written, FALSE);
                    } else {
                        CancelIoEx(m_hAsioPipe, &wOv);
                        ok = FALSE;
                    }
                }
                CloseHandle(wOv.hEvent);
                if (!ok) {
                    return false;
                }
                offset += chunk;
            }
            return true;
        }

        if (m_hMidiInterface && m_midiOutPipe != 0) {
            size_t offset = 0;
            while (offset < len) {
                const size_t chunk = std::min<size_t>(61, len - offset);
                if (!bcd2000::SendBcdMidiOverlapped(m_hMidiInterface, m_midiOutPipe,
                                                    data + offset, chunk)) {
                    return false;
                }
                offset += chunk;
            }
            return true;
        }

        return false;
    }

private:
    void StartTransportLocked() {
        m_stopRequested.store(false, std::memory_order_release);
        m_handoffRequested.store(false, std::memory_order_release);
        m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_hHandoffEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        // Probe once synchronously on initial open so immediate MODM_DATA works right away
        if (!TryConnectAsioPipe()) {
            TryOpenUsbStandalone();
        }

        m_ctrlThread = std::thread(&Bcd2000MidiTransport::CtrlPipeThreadMain, this);
        m_transportThread = std::thread(&Bcd2000MidiTransport::TransportThreadMain, this);
    }

    void StopTransportLocked() {
        m_stopRequested.store(true, std::memory_order_release);
        if (m_hStopEvent) {
            SetEvent(m_hStopEvent);
        }
        {
            std::lock_guard<std::mutex> writeLock(m_writeMutex);
            if (m_hAsioPipe != INVALID_HANDLE_VALUE) {
                CancelIoEx(m_hAsioPipe, nullptr);
            }
            if (m_hMidiInterface && m_midiInPipe != 0) {
                WinUsb_AbortPipe(m_hMidiInterface, m_midiInPipe);
            }
        }

        if (m_ctrlThread.joinable()) {
            m_ctrlThread.join();
        }
        if (m_transportThread.joinable()) {
            m_transportThread.join();
        }

        CloseAsioPipe();
        CloseUsbStandalone();

        if (m_hStopEvent) {
            CloseHandle(m_hStopEvent);
            m_hStopEvent = nullptr;
        }
        if (m_hHandoffEvent) {
            CloseHandle(m_hHandoffEvent);
            m_hHandoffEvent = nullptr;
        }
    }

    void ForceShutdown() {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        m_openClients = 0;
        if (m_transportThread.joinable() || m_ctrlThread.joinable() ||
            m_hAsioPipe != INVALID_HANDLE_VALUE || m_hDevice != INVALID_HANDLE_VALUE) {
            StopTransportLocked();
        }
    }

    bool TryConnectAsioPipe() {
        HANDLE hPipe = CreateFileW(
            bcd2000::kAsioMidiPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD readMode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(hPipe, &readMode, nullptr, nullptr);

        std::lock_guard<std::mutex> writeLock(m_writeMutex);
        m_hAsioPipe = hPipe;
        return true;
    }

    void CloseAsioPipe() {
        std::lock_guard<std::mutex> writeLock(m_writeMutex);
        if (m_hAsioPipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(m_hAsioPipe, nullptr);
            CloseHandle(m_hAsioPipe);
            m_hAsioPipe = INVALID_HANDLE_VALUE;
        }
    }

    bool TryOpenUsbStandalone() {
        const std::wstring devPath = bcd2000::FindBcd2000DevicePath();
        if (devPath.empty()) {
            return false;
        }

        HANDLE hDev = CreateFileW(
            devPath.c_str(),
            GENERIC_WRITE | GENERIC_READ,
            FILE_SHARE_WRITE | FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (hDev == INVALID_HANDLE_VALUE) {
            return false;
        }

        WINUSB_INTERFACE_HANDLE hIf0 = nullptr;
        if (!WinUsb_Initialize(hDev, &hIf0)) {
            CloseHandle(hDev);
            return false;
        }

        std::vector<WINUSB_INTERFACE_HANDLE> allIfs;
        allIfs.push_back(hIf0);
        for (UCHAR assoc = 0; assoc < 4; ++assoc) {
            WINUSB_INTERFACE_HANDLE hAssoc = nullptr;
            if (WinUsb_GetAssociatedInterface(hIf0, assoc, &hAssoc)) {
                allIfs.push_back(hAssoc);
            }
        }

        WINUSB_INTERFACE_HANDLE hMidiIf = nullptr;
        UCHAR midiIn = 0x81;
        UCHAR midiOut = 0x01;
        bool foundIn = false;
        bool foundOut = false;

        for (WINUSB_INTERFACE_HANDLE hIf : allIfs) {
            for (UCHAR alt = 0; alt < 4; ++alt) {
                USB_INTERFACE_DESCRIPTOR desc{};
                if (!WinUsb_QueryInterfaceSettings(hIf, alt, &desc)) {
                    break;
                }
                for (UCHAR p = 0; p < desc.bNumEndpoints; ++p) {
                    WINUSB_PIPE_INFORMATION pipeInfo{};
                    if (!WinUsb_QueryPipe(hIf, alt, p, &pipeInfo)) {
                        continue;
                    }
                    if (pipeInfo.PipeId == 0x01) {
                        hMidiIf = hIf;
                        midiOut = 0x01;
                        foundOut = true;
                    } else if (pipeInfo.PipeId == 0x81) {
                        hMidiIf = hIf;
                        midiIn = 0x81;
                        foundIn = true;
                    }
                }
            }
        }

        if (!hMidiIf || (!foundIn && !foundOut)) {
            hMidiIf = hIf0;
        }

        ULONG timeoutMs = 400;
        WinUsb_SetPipePolicy(hMidiIf, midiOut, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
        UCHAR autoClear = 1;
        WinUsb_SetPipePolicy(hMidiIf, midiOut, AUTO_CLEAR_STALL, sizeof(autoClear), &autoClear);
        WinUsb_SetPipePolicy(hMidiIf, midiIn, AUTO_CLEAR_STALL, sizeof(autoClear), &autoClear);

        // Send 52-byte 8051 init handshake to EP 0x01
        ULONG written = 0;
        WinUsb_WritePipe(hMidiIf, midiOut,
                         const_cast<PUCHAR>(bcd2000::kInitSequence),
                         sizeof(bcd2000::kInitSequence), &written, nullptr);

        // Drain stale EP 0x81 IN buffer briefly
        ULONG drainTimeout = 40;
        WinUsb_SetPipePolicy(hMidiIf, midiIn, PIPE_TRANSFER_TIMEOUT, sizeof(drainTimeout), &drainTimeout);
        uint8_t drainBuf[64]{};
        for (int i = 0; i < 4; ++i) {
            ULONG bytesRead = 0;
            if (!WinUsb_ReadPipe(hMidiIf, midiIn, drainBuf, sizeof(drainBuf), &bytesRead, nullptr)) {
                break;
            }
        }
        ULONG infTimeout = 0;
        WinUsb_SetPipePolicy(hMidiIf, midiIn, PIPE_TRANSFER_TIMEOUT, sizeof(infTimeout), &infTimeout);
        UCHAR rawIo = 1;
        WinUsb_SetPipePolicy(hMidiIf, midiIn, RAW_IO, sizeof(rawIo), &rawIo);

        {
            std::lock_guard<std::mutex> writeLock(m_writeMutex);
            m_hDevice = hDev;
            m_allInterfaces = std::move(allIfs);
            m_hMidiInterface = hMidiIf;
            m_midiInPipe = midiIn;
            m_midiOutPipe = midiOut;
        }
        return true;
    }

    void CloseUsbStandalone() {
        std::lock_guard<std::mutex> writeLock(m_writeMutex);
        for (size_t i = m_allInterfaces.size(); i > 0; --i) {
            if (m_allInterfaces[i - 1]) {
                WinUsb_Free(m_allInterfaces[i - 1]);
            }
        }
        m_allInterfaces.clear();
        m_hMidiInterface = nullptr;

        if (m_hDevice != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hDevice);
            m_hDevice = INVALID_HANDLE_VALUE;
        }
    }

    void CtrlPipeThreadMain() {
        HANDLE hConnEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!hConnEvent) {
            return;
        }

        while (!m_stopRequested.load(std::memory_order_acquire)) {
            HANDLE hPipe = CreateNamedPipeW(
                bcd2000::kMidiCtrlPipeName,
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                4, 64, 64, 0, nullptr);
            if (hPipe == INVALID_HANDLE_VALUE) {
                if (WaitForSingleObject(m_hStopEvent, 100) == WAIT_OBJECT_0) {
                    break;
                }
                continue;
            }

            OVERLAPPED ov{};
            ov.hEvent = hConnEvent;
            ResetEvent(hConnEvent);
            BOOL connected = ConnectNamedPipe(hPipe, &ov);
            DWORD err = connected ? ERROR_SUCCESS : GetLastError();
            if (!connected && err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {m_hStopEvent, hConnEvent};
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0 || m_stopRequested.load(std::memory_order_acquire)) {
                    CancelIoEx(hPipe, &ov);
                    CloseHandle(hPipe);
                    break;
                }
                DWORD dummy = 0;
                connected = GetOverlappedResult(hPipe, &ov, &dummy, FALSE);
            } else if (!connected && err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }

            if (connected) {
                char cmd = 0;
                DWORD bytesRead = 0;
                OVERLAPPED rOv{};
                rOv.hEvent = hConnEvent;
                ResetEvent(hConnEvent);
                BOOL rOk = ReadFile(hPipe, &cmd, 1, &bytesRead, &rOv);
                if (!rOk && GetLastError() == ERROR_IO_PENDING) {
                    HANDLE waits[2] = {m_hStopEvent, hConnEvent};
                    if (WaitForMultipleObjects(2, waits, FALSE, 200) == WAIT_OBJECT_0 + 1) {
                        rOk = GetOverlappedResult(hPipe, &rOv, &bytesRead, FALSE);
                    } else {
                        CancelIoEx(hPipe, &rOv);
                    }
                }

                if (rOk && bytesRead >= 1 && cmd == 'R') {
                    // Yield standalone WinUSB handle to bcd2000_asio.dll
                    m_handoffInProgress.store(true, std::memory_order_release);
                    m_handoffRequested.store(true, std::memory_order_release);
                    if (m_hHandoffEvent) {
                        SetEvent(m_hHandoffEvent);
                    }
                    if (m_hMidiInterface && m_midiInPipe != 0) {
                        WinUsb_AbortPipe(m_hMidiInterface, m_midiInPipe);
                    }

                    // Wait until TransportThreadMain has freed WinUSB
                    for (int i = 0; i < 100; ++i) {
                        if (!m_usbActive.load(std::memory_order_acquire)) {
                            break;
                        }
                        Sleep(5);
                    }

                    const char ack = 'K';
                    DWORD written = 0;
                    OVERLAPPED wOv{};
                    wOv.hEvent = hConnEvent;
                    ResetEvent(hConnEvent);
                    if (!WriteFile(hPipe, &ack, 1, &written, &wOv) &&
                        GetLastError() == ERROR_IO_PENDING) {
                        WaitForSingleObject(hConnEvent, 100);
                    }
                    FlushFileBuffers(hPipe);
                }
                DisconnectNamedPipe(hPipe);
            }
            CloseHandle(hPipe);
        }

        CloseHandle(hConnEvent);
    }

    void RunAsioPipeReadLoop(HANDLE hEvent) {
        DWORD packedMsg = 0;
        while (!m_stopRequested.load(std::memory_order_acquire) &&
               m_hAsioPipe != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov{};
            ov.hEvent = hEvent;
            ResetEvent(hEvent);
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(m_hAsioPipe, &packedMsg, sizeof(packedMsg), &bytesRead, &ov);
            DWORD err = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok && err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {m_hStopEvent, hEvent};
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0 || m_stopRequested.load(std::memory_order_acquire)) {
                    CancelIoEx(m_hAsioPipe, &ov);
                    break;
                }
                ok = GetOverlappedResult(m_hAsioPipe, &ov, &bytesRead, FALSE);
            }
            if (!ok) {
                break; // Pipe closed (ASIO stopped); fall back cleanly
            }
            if (bytesRead == sizeof(DWORD)) {
                DispatchShortMidiIn(packedMsg);
            }
        }
        CloseAsioPipe();
    }

    void RunUsbStandaloneReadLoop(HANDLE hEvent) {
        m_usbActive.store(true, std::memory_order_release);
        bcd2000::MidiRunningStatusParser parser;
        uint8_t buf[64]{};

        while (!m_stopRequested.load(std::memory_order_acquire) &&
               !m_handoffRequested.load(std::memory_order_acquire) &&
               m_hMidiInterface != nullptr) {
            OVERLAPPED ov{};
            ov.hEvent = hEvent;
            ResetEvent(hEvent);
            ULONG bytesRead = 0;
            BOOL ok = WinUsb_ReadPipe(m_hMidiInterface, m_midiInPipe, buf, sizeof(buf),
                                      &bytesRead, &ov);
            DWORD err = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok && err == ERROR_IO_PENDING) {
                HANDLE waits[3] = {m_hStopEvent, m_hHandoffEvent, hEvent};
                DWORD w = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0 || w == WAIT_OBJECT_0 + 1 ||
                    m_stopRequested.load(std::memory_order_acquire) ||
                    m_handoffRequested.load(std::memory_order_acquire)) {
                    WinUsb_AbortPipe(m_hMidiInterface, m_midiInPipe);
                    WaitForSingleObject(hEvent, 100);
                    break;
                }
                ok = WinUsb_GetOverlappedResult(m_hMidiInterface, &ov, &bytesRead, FALSE);
            }
            if (ok && bytesRead > 0) {
                parser.FeedUsbPacket(buf, bytesRead, [this](DWORD packedMsg) {
                    DispatchShortMidiIn(packedMsg);
                });
            } else if (!ok) {
                if (m_stopRequested.load(std::memory_order_acquire) ||
                    m_handoffRequested.load(std::memory_order_acquire)) {
                    break;
                }
                DWORD lastErr = GetLastError();
                if (lastErr == ERROR_DEVICE_NOT_CONNECTED ||
                    lastErr == ERROR_BAD_COMMAND ||
                    lastErr == ERROR_GEN_FAILURE) {
                    break;
                }
                Sleep(10);
            }
        }

        CloseUsbStandalone();
        m_usbActive.store(false, std::memory_order_release);
    }

    void TransportThreadMain() {
        HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!hEvent) {
            return;
        }

        while (!m_stopRequested.load(std::memory_order_acquire)) {
            if (m_hAsioPipe != INVALID_HANDLE_VALUE) {
                m_handoffInProgress.store(false, std::memory_order_release);
                RunAsioPipeReadLoop(hEvent);
                if (m_stopRequested.load(std::memory_order_acquire)) {
                    break;
                }
                // Give bcd2000_asio.dll a moment to release WinUSB cleanly
                WaitForSingleObject(m_hStopEvent, 120);
                continue;
            }

            if (m_hMidiInterface != nullptr) {
                RunUsbStandaloneReadLoop(hEvent);
                if (m_stopRequested.load(std::memory_order_acquire)) {
                    break;
                }
                if (m_handoffRequested.exchange(false, std::memory_order_acq_rel)) {
                    if (m_hHandoffEvent) {
                        ResetEvent(m_hHandoffEvent);
                    }
                    // Wait for bcd2000_asio.dll to bring up kAsioMidiPipeName
                    for (int attempt = 0; attempt < 40; ++attempt) {
                        if (m_stopRequested.load(std::memory_order_acquire)) {
                            break;
                        }
                        if (TryConnectAsioPipe()) {
                            break;
                        }
                        WaitForSingleObject(m_hStopEvent, 25);
                    }
                    m_handoffInProgress.store(false, std::memory_order_release);
                    continue;
                }
                WaitForSingleObject(m_hStopEvent, 150);
                continue;
            }

            // Neither connected yet: prefer ASIO Pipe first, then standalone WinUSB
            if (TryConnectAsioPipe()) {
                m_handoffInProgress.store(false, std::memory_order_release);
                continue;
            }
            if (TryOpenUsbStandalone()) {
                m_handoffInProgress.store(false, std::memory_order_release);
                continue;
            }
            m_handoffInProgress.store(false, std::memory_order_release);
            if (WaitForSingleObject(m_hStopEvent, 250) == WAIT_OBJECT_0) {
                break;
            }
        }

        CloseHandle(hEvent);
    }

    std::mutex m_lifecycleMutex;
    size_t m_openClients = 0;

    std::mutex m_inClientsMutex;
    std::vector<std::shared_ptr<MidiInClient>> m_inClients;

    std::mutex m_writeMutex;
    HANDLE m_hAsioPipe = INVALID_HANDLE_VALUE;
    HANDLE m_hDevice = INVALID_HANDLE_VALUE;
    std::vector<WINUSB_INTERFACE_HANDLE> m_allInterfaces;
    WINUSB_INTERFACE_HANDLE m_hMidiInterface = nullptr;
    UCHAR m_midiInPipe = 0x81;
    UCHAR m_midiOutPipe = 0x01;

    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_handoffRequested{false};
    std::atomic<bool> m_handoffInProgress{false};
    std::atomic<bool> m_usbActive{false};
    HANDLE m_hStopEvent = nullptr;
    HANDLE m_hHandoffEvent = nullptr;
    std::thread m_transportThread;
    std::thread m_ctrlThread;
};

Bcd2000MidiTransport &GetTransport() {
    static Bcd2000MidiTransport instance;
    return instance;
}

// Track active IN shared_ptrs so MIDM_CLOSE erases deterministically
std::mutex g_inRegistryMutex;
std::vector<std::shared_ptr<MidiInClient>> g_activeInClients;

std::shared_ptr<MidiInClient> LookupInClient(DWORD_PTR dwUser) {
    auto *raw = reinterpret_cast<MidiInClient *>(dwUser);
    if (!raw) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_inRegistryMutex);
    for (const auto &sp : g_activeInClients) {
        if (sp.get() == raw) {
            return sp;
        }
    }
    return nullptr;
}

// =============================================================================
// Registry Install/Uninstall Helpers (Drivers32 midi2..midi9)
// =============================================================================

bool EndsWithInsensitive(const wchar_t *str, const wchar_t *suffix) {
    if (!str || !suffix) {
        return false;
    }
    const size_t lenStr = std::wcslen(str);
    const size_t lenSuffix = std::wcslen(suffix);
    if (lenStr < lenSuffix) {
        return false;
    }
    return _wcsicmp(str + (lenStr - lenSuffix), suffix) == 0;
}

bool RegisterDrivers32Slot(const wchar_t *dllPath) {
    HKEY hKey = nullptr;
    LONG status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        kDrivers32KeyPath,
        0,
        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
        &hKey);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    const auto makeSlotName = [](int idx, wchar_t outName[16]) {
        if (idx == 0) {
            std::wcscpy(outName, L"midi");
        } else {
            std::swprintf(outName, 16, L"midi%d", idx);
        }
    };

    wchar_t chosenSlot[16]{};
    wchar_t firstEmptySlot[16]{};

    for (int idx = 0; idx <= 9; ++idx) {
        wchar_t slotName[16]{};
        makeSlotName(idx, slotName);

        wchar_t existingVal[MAX_PATH * 2]{};
        DWORD type = 0;
        DWORD valSize = sizeof(existingVal);
        LONG q = RegQueryValueExW(
            hKey, slotName, nullptr, &type,
            reinterpret_cast<LPBYTE>(existingVal), &valSize);

        if (q == ERROR_FILE_NOT_FOUND) {
            if (firstEmptySlot[0] == L'\0') {
                std::wcscpy(firstEmptySlot, slotName);
            }
        } else if (q == ERROR_SUCCESS && type == REG_SZ) {
            if (EndsWithInsensitive(existingVal, kMidiDllFileNameW)) {
                std::wcscpy(chosenSlot, slotName);
                break;
            }
        }
    }

    if (chosenSlot[0] == L'\0') {
        if (firstEmptySlot[0] == L'\0') {
            RegCloseKey(hKey);
            return false;
        }
        std::wcscpy(chosenSlot, firstEmptySlot);
    }

    const wchar_t *regValueStr =
        EndsWithInsensitive(dllPath, L"\\System32\\bcd2000_midi.dll")
            ? kMidiDllFileNameW
            : dllPath;
    const DWORD byteLen = static_cast<DWORD>((std::wcslen(regValueStr) + 1) * sizeof(wchar_t));
    status = RegSetValueExW(
        hKey, chosenSlot, 0, REG_SZ,
        reinterpret_cast<const BYTE *>(regValueStr), byteLen);
    RegCloseKey(hKey);

    if (status != ERROR_SUCCESS) {
        return false;
    }

    // Also populate drivers.desc for informational display
    HKEY hDescKey = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kDriversDescKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
                        nullptr, &hDescKey, nullptr) == ERROR_SUCCESS) {
        const DWORD descLen = static_cast<DWORD>((std::wcslen(kMidiDeviceNameW) + 1) * sizeof(wchar_t));
        RegSetValueExW(hDescKey, regValueStr, 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(kMidiDeviceNameW), descLen);
        RegCloseKey(hDescKey);
    }

    return true;
}

bool UnregisterDrivers32Slot() {
    HKEY hKey = nullptr;
    LONG status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        kDrivers32KeyPath,
        0,
        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
        &hKey);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    bool allOk = true;
    for (int idx = 0; idx <= 9; ++idx) {
        wchar_t slotName[16]{};
        if (idx == 0) {
            std::wcscpy(slotName, L"midi");
        } else {
            std::swprintf(slotName, 16, L"midi%d", idx);
        }

        wchar_t existingVal[MAX_PATH * 2]{};
        DWORD type = 0;
        DWORD valSize = sizeof(existingVal);
        LONG q = RegQueryValueExW(
            hKey, slotName, nullptr, &type,
            reinterpret_cast<LPBYTE>(existingVal), &valSize);
        if (q == ERROR_SUCCESS && type == REG_SZ &&
            EndsWithInsensitive(existingVal, kMidiDllFileNameW)) {
            if (RegDeleteValueW(hKey, slotName) != ERROR_SUCCESS) {
                allOk = false;
            }
            HKEY hDescKey = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDriversDescKeyPath, 0,
                              KEY_WRITE | KEY_WOW64_64KEY, &hDescKey) == ERROR_SUCCESS) {
                RegDeleteValueW(hDescKey, existingVal);
                RegCloseKey(hDescKey);
            }
        }
    }

    RegCloseKey(hKey);
    return allOk;
}

} // namespace

// =============================================================================
// DLL Entry Point & Self-Registration
// =============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllRegisterServer(void) {
    wchar_t dllPath[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hModule, dllPath, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return RegisterDrivers32Slot(dllPath) ? S_OK : E_ACCESSDENIED;
}

extern "C" HRESULT __stdcall DllUnregisterServer(void) {
    return UnregisterDrivers32Slot() ? S_OK : E_ACCESSDENIED;
}

// =============================================================================
// WinMM Installable Driver Entry Point
// =============================================================================

extern "C" LRESULT CALLBACK DriverProc(DWORD_PTR dwDriverId,
                                       HDRVR hdrvr,
                                       UINT uMsg,
                                       LPARAM lParam1,
                                       LPARAM lParam2) {
    switch (uMsg) {
    case DRV_LOAD:
    case DRV_FREE:
    case DRV_OPEN:
    case DRV_CLOSE:
    case DRV_ENABLE:
    case DRV_DISABLE:
        return 1L;
    case DRV_INSTALL:
    case DRV_REMOVE:
        return DRVCNF_OK;
    case DRV_QUERYCONFIGURE:
        return 0L;
    case DRV_CONFIGURE:
        return DRVCNF_CANCEL;
    default:
        return DefDriverProc(dwDriverId, hdrvr, uMsg, lParam1, lParam2);
    }
}

// =============================================================================
// WinMM MIDI IN Entry Point (midMessage)
// =============================================================================

extern "C" DWORD APIENTRY midMessage(UINT uDeviceID,
                                     UINT uMsg,
                                     DWORD_PTR dwUser,
                                     DWORD_PTR dwParam1,
                                     DWORD_PTR dwParam2) {
    switch (uMsg) {
    case MIDM_GETNUMDEVS:
        return 1;

    case MIDM_GETDEVCAPS: {
        if (uDeviceID != 0) {
            return MMSYSERR_BADDEVICEID;
        }
        if (dwParam1 == 0) {
            return MMSYSERR_INVALPARAM;
        }
        const size_t cbSize = static_cast<size_t>(dwParam2);
        if (cbSize == sizeof(MIDIINCAPSW)) {
            auto *caps = reinterpret_cast<LPMIDIINCAPSW>(dwParam1);
            std::memset(caps, 0, sizeof(*caps));
            caps->wMid = 0xFFFF;
            caps->wPid = 0x00BD;
            caps->vDriverVersion = 0x0100;
            std::wcsncpy(caps->szPname, kMidiDeviceNameW, MAXPNAMELEN - 1);
            caps->dwSupport = 0;
            return MMSYSERR_NOERROR;
        }
        if (cbSize == sizeof(MIDIINCAPSA)) {
            auto *caps = reinterpret_cast<LPMIDIINCAPSA>(dwParam1);
            std::memset(caps, 0, sizeof(*caps));
            caps->wMid = 0xFFFF;
            caps->wPid = 0x00BD;
            caps->vDriverVersion = 0x0100;
            std::strncpy(caps->szPname, kMidiDeviceNameA, MAXPNAMELEN - 1);
            caps->dwSupport = 0;
            return MMSYSERR_NOERROR;
        }
        return MMSYSERR_INVALPARAM;
    }

    case MIDM_OPEN: {
        if (uDeviceID != 0) {
            return MMSYSERR_BADDEVICEID;
        }
        if (dwUser == 0 || dwParam1 == 0) {
            return MMSYSERR_INVALPARAM;
        }
        const auto *openDesc = reinterpret_cast<const MIDIOPENDESC *>(dwParam1);

        auto client = std::make_shared<MidiInClient>();
        client->hMidi = reinterpret_cast<HMIDIIN>(openDesc->hMidi);
        client->dwCallback = openDesc->dwCallback;
        client->dwInstance = openDesc->dwInstance;
        client->dwFlags = static_cast<DWORD>(dwParam2);
        client->started.store(false, std::memory_order_release);

        GetTransport().AcquireClient();
        {
            std::lock_guard<std::mutex> lock(g_inRegistryMutex);
            g_activeInClients.push_back(client);
        }
        GetTransport().RegisterInClient(client);

        *reinterpret_cast<DWORD_PTR *>(dwUser) = reinterpret_cast<DWORD_PTR>(client.get());

        InvokeDriverCallback(
            client->dwCallback,
            client->dwFlags,
            reinterpret_cast<HDRVR>(client->hMidi),
            MIM_OPEN,
            client->dwInstance,
            0,
            0);
        return MMSYSERR_NOERROR;
    }

    case MIDM_CLOSE: {
        auto client = LookupInClient(dwUser);
        if (!client) {
            return MMSYSERR_INVALHANDLE;
        }
        client->started.store(false, std::memory_order_release);
        GetTransport().UnregisterInClient(client);

        {
            std::lock_guard<std::mutex> sLock(client->sysexMutex);
            for (LPMIDIHDR hdr : client->pendingBuffers) {
                hdr->dwBytesRecorded = 0;
                hdr->dwFlags = (hdr->dwFlags & ~MHDR_INQUEUE) | MHDR_DONE;
                InvokeDriverCallback(
                    client->dwCallback,
                    client->dwFlags,
                    reinterpret_cast<HDRVR>(client->hMidi),
                    MIM_LONGDATA,
                    client->dwInstance,
                    reinterpret_cast<DWORD_PTR>(hdr),
                    0);
            }
            client->pendingBuffers.clear();
        }

        {
            std::lock_guard<std::mutex> lock(g_inRegistryMutex);
            g_activeInClients.erase(
                std::remove(g_activeInClients.begin(), g_activeInClients.end(), client),
                g_activeInClients.end());
        }

        GetTransport().ReleaseClient();

        InvokeDriverCallback(
            client->dwCallback,
            client->dwFlags,
            reinterpret_cast<HDRVR>(client->hMidi),
            MIM_CLOSE,
            client->dwInstance,
            0,
            0);
        return MMSYSERR_NOERROR;
    }

    case MIDM_PREPARE:
    case MIDM_UNPREPARE:
        return MMSYSERR_NOTSUPPORTED;

    case MIDM_ADDBUFFER: {
        auto client = LookupInClient(dwUser);
        if (!client) {
            return MMSYSERR_INVALHANDLE;
        }
        auto *hdr = reinterpret_cast<LPMIDIHDR>(dwParam1);
        if (!hdr) {
            return MMSYSERR_INVALPARAM;
        }
        hdr->dwFlags &= ~MHDR_DONE;
        hdr->dwFlags |= MHDR_INQUEUE;
        hdr->dwBytesRecorded = 0;
        std::lock_guard<std::mutex> sLock(client->sysexMutex);
        client->pendingBuffers.push_back(hdr);
        return MMSYSERR_NOERROR;
    }

    case MIDM_START: {
        auto client = LookupInClient(dwUser);
        if (!client) {
            return MMSYSERR_INVALHANDLE;
        }
        client->startTickMs = GetTickCount();
        client->started.store(true, std::memory_order_release);
        return MMSYSERR_NOERROR;
    }

    case MIDM_STOP: {
        auto client = LookupInClient(dwUser);
        if (!client) {
            return MMSYSERR_INVALHANDLE;
        }
        client->started.store(false, std::memory_order_release);
        return MMSYSERR_NOERROR;
    }

    case MIDM_RESET: {
        auto client = LookupInClient(dwUser);
        if (!client) {
            return MMSYSERR_INVALHANDLE;
        }
        client->started.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> sLock(client->sysexMutex);
        for (LPMIDIHDR hdr : client->pendingBuffers) {
            hdr->dwBytesRecorded = 0;
            hdr->dwFlags = (hdr->dwFlags & ~MHDR_INQUEUE) | MHDR_DONE;
            InvokeDriverCallback(
                client->dwCallback,
                client->dwFlags,
                reinterpret_cast<HDRVR>(client->hMidi),
                MIM_LONGDATA,
                client->dwInstance,
                reinterpret_cast<DWORD_PTR>(hdr),
                0);
        }
        client->pendingBuffers.clear();
        return MMSYSERR_NOERROR;
    }

    default:
        return MMSYSERR_NOTSUPPORTED;
    }
}

// =============================================================================
// WinMM MIDI OUT Entry Point (modMessage)
// =============================================================================

extern "C" DWORD APIENTRY modMessage(UINT uDeviceID,
                                     UINT uMsg,
                                     DWORD_PTR dwUser,
                                     DWORD_PTR dwParam1,
                                     DWORD_PTR dwParam2) {
    switch (uMsg) {
    case MODM_GETNUMDEVS:
        return 1;

    case MODM_GETDEVCAPS: {
        if (uDeviceID != 0) {
            return MMSYSERR_BADDEVICEID;
        }
        if (dwParam1 == 0) {
            return MMSYSERR_INVALPARAM;
        }
        const size_t cbSize = static_cast<size_t>(dwParam2);
        if (cbSize == sizeof(MIDIOUTCAPSW)) {
            auto *caps = reinterpret_cast<LPMIDIOUTCAPSW>(dwParam1);
            std::memset(caps, 0, sizeof(*caps));
            caps->wMid = 0xFFFF;
            caps->wPid = 0x00BD;
            caps->vDriverVersion = 0x0100;
            std::wcsncpy(caps->szPname, kMidiDeviceNameW, MAXPNAMELEN - 1);
            caps->wTechnology = MOD_MIDIPORT;
            caps->wVoices = 0;
            caps->wNotes = 0;
            caps->wChannelMask = 0xFFFF;
            caps->dwSupport = 0;
            return MMSYSERR_NOERROR;
        }
        if (cbSize == sizeof(MIDIOUTCAPSA)) {
            auto *caps = reinterpret_cast<LPMIDIOUTCAPSA>(dwParam1);
            std::memset(caps, 0, sizeof(*caps));
            caps->wMid = 0xFFFF;
            caps->wPid = 0x00BD;
            caps->vDriverVersion = 0x0100;
            std::strncpy(caps->szPname, kMidiDeviceNameA, MAXPNAMELEN - 1);
            caps->wTechnology = MOD_MIDIPORT;
            caps->wVoices = 0;
            caps->wNotes = 0;
            caps->wChannelMask = 0xFFFF;
            caps->dwSupport = 0;
            return MMSYSERR_NOERROR;
        }
        return MMSYSERR_INVALPARAM;
    }

    case MODM_OPEN: {
        if (uDeviceID != 0) {
            return MMSYSERR_BADDEVICEID;
        }
        if (dwUser == 0 || dwParam1 == 0) {
            return MMSYSERR_INVALPARAM;
        }
        const auto *openDesc = reinterpret_cast<const MIDIOPENDESC *>(dwParam1);

        auto *client = new (std::nothrow) MidiOutClient();
        if (!client) {
            return MMSYSERR_NOMEM;
        }
        client->hMidi = reinterpret_cast<HMIDIOUT>(openDesc->hMidi);
        client->dwCallback = openDesc->dwCallback;
        client->dwInstance = openDesc->dwInstance;
        client->dwFlags = static_cast<DWORD>(dwParam2);

        GetTransport().AcquireClient();
        *reinterpret_cast<DWORD_PTR *>(dwUser) = reinterpret_cast<DWORD_PTR>(client);

        InvokeDriverCallback(
            client->dwCallback,
            client->dwFlags,
            reinterpret_cast<HDRVR>(client->hMidi),
            MOM_OPEN,
            client->dwInstance,
            0,
            0);
        return MMSYSERR_NOERROR;
    }

    case MODM_CLOSE: {
        auto *client = reinterpret_cast<MidiOutClient *>(dwUser);
        if (!client) {
            return MMSYSERR_INVALHANDLE;
        }

        const DWORD_PTR cb = client->dwCallback;
        const DWORD flags = client->dwFlags;
        const HMIDIOUT hMidi = client->hMidi;
        const DWORD_PTR inst = client->dwInstance;

        GetTransport().ReleaseClient();
        delete client;

        InvokeDriverCallback(
            cb,
            flags,
            reinterpret_cast<HDRVR>(hMidi),
            MOM_CLOSE,
            inst,
            0,
            0);
        return MMSYSERR_NOERROR;
    }

    case MODM_PREPARE:
    case MODM_UNPREPARE:
        return MMSYSERR_NOTSUPPORTED;

    case MODM_DATA: {
        auto *client = reinterpret_cast<MidiOutClient *>(dwUser);
        if (!client) {
            return MMSYSERR_INVALHANDLE;
        }
        uint8_t rawBytes[3]{};
        const size_t len = bcd2000::UnpackShortMidiMessage(static_cast<DWORD>(dwParam1), rawBytes);
        if (len > 0) {
            GetTransport().SendMidiBytes(rawBytes, len);
        }
        return MMSYSERR_NOERROR;
    }

    case MODM_LONGDATA: {
        auto *client = reinterpret_cast<MidiOutClient *>(dwUser);
        if (!client) {
            return MMSYSERR_INVALHANDLE;
        }
        auto *hdr = reinterpret_cast<LPMIDIHDR>(dwParam1);
        if (!hdr) {
            return MMSYSERR_INVALPARAM;
        }
        if (hdr->lpData && hdr->dwBufferLength > 0) {
            GetTransport().SendMidiBytes(
                reinterpret_cast<const uint8_t *>(hdr->lpData),
                static_cast<size_t>(hdr->dwBufferLength));
        }
        hdr->dwFlags = (hdr->dwFlags & ~MHDR_INQUEUE) | MHDR_DONE;
        InvokeDriverCallback(
            client->dwCallback,
            client->dwFlags,
            reinterpret_cast<HDRVR>(client->hMidi),
            MOM_DONE,
            client->dwInstance,
            reinterpret_cast<DWORD_PTR>(hdr),
            0);
        return MMSYSERR_NOERROR;
    }

    case MODM_RESET:
        return MMSYSERR_NOERROR;

    case MODM_GETVOLUME:
    case MODM_SETVOLUME:
        return MMSYSERR_NOTSUPPORTED;

    default:
        return MMSYSERR_NOTSUPPORTED;
    }
}
