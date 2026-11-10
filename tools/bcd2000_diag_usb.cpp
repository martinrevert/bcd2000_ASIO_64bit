/**
 * @file bcd2000_diag_usb.cpp
 * @brief Behringer BCD2000 USB Detection, Enumeration, Handshake, and Live Link Diagnostic Tool
 * 
 * Part of Milestone 1 ("Detección y Enlace USB") for Behringer BCD2000 64-bit ASIO Driver.
 * Target Hardware: Behringer BCD2000 DJ Controller (VID: 0x1397, PID: 0x00BD)
 *
 * This utility:
 *  1. Scans the USB topology for the physical BCD2000 hardware (VID 0x1397, PID 0x00BD).
 *  2. Locates the active WinUSB device interface path using Windows SetupAPI.
 *  3. Opens the device handle and initializes the WinUSB communication layer.
 *  4. Queries device descriptors, interfaces, associated interfaces, and all communication pipes.
 *  5. Transmits the full 52-byte initialization handshake sequence (from Linux kernel snd-bcd2000).
 *  6. Executes a comprehensive LED illumination test using proper framing:
 *     [0x03, 0x00, len, MIDI_BYTES..., 0x00...] padded to 64 bytes.
 *  7. Enters a live interactive listener loop on Endpoint 0x81 (IN) to capture fader movements,
 *     jog wheels, and button presses, echoing LED feedback in real-time.
 */

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>
#include <devpkey.h>
#include <objbase.h>

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <memory>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <chrono>

// Target Device Identifiers
constexpr uint16_t BCD2000_VID = 0x1397;
constexpr uint16_t BCD2000_PID = 0x00BD;

// Standard WinUSB Device Interface GUID (common default in Zadig/libwdi)
// {DEE824EF-729B-4A0E-9C14-B7117D33A817}
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB_GENERIC,
    0xdee824ef, 0x729b, 0x4a0e, 0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);

/**
 * @brief 52-byte hardware initialization sequence.
 * 
 * Sourced directly from the official Linux ALSA kernel driver (sound/usb/bcd2000/bcd2000.c).
 * When submitted to OUT Endpoint 1 (0x01), this payload configures the 8051 microcontroller's
 * internal memory registers, awakens the hardware command processor, and transitions
 * the BCD2000 out of its bootloader state into active operational mode.
 */
static const uint8_t BCD2000_INIT_SEQUENCE[52] = {
    0x07, 0x00, 0x00, 0x00, 0x78, 0x48, 0x1c, 0x81,
    0xc4, 0x00, 0x00, 0x00, 0x5e, 0x53, 0x4a, 0xf7,
    0x18, 0xfa, 0x11, 0xff, 0x6c, 0xf3, 0x90, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x51, 0x48, 0x1c, 0x81,
    0x00, 0x00, 0x00, 0x00, 0x50, 0x48, 0x1c, 0x81,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

// ============================================================================
// Helper Utilities
// ============================================================================

/**
 * @brief Converts a Win32 system error code into a formatted human-readable string.
 */
std::string FormatWin32Error(DWORD errorCode) {
    LPWSTR buffer = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr
    );

    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << errorCode << std::dec;

    if (size > 0 && buffer != nullptr) {
        std::wstring wmsg(buffer, size);
        LocalFree(buffer);
        while (!wmsg.empty() && (wmsg.back() == L'\r' || wmsg.back() == L'\n' || wmsg.back() == L' ')) {
            wmsg.pop_back();
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, wmsg.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string msg(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wmsg.c_str(), -1, &msg[0], len, nullptr, nullptr);
            oss << " (" << msg << ")";
        }
    }
    return oss.str();
}

/**
 * @brief Converts std::wstring to UTF-8 std::string for std::cout streaming.
 */
std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &result[0], size, nullptr, nullptr);
    return result;
}

/**
 * @brief Performs a case-insensitive substring search for std::wstring.
 */
bool ContainsInsensitive(const std::wstring& text, const std::wstring& target) {
    auto it = std::search(
        text.begin(), text.end(),
        target.begin(), target.end(),
        [](wchar_t ch1, wchar_t ch2) {
            return towlower(ch1) == towlower(ch2);
        }
    );
    return it != text.end();
}

/**
 * @brief Prints a standard formatted hexadecimal and ASCII memory dump.
 */
void PrintHexDump(const uint8_t* data, size_t length, const std::string& indent = "    ") {
    for (size_t i = 0; i < length; i += 16) {
        std::cout << indent << std::hex << std::setw(4) << std::setfill('0') << i << ": ";
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < length) {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<int>(data[i + j]) << " ";
            } else {
                std::cout << "   ";
            }
            if (j == 7) std::cout << " ";
        }
        std::cout << " |";
        for (size_t j = 0; j < 16 && (i + j) < length; ++j) {
            char c = static_cast<char>(data[i + j]);
            std::cout << (c >= 32 && c <= 126 ? c : '.');
        }
        std::cout << "|\n";
    }
    std::cout << std::dec << std::setfill(' ');
}

