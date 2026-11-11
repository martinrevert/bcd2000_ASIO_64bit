/**
 * @file bcd2000_asio_driver.h
 * @brief 64-bit User-Mode ASIO Driver Class for Behringer BCD2000 (VID 0x1397, PID 0x00BD)
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

#ifndef BCD2000_ASIO_DRIVER_H
#define BCD2000_ASIO_DRIVER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winusb.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/bcd2000_usb_protocol.h"
#include "../common/bcd2000_shared_state.h"
#include "../ui/bcd2000_control_panel_gui.h"
#include "../../third_party/asio-sdk/common/iasiodrv.h"

extern std::atomic<long> g_ActiveComObjects;
extern std::atomic<long> g_ServerLocks;

class Bcd2000AsioDriver final : public IASIO {
public:
    static constexpr long kNumInputs        = 4;
    static constexpr long kNumOutputs       = 4;
    static constexpr long kMinBufferSize    = 64;
    static constexpr long kMaxBufferSize    = 4096;
    static constexpr long kPreferredBufSize = 512;
    static constexpr long kBufferGranularity = 64;
    static constexpr double kDefaultSampleRate = 44100.0;

    Bcd2000AsioDriver();
    virtual ~Bcd2000AsioDriver();

    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IASIO methods
    ASIOBool init(void *sysHandle) override;
    void getDriverName(char *name) override;
    long getDriverVersion() override;
    void getErrorMessage(char *string) override;
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getChannels(long *numInputChannels, long *numOutputChannels) override;
    ASIOError getLatencies(long *inputLatency, long *outputLatency) override;
    ASIOError getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity) override;
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getSampleRate(ASIOSampleRate *sampleRate) override;
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getClockSources(ASIOClockSource *clocks, long *numSources) override;
    ASIOError setClockSource(long reference) override;
    ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo *info) override;
    ASIOError createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;
    ASIOError future(long selector, void *opt) override;
    ASIOError outputReady() override;

private:
    enum class DriverState {
        Loaded,
        Initialized,
        Prepared,
        Running
    };

    bool OpenUsbHardware();
    void CloseUsbHardware();
    bool SendLedCommand(uint8_t cc, uint8_t value);
    void SetStatusLeds(bool turnOn);
    void FlashHardwareLeds();
    bool SendRawMidiToUsb(const uint8_t *data, size_t len);
    void StartMidiBridge();
    void StopMidiBridge();
    void MidiUsbInThreadMain();
    void MidiPipeServerThreadMain();
    bool HasConnectedMidiClients();

    void StreamingThreadMain();
    void UsbStreamingThreadMain();
    void AsioHostThreadMain();
    void InvokeHostBufferSwitch();
    uint64_t QuerySystemTimeNanoseconds() const;

    std::atomic<ULONG> m_refCount{1};
    DriverState m_state{DriverState::Loaded};
    void *m_sysHandle{nullptr};
    char m_errorMessage[128]{};

    // Audio configuration
    double m_sampleRate{kDefaultSampleRate};
    long m_bufferSize{kPreferredBufSize};
    long m_preferredBufSize{kPreferredBufSize};
    HANDLE m_hSharedMem{nullptr};
    bcd2000::Bcd2000SharedState *m_sharedState{nullptr};
    std::atomic<uint64_t> m_xrunCount{0};
    ASIOCallbacks m_callbacks{};
    bool m_supportsTimeInfo{false};
    ASIOTime m_asioTime{};

    // Channel double buffers: [channel][doubleBufferIndex (0 or 1)]
    // Native 16-bit signed little-endian PCM (ASIOSTInt16LSB) matching the
    // BCD2000 TUSB3200 / CS4220 hardware DMA format.
    std::vector<int16_t> m_inputStorage[kNumInputs][2];
    std::vector<int16_t> m_outputStorage[kNumOutputs][2];
    bool m_inputActive[kNumInputs]{};
    bool m_outputActive[kNumOutputs]{};

    // Interleaved 4-channel S16_LE ring buffer for decoupling power-of-2 ASIO
    // buffers from 45-frame (360-byte) USB 1.1 Isochronous URBs
    static constexpr size_t kRingCapacityFrames = 32768; // Power of 2 (~728 ms headroom)
    std::vector<int16_t> m_outRingBuffer;                // kRingCapacityFrames * 4 int16_t
    std::atomic<uint64_t> m_ringWriteFrames{0};
    std::atomic<uint64_t> m_ringReadFrames{0};

    // Continuous fractional resampler state for any host sample rate (32k, 44.1k, 48k, 88.2k, 96k)
    // to match the hardware's 45,000 frames/sec (45 frames/ms) USB transfer rate.
    double m_resamplePhase{0.0};
    int16_t m_resampleHistory[4][kNumOutputs]{};

    // Real-time decoupled streaming state
    std::atomic<bool> m_streamingActive{false};
    std::atomic<bool> m_usbRateReady{false};
    std::atomic<bool> m_exactModeActive{true};
    std::atomic<double> m_usbTargetRate{kDefaultSampleRate};
    std::atomic<bool> m_prefillReady{false};
    HANDLE m_hAsioTriggerEvent{nullptr};
    std::thread m_usbThread;
    std::thread m_asioThread;
    long m_toggleIndex{0};
    std::atomic<uint64_t> m_samplePosition{0};
    std::atomic<uint64_t> m_systemTimeNs{0};
    double m_smoothedSystemTimeNs{0.0};
    LARGE_INTEGER m_qpcFreq{};

    // WinUSB Hardware Handles
    bool m_hardwareConnected{false};
    HANDLE m_hDevice{INVALID_HANDLE_VALUE};
    WINUSB_INTERFACE_HANDLE m_hInterface0{nullptr};
    WINUSB_INTERFACE_HANDLE m_hMidiInterface{nullptr};
    WINUSB_INTERFACE_HANDLE m_hAudioInterface{nullptr};
    std::vector<WINUSB_INTERFACE_HANDLE> m_allInterfaces;

    UCHAR m_midiOutPipe{0x01};
    UCHAR m_midiInPipe{0x81};
    UCHAR m_audioOutPipe{0x02};
    UCHAR m_audioInPipe{0x83};
    UCHAR m_audioAltSetting{0};
    USBD_PIPE_TYPE m_audioOutPipeType{UsbdPipeTypeIsochronous};
    ULONG m_audioOutBytesPerInterval{360};

    // Embedded WinUSB -> WinMM MIDI bridge (Named Pipe server + Overlapped EP 0x81/0x01)
    struct MidiPipeClient;
    std::mutex m_midiWriteMutex;
    std::atomic<bool> m_midiBridgeRunning{false};
    HANDLE m_hMidiStopEvent{nullptr};
    std::thread m_midiInThread;
    std::thread m_midiPipeThread;
    std::mutex m_midiClientsMutex;
    std::vector<std::shared_ptr<MidiPipeClient>> m_midiClients;
};

#endif // BCD2000_ASIO_DRIVER_H

