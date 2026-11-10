/**
 * @file bcd2000_asio_driver.h
 * @brief 64-bit User-Mode ASIO Driver Class for Behringer BCD2000 (VID 0x1397, PID 0x00BD)
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../third_party/asio-sdk/common/iasiodrv.h"

extern std::atomic<long> g_ActiveComObjects;
extern std::atomic<long> g_ServerLocks;

class Bcd2000AsioDriver final : public IASIO {
public:
    static constexpr long kNumInputs        = 4;
    static constexpr long kNumOutputs       = 4;
    static constexpr long kMinBufferSize    = 64;
    static constexpr long kMaxBufferSize    = 512;
    static constexpr long kPreferredBufSize = 256;
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

    void StreamingThreadMain();
    void InvokeHostBufferSwitch();
    uint64_t QuerySystemTimeNanoseconds() const;

    std::atomic<ULONG> m_refCount{1};
    DriverState m_state{DriverState::Loaded};
    void *m_sysHandle{nullptr};
    char m_errorMessage[128]{};

    // Audio configuration
    double m_sampleRate{kDefaultSampleRate};
    long m_bufferSize{kPreferredBufSize};
    ASIOCallbacks m_callbacks{};
    bool m_supportsTimeInfo{false};
    ASIOTime m_asioTime{};

    // Channel double buffers: [channel][doubleBufferIndex (0 or 1)]
    // Exposed to ASIO hosts as 32-bit signed PCM (ASIOSTInt32LSB) to prevent
    // host-side 16-bit truncation grit and 0 dBFS float->int16 wrap-around.
    std::vector<int32_t> m_inputStorage[kNumInputs][2];
    std::vector<int32_t> m_outputStorage[kNumOutputs][2];
    bool m_inputActive[kNumInputs]{};
    bool m_outputActive[kNumOutputs]{};

    // Interleaved 4-channel S16_LE ring buffer for decoupling power-of-2 ASIO
    // buffers from 44.1-frame/ms (360-byte) USB 1.1 Isochronous packets
    static constexpr size_t kRingCapacityFrames = 8192; // Power of 2
    std::vector<int16_t> m_outRingBuffer;               // kRingCapacityFrames * 4 int16_t
    uint64_t m_ringWriteFrames{0};
    uint64_t m_ringReadFrames{0};
    uint32_t m_ditherState{0x12345678u};
    uint32_t m_packetPhase{0};

    // Real-time streaming state
    std::atomic<bool> m_streamingActive{false};
    std::thread m_streamingThread;
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
};

#endif // BCD2000_ASIO_DRIVER_H