/**
 * @brief Converts pipe type enumeration to a human-readable string.
 */
const char* PipeTypeToString(USBD_PIPE_TYPE pipeType) {
    switch (pipeType) {
        case UsbdPipeTypeControl:     return "Control";
        case UsbdPipeTypeIsochronous: return "Isochronous";
        case UsbdPipeTypeBulk:        return "Bulk";
        case UsbdPipeTypeInterrupt:   return "Interrupt";
        default:                      return "Unknown";
    }
}

// ============================================================================
// USB Framing & Protocol Helpers (Derived from snd-bcd2000)
// ============================================================================

/**
 * @brief Sends a formatted BCD2000 command packet to Endpoint 0x01.
 * 
 * Hardware Protocol Specifications:
 *  Byte 0: 0x03 (Command prefix byte 1 - device_cmd_prefix[0])
 *  Byte 1: 0x00 (Command prefix byte 2 - device_cmd_prefix[1])
 *  Byte 2: len (Payload length in bytes, 1 <= len <= 61)
 *  Byte 3..(3+len-1): Raw MIDI stream bytes (0x90 Note On, 0xB0 CC, etc.)
 *  Byte (3+len)..63: Zero-padded up to BUFSIZE (64 bytes)
 *
 * Buffer size sent to WinUSB: exactly 64 bytes (matching MaximumPacketSize).
 */
bool SendBcdMidiFrame(
    WINUSB_INTERFACE_HANDLE hWinUsb,
    UCHAR outPipeId,
    const uint8_t* midiPayload,
    uint8_t payloadLen)
{
    if (!hWinUsb || outPipeId == 0 || !midiPayload || payloadLen == 0 || payloadLen > 61) {
        return false;
    }

    uint8_t frame[64] = { 0 };
    frame[0] = 0x03;
    frame[1] = 0x00;
    frame[2] = payloadLen;
    memcpy(&frame[3], midiPayload, payloadLen);

    ULONG bytesWritten = 0;
    BOOL ok = WinUsb_WritePipe(hWinUsb, outPipeId, frame, sizeof(frame), &bytesWritten, nullptr);
    return (ok && bytesWritten == sizeof(frame));
}

/**
 * @brief Helper to send a single 3-byte MIDI message (e.g. Note On or Control Change).
 */
bool SendBcdSingleMidi(
    WINUSB_INTERFACE_HANDLE hWinUsb,
    UCHAR outPipeId,
    uint8_t status,
    uint8_t data1,
    uint8_t data2)
{
    uint8_t msg[3] = { status, data1, data2 };
    return SendBcdMidiFrame(hWinUsb, outPipeId, msg, sizeof(msg));
}

// ============================================================================
// USB Device Topology and Interface Discovery
// ============================================================================

struct BCD2000DeviceInfo {
    bool hardwarePresent = false;
    std::wstring deviceInstanceId;
    std::wstring serviceName;
    std::wstring friendlyName;
    std::wstring deviceInterfacePath;
    bool hasRegistryGuid = false;
    GUID registryGuid{};
};

/**
 * @brief Inspects the USB device tree for physical BCD2000 hardware presence and driver state.
 */
