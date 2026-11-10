/**
 * @file bcd2000_asio_driver.cpp
 * @brief Implementation of the 64-bit ASIO Driver (IASIO) for Behringer BCD2000
 */

#include "bcd2000_asio_driver.h"

#include <avrt.h>
#include <initguid.h>
#include <setupapi.h>
#include <usb.h>

#include <algorithm>
#include <chrono>
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

    HDEVINFO hIntfInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_WINUSB_BCD2000, nullptr, nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    std::wstring targetPath;
    const std::wstring vidStr = L"vid_1397";
    const std::wstring pidStr = L"pid_00bd";

    if (hIntfInfo != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(
                 hIntfInfo, nullptr, &GUID_DEVINTERFACE_WINUSB_BCD2000, idx,
                 &ifData);
             ++idx) {
            DWORD reqSize = 0;
            SetupDiGetDeviceInterfaceDetailW(hIntfInfo, &ifData, nullptr, 0,
                                             &reqSize, nullptr);
            if (reqSize == 0)
                continue;

            std::vector<BYTE> buf(reqSize);
            auto detailData =
                reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (SetupDiGetDeviceInterfaceDetailW(hIntfInfo, &ifData, detailData,
                                                 reqSize, nullptr, nullptr)) {
                std::wstring path(detailData->DevicePath);
                if (ContainsInsensitiveW(path, vidStr) &&
                    ContainsInsensitiveW(path, pidStr)) {
                    targetPath = path;
                    break;
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hIntfInfo);
    }

    if (targetPath.empty()) {
        std::snprintf(m_errorMessage, sizeof(m_errorMessage),
                      "BCD2000 (1397:00BD) WinUSB device not found.");
        return false;
    }

    m_hDevice = CreateFileW(targetPath.c_str(), GENERIC_WRITE | GENERIC_READ,
                            FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                            nullptr);

    if (m_hDevice == INVALID_HANDLE_VALUE) {
        std::snprintf(m_errorMessage, sizeof(m_errorMessage),
                      "CreateFileW failed on BCD2000 (err=%lu).",
                      GetLastError());
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

    m_hardwareConnected = true;
    return true;
}

void Bcd2000AsioDriver::CloseUsbHardware() {
    if (m_hardwareConnected) {
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

bool Bcd2000AsioDriver::SendLedCommand(uint8_t cc, uint8_t value) {
    if (!m_hMidiInterface || m_midiOutPipe == 0)
        return false;

    uint8_t frame[64] = {0};
    frame[0] = 0x03;
    frame[1] = 0x00;
    frame[2] = 0x03;
    frame[3] = 0xB0;
    frame[4] = cc;
    frame[5] = value;

    ULONG sent = 0;
    return WinUsb_WritePipe(m_hMidiInterface, m_midiOutPipe, frame,
                            sizeof(frame), &sent, nullptr) != FALSE;
}

void Bcd2000AsioDriver::SetStatusLeds(bool turnOn) {
    uint8_t val = turnOn ? 0x7F : 0x00;
    const uint8_t leds[4] = {kLedPlayA, kLedPlayB, kLedPflA, kLedPflB};
    for (uint8_t cc : leds) {
        SendLedCommand(cc, val);
        Sleep(10);
    }
}

// ============================================================================
// IASIO Core Methods
// ============================================================================

ASIOBool Bcd2000AsioDriver::init(void *sysHandle) {
    m_sysHandle = sysHandle;

    if (m_state != DriverState::Loaded) {
        return ASIOTrue;
    }

    // Open the physical BCD2000 via WinUSB (falls back gracefully to timer clock
    // if run in an environment without the USB hardware attached)
    OpenUsbHardware();

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
    *maxSize = kMaxBufferSize;         // 512 samples (~11.6 ms)
    *preferredSize = kPreferredBufSize; // 256 samples (~5.80 ms)
    *granularity = kBufferGranularity; // 64 samples
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::canSampleRate(ASIOSampleRate sampleRate) {
    if (sampleRate >= 44099.0 && sampleRate <= 44101.0) {
        return ASE_OK;
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
    if (canSampleRate(sampleRate) != ASE_OK) {
        return ASE_NoClock;
    }
    m_sampleRate = kDefaultSampleRate;
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
                  "BCD2000 Internal 44.1kHz");
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
    if (m_hardwareConnected) {
        SetStatusLeds(true);
    }

    m_toggleIndex = 0;
    m_samplePosition.store(0, std::memory_order_release);
    m_systemTimeNs.store(QuerySystemTimeNanoseconds(),
                         std::memory_order_release);
    m_ringWriteFrames = 0;
    m_ringReadFrames = 0;
    std::fill(m_outRingBuffer.begin(), m_outRingBuffer.end(), static_cast<int16_t>(0));

    m_streamingActive.store(true, std::memory_order_release);
    m_state = DriverState::Running;
    m_streamingThread = std::thread(&Bcd2000AsioDriver::StreamingThreadMain, this);

    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::stop() {
    if (m_state != DriverState::Running) {
        return ASE_OK;
    }

    m_streamingActive.store(false, std::memory_order_release);
    if (m_streamingThread.joinable()) {
        m_streamingThread.join();
    }

    if (m_hardwareConnected) {
        SetStatusLeds(false);
    }

    m_state = DriverState::Prepared;
    return ASE_OK;
}

ASIOError Bcd2000AsioDriver::controlPanel() {
    char msg[512];
    std::snprintf(
        msg, sizeof(msg),
        "Behringer BCD2000 ASIO 64-bit Driver (v2.3)\n"
        "--------------------------------------------------\n"
        "Hardware Status : %s\n"
        "Sample Rate     : %.0f Hz (16-bit Signed LE)\n"
        "ASIO Buffer Size: %ld samples (%.2f ms)\n"
        "Output Channels : 4 (Ch 1-2 Master RCA, Ch 3-4 Headphones)\n"
        "Input Channels  : 4 (Ch 1-2 Deck A/Mic, Ch 3-4 Deck B)\n"
        "Transport Layer : WinUSB.sys Isochronous (EP 0x02 OUT)",
        m_hardwareConnected ? "CONNECTED (VID 0x1397, PID 0x00BD)"
                            : "DISCONNECTED (Timer Simulation)",
        m_sampleRate, m_bufferSize,
        (static_cast<double>(m_bufferSize) * 1000.0) / m_sampleRate);

    MessageBoxA(static_cast<HWND>(m_sysHandle), msg,
                "Behringer BCD2000 ASIO 64-bit Control Panel",
                MB_OK | MB_ICONINFORMATION);
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
    const uint64_t nowNs = QuerySystemTimeNanoseconds();
    const uint64_t curPos = m_samplePosition.load(std::memory_order_relaxed);

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

    // Interleave the 4 non-interleaved ASIO output channels into m_outRingBuffer
    const int16_t *ch0 = m_outputActive[0] ? m_outputStorage[0][bufIdx].data() : nullptr;
    const int16_t *ch1 = m_outputActive[1] ? m_outputStorage[1][bufIdx].data() : nullptr;
    const int16_t *ch2 = m_outputActive[2] ? m_outputStorage[2][bufIdx].data() : nullptr;
    const int16_t *ch3 = m_outputActive[3] ? m_outputStorage[3][bufIdx].data() : nullptr;

    const size_t mask = kRingCapacityFrames - 1;
    uint64_t writeIdx = m_ringWriteFrames;

    for (long s = 0; s < m_bufferSize; ++s) {
        size_t slot = static_cast<size_t>(writeIdx & mask) * 4;
        m_outRingBuffer[slot + 0] = ch0 ? ch0[s] : 0; // Master L
        m_outRingBuffer[slot + 1] = ch1 ? ch1[s] : 0; // Master R
        m_outRingBuffer[slot + 2] = ch2 ? ch2[s] : 0; // Phones L
        m_outRingBuffer[slot + 3] = ch3 ? ch3[s] : 0; // Phones R
        writeIdx++;
    }

    m_ringWriteFrames = writeIdx;
    m_samplePosition.store(curPos + static_cast<uint64_t>(m_bufferSize),
                           std::memory_order_release);
    m_toggleIndex ^= 1;
}

void Bcd2000AsioDriver::StreamingThreadMain() {
    // 1. Elevate thread priority via MMCSS ("Pro Audio")
    DWORD mmcssTaskIdx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIdx);
    if (hMmcss) {
        AvSetMmThreadPriority(hMmcss, AVRT_PRIORITY_CRITICAL);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    // 2. If physical BCD2000 hardware is connected, drive ASIO callbacks locked
    //    to the WinUSB Isochronous OUT completion pipe (Endpoint 0x02).
    if (m_hardwareConnected && m_hAudioInterface) {
        constexpr size_t kNumUrbs = 6;
        constexpr size_t kPacketsPerUrb = 8; // 8 ms per URB (360 frames = 2880 B)
        const ULONG packetSize = m_audioOutBytesPerInterval;
        const ULONG urbBufferSize = static_cast<ULONG>(kPacketsPerUrb * packetSize);
        const ULONG totalDmaBytes = static_cast<ULONG>(kNumUrbs * urbBufferSize);
        const size_t framesPerUrb = urbBufferSize / 8; // 8 bytes per 4-ch S16_LE frame

        std::vector<uint8_t> dmaBuffer(totalDmaBytes, 0);
        WINUSB_ISOCH_BUFFER_HANDLE hIsochBuffer = nullptr;
        const bool isIsoch = (m_audioOutPipeType == UsbdPipeTypeIsochronous);

        WinUsb_ResetPipe(m_hAudioInterface, m_audioOutPipe);
        if (isIsoch) {
            if (!WinUsb_RegisterIsochBuffer(m_hAudioInterface, m_audioOutPipe,
                                            dmaBuffer.data(), totalDmaBytes,
                                            &hIsochBuffer)) {
                hIsochBuffer = nullptr;
            }
        }

        struct UrbSlot {
            OVERLAPPED ov{};
            ULONG offset = 0;
            ULONG length = 0;
            bool inFlight = false;
        };

        std::vector<UrbSlot> urbs(kNumUrbs);
        for (size_t i = 0; i < kNumUrbs; ++i) {
            urbs[i].offset = static_cast<ULONG>(i * urbBufferSize);
            urbs[i].length = urbBufferSize;
            urbs[i].ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        }

        bool continueStream = false;
        bool useExplicitFrame = false;
        ULONG nextFrameNumber = 0;

        auto FillAndSubmitUrb = [&](size_t idx) -> bool {
            UrbSlot &slot = urbs[idx];
            ResetEvent(slot.ov.hEvent);

            // Invoke ASIO bufferSwitch until we have enough frames in the ring buffer
            while ((m_ringWriteFrames - m_ringReadFrames) < framesPerUrb &&
                   m_streamingActive.load(std::memory_order_acquire)) {
                InvokeHostBufferSwitch();
            }

            // Copy framesPerUrb 4-channel frames into this URB's DMA slice
            int16_t *dst = reinterpret_cast<int16_t *>(dmaBuffer.data() + slot.offset);
            const size_t mask = kRingCapacityFrames - 1;
            uint64_t readIdx = m_ringReadFrames;

            for (size_t f = 0; f < framesPerUrb; ++f) {
                size_t ringSlot = static_cast<size_t>(readIdx & mask) * 4;
                dst[f * 4 + 0] = m_outRingBuffer[ringSlot + 0];
                dst[f * 4 + 1] = m_outRingBuffer[ringSlot + 1];
                dst[f * 4 + 2] = m_outRingBuffer[ringSlot + 2];
                dst[f * 4 + 3] = m_outRingBuffer[ringSlot + 3];
                readIdx++;
            }
            m_ringReadFrames = readIdx;

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

        bool pipelineActive = true;
        for (size_t i = 0; i < kNumUrbs; ++i) {
            if (!FillAndSubmitUrb(i)) {
                pipelineActive = false;
                break;
            }
        }

        size_t nextReap = 0;
        while (pipelineActive &&
               m_streamingActive.load(std::memory_order_acquire)) {
            UrbSlot &slot = urbs[nextReap];
            if (!slot.inFlight) {
                break;
            }
            DWORD waitRes = WaitForSingleObject(slot.ov.hEvent, 500);
            if (waitRes != WAIT_OBJECT_0) {
                break;
            }
            DWORD bytesTransferred = 0;
            BOOL ovOk = WinUsb_GetOverlappedResult(
                m_hAudioInterface, &slot.ov, &bytesTransferred, FALSE);
            slot.inFlight = false;
            if (!ovOk) {
                continueStream = false;
            }

            if (!m_streamingActive.load(std::memory_order_acquire)) {
                break;
            }
            if (!FillAndSubmitUrb(nextReap)) {
                break;
            }
            nextReap = (nextReap + 1) % kNumUrbs;
        }

        // Cleanly drain in-flight URBs and park DAC at 0
        for (size_t i = 0; i < kNumUrbs; ++i) {
            if (urbs[i].inFlight) {
                WaitForSingleObject(urbs[i].ov.hEvent, 150);
                DWORD dummy = 0;
                WinUsb_GetOverlappedResult(m_hAudioInterface, &urbs[i].ov,
                                           &dummy, FALSE);
                urbs[i].inFlight = false;
            }
        }

        if (isIsoch && hIsochBuffer) {
            std::memset(dmaBuffer.data(), 0, urbBufferSize);
            ResetEvent(urbs[0].ov.hEvent);
            if (WinUsb_WriteIsochPipeAsap(hIsochBuffer, 0, urbBufferSize, FALSE,
                                          &urbs[0].ov) ||
                GetLastError() == ERROR_IO_PENDING) {
                WaitForSingleObject(urbs[0].ov.hEvent, 100);
            }
        }

        WinUsb_AbortPipe(m_hAudioInterface, m_audioOutPipe);

        for (size_t i = 0; i < kNumUrbs; ++i) {
            if (urbs[i].ov.hEvent) {
                CloseHandle(urbs[i].ov.hEvent);
            }
        }

        if (hIsochBuffer) {
            WinUsb_UnregisterIsochBuffer(hIsochBuffer);
        }

        if (hMmcss) {
            AvRevertMmThreadCharacteristics(hMmcss);
        }
        return;
    }

    // 3. Fallback High-Precision QPC Timer Loop (when running without USB hardware)
    const double periodSec =
        static_cast<double>(m_bufferSize) / m_sampleRate;
    auto nextWake = std::chrono::steady_clock::now();

    while (m_streamingActive.load(std::memory_order_acquire)) {
        InvokeHostBufferSwitch();
        nextWake += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(periodSec));
        std::this_thread::sleep_until(nextWake);
    }

    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }
}