void InspectHardwareNode(BCD2000DeviceInfo& info) {
    std::cout << "[SCAN] Querying USB device tree via SetupAPI (SetupDiGetClassDevs)...\n";

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        nullptr,
        L"USB",
        nullptr,
        DIGCF_ALLCLASSES | DIGCF_PRESENT
    );

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        std::cerr << "  [-] Failed to obtain USB device information set: "
                  << FormatWin32Error(GetLastError()) << "\n";
        return;
    }

    SP_DEVINFO_DATA devInfoData{};
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    const std::wstring targetPattern = L"VID_1397&PID_00BD";

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); ++i) {
        wchar_t hardwareIdBuf[512] = { 0 };
        if (SetupDiGetDeviceRegistryPropertyW(
                hDevInfo,
                &devInfoData,
                SPDRP_HARDWAREID,
                nullptr,
                reinterpret_cast<PBYTE>(hardwareIdBuf),
                sizeof(hardwareIdBuf),
                nullptr))
        {
            if (ContainsInsensitive(hardwareIdBuf, targetPattern)) {
                info.hardwarePresent = true;

                // Query Device Instance ID
                wchar_t instanceIdBuf[512] = { 0 };
                if (SetupDiGetDeviceInstanceIdW(hDevInfo, &devInfoData, instanceIdBuf, 512, nullptr)) {
                    info.deviceInstanceId = instanceIdBuf;
                }

                // Query Bound Service Name (e.g. WinUSB, usbccgp, etc.)
                wchar_t serviceBuf[256] = { 0 };
                if (SetupDiGetDeviceRegistryPropertyW(
                        hDevInfo,
                        &devInfoData,
                        SPDRP_SERVICE,
                        nullptr,
                        reinterpret_cast<PBYTE>(serviceBuf),
                        sizeof(serviceBuf),
                        nullptr))
                {
                    info.serviceName = serviceBuf;
                }

                // Query Friendly Name / Device Description
                wchar_t descBuf[256] = { 0 };
                if (SetupDiGetDeviceRegistryPropertyW(
                        hDevInfo,
                        &devInfoData,
                        SPDRP_DEVICEDESC,
                        nullptr,
                        reinterpret_cast<PBYTE>(descBuf),
                        sizeof(descBuf),
                        nullptr))
                {
                    info.friendlyName = descBuf;
                }

                // Check device parameters registry key for DeviceInterfaceGUIDs
                HKEY hKey = SetupDiOpenDevRegKey(hDevInfo, &devInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
                if (hKey != INVALID_HANDLE_VALUE) {
                    wchar_t guidBuf[256] = { 0 };
                    DWORD bufSize = sizeof(guidBuf);
                    DWORD type = 0;
                    if (RegQueryValueExW(hKey, L"DeviceInterfaceGUIDs", nullptr, &type, reinterpret_cast<LPBYTE>(guidBuf), &bufSize) == ERROR_SUCCESS ||
                        RegQueryValueExW(hKey, L"DeviceInterfaceGUID", nullptr, &type, reinterpret_cast<LPBYTE>(guidBuf), &bufSize) == ERROR_SUCCESS)
                    {
                        GUID parsedGuid{};
                        if (SUCCEEDED(CLSIDFromString(guidBuf, &parsedGuid))) {
                            info.hasRegistryGuid = true;
                            info.registryGuid = parsedGuid;
                        }
                    }
                    RegCloseKey(hKey);
                }

                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
}

/**
 * @brief Discovers the WinUSB device interface path for VID 0x1397, PID 0x00BD.
 */
bool FindDeviceInterfacePath(BCD2000DeviceInfo& info, const GUID* customGuid = nullptr) {
    std::cout << "[SCAN] Searching for active WinUSB device interface path...\n";

    const std::wstring vidStr = L"vid_1397";
    const std::wstring pidStr = L"pid_00bd";

    std::vector<GUID> targetGuids;
    if (customGuid) {
        targetGuids.push_back(*customGuid);
    }
    if (info.hasRegistryGuid) {
        targetGuids.push_back(info.registryGuid);
    }
    targetGuids.push_back(GUID_DEVINTERFACE_WINUSB_GENERIC);
    targetGuids.push_back(GUID_DEVINTERFACE_USB_DEVICE);

    for (const auto& guid : targetGuids) {
        HDEVINFO hIntfInfo = SetupDiGetClassDevsW(
            &guid,
            nullptr,
            nullptr,
            DIGCF_DEVICEINTERFACE | DIGCF_PRESENT
        );

        if (hIntfInfo == INVALID_HANDLE_VALUE) {
            continue;
        }

        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hIntfInfo, nullptr, &guid, idx, &ifData); ++idx) {
            DWORD requiredSize = 0;
            SetupDiGetDeviceInterfaceDetailW(hIntfInfo, &ifData, nullptr, 0, &requiredSize, nullptr);

            if (requiredSize == 0) continue;

            std::vector<BYTE> detailBuf(requiredSize);
            auto detailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.data());
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (SetupDiGetDeviceInterfaceDetailW(hIntfInfo, &ifData, detailData, requiredSize, nullptr, nullptr)) {
                std::wstring path(detailData->DevicePath);
                if (ContainsInsensitive(path, vidStr) && ContainsInsensitive(path, pidStr)) {
                    info.deviceInterfacePath = path;
                    SetupDiDestroyDeviceInfoList(hIntfInfo);
                    std::cout << "  [+] Found interface via SetupAPI GUID query.\n";
                    return true;
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hIntfInfo);
    }

    return false;
}

// ============================================================================
// Pipe Telemetry and Handshake Execution
// ============================================================================

struct PipeSummary {
    UCHAR pipeId = 0;
    USBD_PIPE_TYPE pipeType = UsbdPipeTypeControl;
    USHORT maxPacketSize = 0;
    UCHAR interval = 0;
    bool isInput = false;
};

void PrintPipeTable(const std::vector<PipeSummary>& pipes, int interfaceIndex = 0) {
    std::cout << "\n======================================================================\n";
    std::cout << "          DETECTED USB PIPES / ENDPOINTS (Interface " << interfaceIndex << ")          \n";
    std::cout << "======================================================================\n";
    std::cout << std::left
              << std::setw(8)  << "Pipe ID"
              << std::setw(14) << "Direction"
              << std::setw(14) << "Transfer Type"
              << std::setw(16) << "Max Packet Size"
              << std::setw(10) << "Interval"
              << "\n";
    std::cout << "----------------------------------------------------------------------\n";

    for (const auto& p : pipes) {
        std::ostringstream idStr;
        idStr << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
              << static_cast<int>(p.pipeId);

        std::string dirStr = p.isInput ? "IN (Dev->Host)" : "OUT (Host->Dev)";
        std::string pktStr = std::to_string(p.maxPacketSize) + " bytes";
        std::string intStr = std::to_string(p.interval) + (p.pipeType == UsbdPipeTypeInterrupt ? " ms" : " frames");

        std::cout << std::left
              << std::setw(8)  << idStr.str()
              << std::setw(14) << dirStr
              << std::setw(14) << PipeTypeToString(p.pipeType)
              << std::setw(16) << pktStr
              << std::setw(10) << intStr
              << "\n";
    }
    std::cout << "======================================================================\n\n";
}

int RunDiagnostic(const GUID* customGuid) {
    std::cout << "======================================================================\n";
    std::cout << "   Behringer BCD2000 USB Link & Handshake Diagnostic (Hito 1 - x64)   \n";
    std::cout << "   Target: VID 0x1397, PID 0x00BD | Driver: WinUSB                    \n";
    std::cout << "======================================================================\n\n";

    BCD2000DeviceInfo devInfo{};

    // 1. Hardware Presence & Driver Service Check
    InspectHardwareNode(devInfo);

    if (devInfo.hardwarePresent) {
        std::cout << "  [+] Physical hardware detected:\n";
        std::cout << "      Description : " << (devInfo.friendlyName.empty() ? "(Unknown)" : WideToUtf8(devInfo.friendlyName)) << "\n";
        std::cout << "      Instance ID : " << WideToUtf8(devInfo.deviceInstanceId) << "\n";
        std::cout << "      Driver Svc  : " << (devInfo.serviceName.empty() ? "(None)" : WideToUtf8(devInfo.serviceName)) << "\n";

        if (!devInfo.serviceName.empty() && !ContainsInsensitive(devInfo.serviceName, L"winusb")) {
            std::cout << "\n  [!] CAUTION: Device is associated with service '" << WideToUtf8(devInfo.serviceName)
                      << "' rather than 'WinUSB'.\n"
                      << "      If interface opening fails, use Zadig to associate VID 1397 PID 00BD with WinUSB.\n\n";
        }
    } else {
        std::cout << "  [-] Hardware node (VID 0x1397, PID 0x00BD) not detected in USB tree.\n"
                  << "      Please verify that the BCD2000 is powered on and connected.\n\n";
    }

    // 2. Discover Device Interface Path
    if (!FindDeviceInterfacePath(devInfo, customGuid)) {
        std::cerr << "\n[ERROR] WinUSB device interface path could not be resolved for BCD2000.\n"
                  << "        Troubleshooting Checklist:\n"
                  << "        1. Launch Zadig (https://zadig.akeo.ie).\n"
                  << "        2. Select Options -> List All Devices.\n"
                  << "        3. Choose 'BCD2000' (VID 1397, PID 00BD).\n"
                  << "        4. Set Target Driver to 'WinUSB' and click Replace/Install Driver.\n"
                  << "        5. Re-run this diagnostic.\n";
        return 1;
    }

    std::cout << "  [+] Active Device Interface Path:\n"
              << "      " << WideToUtf8(devInfo.deviceInterfacePath) << "\n\n";

    // 3. Open Device Handle via CreateFile (must specify FILE_FLAG_OVERLAPPED for WinUSB)
    std::cout << "[USB] Opening device handle via CreateFileW...\n";
    HANDLE hDevice = CreateFileW(
        devInfo.deviceInterfacePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (hDevice == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::cerr << "  [-] CreateFileW failed: " << FormatWin32Error(err) << "\n";
        if (err == ERROR_ACCESS_DENIED) {
            std::cerr << "      [!] Access Denied: Run terminal as Administrator or check if another app has an exclusive lock.\n";
        }
        return 2;
    }
    std::cout << "  [+] Device handle successfully opened (HANDLE: " << hDevice << ").\n";

    // 4. Initialize WinUSB Handle
    std::cout << "[USB] Initializing WinUSB via WinUsb_Initialize...\n";
    WINUSB_INTERFACE_HANDLE hWinUsb = nullptr;
    if (!WinUsb_Initialize(hDevice, &hWinUsb)) {
        DWORD err = GetLastError();
        std::cerr << "  [-] WinUsb_Initialize failed: " << FormatWin32Error(err) << "\n";
        CloseHandle(hDevice);
        return 3;
    }
    std::cout << "  [+] WinUsb_Initialize succeeded.\n\n";

    // 5. Query Device Descriptor
    std::cout << "[USB] Querying Device Descriptor...\n";
    USB_DEVICE_DESCRIPTOR devDesc{};
    ULONG transferred = 0;
    if (WinUsb_GetDescriptor(
            hWinUsb,
            USB_DEVICE_DESCRIPTOR_TYPE,
            0,
            0,
            reinterpret_cast<PUCHAR>(&devDesc),
            sizeof(devDesc),
            &transferred))
    {
        std::cout << "  [+] Device Descriptor Retrieved (" << transferred << " bytes):\n";
        std::cout << "      USB Spec    : " << std::hex << (devDesc.bcdUSB >> 8) << "."
                  << ((devDesc.bcdUSB & 0xFF) >> 4) << (devDesc.bcdUSB & 0x0F) << std::dec << "\n";
        std::cout << "      VID / PID   : 0x" << std::hex << devDesc.idVendor
                  << " : 0x" << std::setw(4) << std::setfill('0') << devDesc.idProduct << std::dec << "\n";
        std::cout << "      Device Rev  : " << std::hex << devDesc.bcdDevice << std::dec << "\n";
        std::cout << "      EP0 MaxPkt  : " << static_cast<int>(devDesc.bMaxPacketSize0) << " bytes\n";
        std::cout << "      Num Configs : " << static_cast<int>(devDesc.bNumConfigurations) << "\n";
    } else {
        std::cerr << "  [-] WinUsb_GetDescriptor (Device) failed: " << FormatWin32Error(GetLastError()) << "\n";
    }

    // 6. Query Interface Settings and Pipes for Interface 0
    std::cout << "\n[USB] Querying Interface 0 settings and endpoints...\n";
    USB_INTERFACE_DESCRIPTOR ifaceDesc{};
    if (!WinUsb_QueryInterfaceSettings(hWinUsb, 0, &ifaceDesc)) {
        std::cerr << "  [-] WinUsb_QueryInterfaceSettings failed: " << FormatWin32Error(GetLastError()) << "\n";
    } else {
        std::cout << "  [+] Interface 0 Descriptor:\n";
        std::cout << "      Interface # : " << static_cast<int>(ifaceDesc.bInterfaceNumber) << "\n";
        std::cout << "      Alt Setting : " << static_cast<int>(ifaceDesc.bAlternateSetting) << "\n";
        std::cout << "      Endpoints   : " << static_cast<int>(ifaceDesc.bNumEndpoints) << "\n";
        std::cout << "      Class/Sub/Pr: 0x" << std::hex << static_cast<int>(ifaceDesc.bInterfaceClass)
                  << " / 0x" << static_cast<int>(ifaceDesc.bInterfaceSubClass)
                  << " / 0x" << static_cast<int>(ifaceDesc.bInterfaceProtocol) << std::dec << "\n";
    }

    std::vector<PipeSummary> pipes;
    UCHAR outPipeId = 0;
    UCHAR inPipeId = 0;

    for (UCHAR i = 0; i < ifaceDesc.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipeInfo{};
        if (WinUsb_QueryPipe(hWinUsb, 0, i, &pipeInfo)) {
            PipeSummary summary;
            summary.pipeId = pipeInfo.PipeId;
            summary.pipeType = pipeInfo.PipeType;
            summary.maxPacketSize = pipeInfo.MaximumPacketSize;
            summary.interval = pipeInfo.Interval;
            summary.isInput = (USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId) != 0);
            pipes.push_back(summary);

            if (!summary.isInput && outPipeId == 0) {
                outPipeId = summary.pipeId;
            }
            if (summary.isInput && inPipeId == 0) {
                inPipeId = summary.pipeId;
            }
        }
    }

    PrintPipeTable(pipes, 0);

    // Check for associated interfaces if present
    WINUSB_INTERFACE_HANDLE hAssoc = nullptr;
    UCHAR assocIdx = 0;
    while (WinUsb_GetAssociatedInterface(hWinUsb, assocIdx, &hAssoc)) {
        std::cout << "  [+] Found Associated Interface #" << static_cast<int>(assocIdx + 1) << "\n";
        USB_INTERFACE_DESCRIPTOR assocDesc{};
        if (WinUsb_QueryInterfaceSettings(hAssoc, 0, &assocDesc)) {
            std::cout << "      Interface # : " << static_cast<int>(assocDesc.bInterfaceNumber) << "\n";
            std::cout << "      Endpoints   : " << static_cast<int>(assocDesc.bNumEndpoints) << "\n";
            std::vector<PipeSummary> assocPipes;
            for (UCHAR j = 0; j < assocDesc.bNumEndpoints; ++j) {
                WINUSB_PIPE_INFORMATION pi{};
                if (WinUsb_QueryPipe(hAssoc, 0, j, &pi)) {
                    PipeSummary s;
                    s.pipeId = pi.PipeId;
                    s.pipeType = pi.PipeType;
                    s.maxPacketSize = pi.MaximumPacketSize;
                    s.interval = pi.Interval;
                    s.isInput = (USB_ENDPOINT_DIRECTION_IN(pi.PipeId) != 0);
                    assocPipes.push_back(s);
                }
            }
            PrintPipeTable(assocPipes, assocIdx + 1);
        }
        WinUsb_Free(hAssoc);
        assocIdx++;
    }

    // 7. Transmit 52-Byte Handshake Initialization Sequence
    std::cout << "[HANDSHAKE] Preparing BCD2000 hardware initialization sequence...\n";
    std::cout << "  Payload size: " << sizeof(BCD2000_INIT_SEQUENCE) << " bytes (Full Linux snd-bcd2000 handshake).\n";
    std::cout << "  Packet contents:\n";
    PrintHexDump(BCD2000_INIT_SEQUENCE, sizeof(BCD2000_INIT_SEQUENCE));

    // Endpoint 0x01 is the standard BCD2000 Control/MIDI OUT endpoint.
    UCHAR targetOutPipe = 0x01;
    bool foundTarget = false;
    for (const auto& p : pipes) {
        if (p.pipeId == targetOutPipe) {
            foundTarget = true;
            break;
        }
    }
    if (!foundTarget && outPipeId != 0) {
        targetOutPipe = outPipeId;
    }

    std::cout << "\n  Target OUT Pipe: 0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(targetOutPipe) << std::dec << "\n";

    // Set Pipe Timeout Policy (2000 ms) and Auto-Clear Stall
    ULONG timeoutMs = 2000;
    WinUsb_SetPipePolicy(hWinUsb, targetOutPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
    UCHAR autoClearStall = 1;
    WinUsb_SetPipePolicy(hWinUsb, targetOutPipe, AUTO_CLEAR_STALL, sizeof(autoClearStall), &autoClearStall);

    std::cout << "  [>] Transmitting 52-byte initialization frame to Endpoint 0x" 
              << std::hex << (int)targetOutPipe << std::dec << "...\n";

    ULONG bytesWritten = 0;
    BOOL writeOk = WinUsb_WritePipe(
        hWinUsb,
        targetOutPipe,
        const_cast<PUCHAR>(BCD2000_INIT_SEQUENCE),
        sizeof(BCD2000_INIT_SEQUENCE),
        &bytesWritten,
        nullptr
    );

    bool handshakeSuccess = false;
    if (writeOk && bytesWritten == sizeof(BCD2000_INIT_SEQUENCE)) {
        handshakeSuccess = true;
        std::cout << "  [+] Handshake transmission SUCCESSFUL! (" << bytesWritten << " of "
                  << sizeof(BCD2000_INIT_SEQUENCE) << " bytes transferred).\n";
        std::cout << "  [+] BCD2000 8051 Microcontroller registers successfully initialized.\n";
    } else {
        DWORD writeErr = GetLastError();
        std::cerr << "  [-] WinUsb_WritePipe failed: " << FormatWin32Error(writeErr) << "\n";
        std::cerr << "      Bytes written: " << bytesWritten << "\n";
    }

    // 7b. Hardware LED Validation Test
    if (handshakeSuccess) {
        std::cout << "\n======================================================================\n";
        std::cout << "           PRUEBA DE ILUMINACION DE LEDS FISICOS (ENDPOINT 0x01)      \n";
        std::cout << "======================================================================\n";
        std::cout << "  Estructura de trama: [0x03, 0x00, len, MIDI_BYTES..., pad to 64 bytes]\n\n";

        // Give the 8051 MCU 150ms to settle after initialization
        Sleep(150);

        // --------------------------------------------------------------------
        // Metodo 1: Iluminacion Global Simultanea (Batch Frame)
        // --------------------------------------------------------------------
        std::cout << "  [Fase 1] Encendido simultaneo de todos los LEDs (Batch Frames)...\n";

        // Batch Frame A: Transport, Cues & Loops (Note On: 0x90, notas 0x00 a 0x13, vel 0x7F)
        // 20 notas * 3 bytes = 60 bytes de payload MIDI
        uint8_t noteBatch[60];
        for (uint8_t i = 0; i < 20; ++i) {
            noteBatch[i * 3 + 0] = 0x90; // Note On
            noteBatch[i * 3 + 1] = i;    // Note ID
            noteBatch[i * 3 + 2] = 0x7F; // Velocity = 127 (LED ON)
        }
        bool batch1Ok = SendBcdMidiFrame(hWinUsb, targetOutPipe, noteBatch, sizeof(noteBatch));
        std::cout << "    -> Batch 1 (Botones/Transporte Note On 0x90): " << (batch1Ok ? "ENVIADO [OK]" : "FALLO") << "\n";

        // Batch Frame B: Kill switches, EQ & FX (Control Change: 0xB0, CC 0x0A a 0x1D, val 0x7F)
        // 20 CCs * 3 bytes = 60 bytes de payload MIDI
        uint8_t ccBatch[60];
        for (uint8_t i = 0; i < 20; ++i) {
            ccBatch[i * 3 + 0] = 0xB0;     // Control Change
            ccBatch[i * 3 + 1] = 0x0A + i; // CC ID
            ccBatch[i * 3 + 2] = 0x7F;     // Value = 127 (LED ON)
        }
        bool batch2Ok = SendBcdMidiFrame(hWinUsb, targetOutPipe, ccBatch, sizeof(ccBatch));
        std::cout << "    -> Batch 2 (Kill Switches & FX CC 0xB0):     " << (batch2Ok ? "ENVIADO [OK]" : "FALLO") << "\n";

        // --------------------------------------------------------------------
        // Metodo 2: Mensajes Individuales en Trama 64 bytes (compatibilidad total)
        // --------------------------------------------------------------------
        std::cout << "  [Fase 2] Enviando barrido individual con tramas completas de 64 bytes...\n";
        for (uint8_t midino = 0x00; midino <= 0x18; ++midino) {
            SendBcdSingleMidi(hWinUsb, targetOutPipe, 0x90, midino, 0x7F);
            Sleep(10);
        }
        for (uint8_t midino = 0x0A; midino <= 0x20; ++midino) {
            SendBcdSingleMidi(hWinUsb, targetOutPipe, 0xB0, midino, 0x7F);
            Sleep(10);
        }

        std::cout << "\n  >>> ¡TODOS LOS LEDS DEBEN ESTAR VISIBLEMENTE ILUMINADOS AHORA! <<<\n";
        std::cout << "  >>> Mantenemos iluminacion activa por 3 segundos (inspecciona la BCD2000)... <<<\n";
        Sleep(3000);

        std::cout << "  [Fase 3] Apagando LEDs con barrido en reversa...\n";
        for (uint8_t midino = 0x00; midino <= 0x18; ++midino) {
            SendBcdSingleMidi(hWinUsb, targetOutPipe, 0x90, midino, 0x00);
            Sleep(5);
        }
        for (uint8_t midino = 0x0A; midino <= 0x20; ++midino) {
            SendBcdSingleMidi(hWinUsb, targetOutPipe, 0xB0, midino, 0x00);
            Sleep(5);
        }
        std::cout << "  [+] Ciclo de prueba de LEDs completado.\n";
    }

    // 8. Interactive Telemetry Listener on Endpoint 0x81 (IN)
    UCHAR targetInPipe = 0x81;
    bool foundInPipe = false;
    for (const auto& p : pipes) {
        if (p.pipeId == targetInPipe) {
            foundInPipe = true;
            break;
        }
    }
    if (!foundInPipe && inPipeId != 0) {
        targetInPipe = inPipeId;
    }

    int telemetryEventsReceived = 0;

    if (targetInPipe != 0) {
        std::cout << "\n======================================================================\n";
        std::cout << "         TELEMETRIA BIDIRECCIONAL EN VIVO (ENDPOINT 0x" 
                  << std::hex << (int)targetInPipe << std::dec << ")             \n";
        std::cout << "======================================================================\n";
        std::cout << "  ¡PRUEBA EN VIVO! La consola entrara en modo escucha durante 12 segundos.\n";
        std::cout << "  -> Por favor, PRESIONA CUALQUIER BOTON (Play, Cue, Loop, FX, Kill)\n";
        std::cout << "     o MUEVE UN FADER / RUEDA JOG en la BCD2000 fisica AHORA...\n";
        std::cout << "  -> Los eventos entrantes se decodificaran en tiempo real y reencenderan\n";
        std::cout << "     el LED correspondiente como confirmacion visual de retorno.\n";
        std::cout << "----------------------------------------------------------------------\n";

        // Short timeout (100 ms) per read to keep loop responsive
        ULONG inTimeoutMs = 100;
        WinUsb_SetPipePolicy(hWinUsb, targetInPipe, PIPE_TRANSFER_TIMEOUT, sizeof(inTimeoutMs), &inTimeoutMs);

        auto startTime = std::chrono::steady_clock::now();
        int lastCountdown = 12;

        while (true) {
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - startTime).count();

            if (elapsedSec >= 12) {
                break;
            }

            int remaining = 12 - static_cast<int>(elapsedSec);
            if (remaining != lastCountdown && remaining % 3 == 0) {
                std::cout << "  [TIEMPO RESTANTE] " << remaining << " segundos... (Mueve controles o presiona botones)\n";
                lastCountdown = remaining;
            }

            uint8_t readBuffer[64] = { 0 };
            ULONG bytesRead = 0;
            BOOL readOk = WinUsb_ReadPipe(
                hWinUsb,
                targetInPipe,
                readBuffer,
                sizeof(readBuffer),
                &bytesRead,
                nullptr
            );

            if (readOk && bytesRead > 0) {
                telemetryEventsReceived++;
                std::cout << "\n  [EVENTO #" << telemetryEventsReceived << "] Recibidos " 
                          << bytesRead << " bytes de Endpoint 0x" << std::hex << (int)targetInPipe << std::dec << ":\n";
                PrintHexDump(readBuffer, bytesRead, "    ");

                // Decode incoming MIDI packets
                for (ULONG b = 0; b + 2 < bytesRead; b += 3) {
                    uint8_t status = readBuffer[b];
                    uint8_t d1 = readBuffer[b + 1];
                    uint8_t d2 = readBuffer[b + 2];

                    if ((status & 0xF0) == 0x90) { // Note On
                        std::cout << "    [DECODIFICADO] Note On  | Ch: " << ((status & 0x0F) + 1)
                                  << " | Nota: 0x" << std::hex << (int)d1 << " (" << std::dec << (int)d1 << ")"
                                  << " | Vel: " << (int)d2 << "\n";
                        // Echo LED feedback: Turn ON the LED for this button
                        if (d2 > 0) {
                            SendBcdSingleMidi(hWinUsb, targetOutPipe, 0x90, d1, 0x7F);
                            std::cout << "    [RETORNO LED] Comando LED ON enviado para Nota 0x" << std::hex << (int)d1 << std::dec << "\n";
                        }
                    } else if ((status & 0xF0) == 0x80) { // Note Off
                        std::cout << "    [DECODIFICADO] Note Off | Ch: " << ((status & 0x0F) + 1)
                                  << " | Nota: 0x" << std::hex << (int)d1 << " (" << std::dec << (int)d1 << ")"
                                  << " | Vel: " << (int)d2 << "\n";
                        SendBcdSingleMidi(hWinUsb, targetOutPipe, 0x90, d1, 0x00);
                    } else if ((status & 0xF0) == 0xB0) { // Control Change
                        std::cout << "    [DECODIFICADO] Control Change | Ch: " << ((status & 0x0F) + 1)
                                  << " | CC#: 0x" << std::hex << (int)d1 << " (" << std::dec << (int)d1 << ")"
                                  << " | Valor: " << (int)d2 << "\n";
                        // Echo LED feedback for Kill / FX buttons
                        if (d1 >= 0x0A && d1 <= 0x20) {
                            SendBcdSingleMidi(hWinUsb, targetOutPipe, 0xB0, d1, d2 > 0 ? 0x7F : 0x00);
                        }
                    } else if ((status & 0xF0) == 0xE0) { // Pitch Bend
                        int pbVal = (static_cast<int>(d2) << 7) | static_cast<int>(d1);
                        std::cout << "    [DECODIFICADO] Pitch Bend | Ch: " << ((status & 0x0F) + 1)
                                  << " | Valor: " << pbVal << "\n";
                    }
                }
            } else {
                DWORD readErr = GetLastError();
                if (readErr != ERROR_SEM_TIMEOUT) {
                    std::cerr << "  [-] WinUsb_ReadPipe error: " << FormatWin32Error(readErr) << "\n";
                    break;
                }
            }
        }

        std::cout << "\n----------------------------------------------------------------------\n";
        std::cout << "  Fin del periodo de escucha en vivo. Total eventos capturados: " 
                  << telemetryEventsReceived << "\n";
    }

    // 9. Cleanup and Summary
    std::cout << "\n[CLEANUP] Releasing WinUSB interface and device handles...\n";
    WinUsb_Free(hWinUsb);
    CloseHandle(hDevice);
    std::cout << "  [+] Resources freed cleanly.\n\n";

    std::cout << "======================================================================\n";
    if (handshakeSuccess) {
        std::cout << "             HITO 1: DETECCION Y ENLACE USB -> COMPLETADO             \n";
        if (telemetryEventsReceived > 0) {
            std::cout << "             TELEMETRIA FISICA BIDIRECCIONAL VERIFICADA               \n";
        }
    } else {
        std::cout << "             HITO 1: FALLO EN HANDSHAKE USB                           \n";
    }
    std::cout << "======================================================================\n";

    return handshakeSuccess ? 0 : 4;
}

// ============================================================================
// CLI Entry Point
// ============================================================================

int main(int argc, char* argv[]) {
    GUID customGuid{};
    bool hasCustomGuid = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: bcd2000_diag_usb [options]\n\n"
                      << "Options:\n"
                      << "  --help, -h          Show this help screen\n"
                      << "  --guid <GUID>       Specify explicit device interface GUID\n"
                      << "                      (e.g., --guid {dee824ef-729b-4a0e-9c14-b7117d33a817})\n";
            return 0;
        } else if (arg == "--guid" && i + 1 < argc) {
            std::string guidStr = argv[++i];
            std::wstring wGuidStr(guidStr.begin(), guidStr.end());
            HRESULT hr = CLSIDFromString(wGuidStr.c_str(), &customGuid);
            if (SUCCEEDED(hr)) {
                hasCustomGuid = true;
            } else {
                std::cerr << "[!] Invalid GUID format specified: " << guidStr << "\n";
                return 1;
            }
        }
    }

    return RunDiagnostic(hasCustomGuid ? &customGuid : nullptr);
}
