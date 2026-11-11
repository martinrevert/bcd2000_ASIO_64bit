[![EN](flags/gb.svg) English](../../TECHNICS.md) | [![ES](flags/es.svg) Español](TECHNICS.es.md) | [![PT](flags/br.svg) Português](TECHNICS.pt.md) | [![DE](flags/de.svg) Deutsch](TECHNICS.de.md) | [![FR](flags/fr.svg) Français](TECHNICS.fr.md) | [![ZH](flags/cn.svg) **中文**](TECHNICS.zh.md) | [![JA](flags/jp.svg) 日本語](TECHNICS.ja.md)

# 技术参考、系统架构与开发者指南 (`TECHNICS.zh.md`)

**Behringer BCD2000 64 位用户态 ASIO 与 WinMM MIDI 驱动套件的深度工程文档。**

[用户指南 (README.zh.md)](README.zh.md) · [历史验证蓝图](../ROADMAP_VERIFICATION.md) · [AI 代理守则 (AGENTS.md)](../../AGENTS.md)

---

## 1. 系统概览与核心设计决策

**Behringer BCD2000**（`VID 0x1397`，`PID 0x00BD`）是一款基于 **8051 微控制器**、**TI TUSB3200** USB 流控制器和 **Cirrus Logic CS4220** 编解码器的 **USB 1.1 全速（Full-Speed, 12 Mbps）** 设备。其出厂固件暴露了专有 USB 描述符，**不兼容**标准 USB Audio Class (UAC) 或 USB-MIDI 规范。

本项目没有编写需要昂贵 EV/WHQL 签名的内核态 `.sys` 驱动，而是在微软系统内置的 `WinUSB.sys` 之上实现了 **100% 用户态（User-Mode）64 位驱动架构**：

* **无内核签名障碍：** 用户态 DLL 在 Windows 10/11 x64 上不需要 EV/WHQL 内核证书，也无需开启测试签名模式。
* **零外部运行库依赖：** 严格面向 `x64`（`C++17`）编译，并在 [`CMakeLists.txt`](../../CMakeLists.txt) 中采用静态 MSVC 运行库（`/MT`），确保 `bcd2000_asio.dll` 和 `bcd2000_midi.dll` 在任何宿主 DAW 进程中都不会因缺少 `VCRUNTIME140.dll` / `MSVCP140.dll` 而加载失败。
* **USB 1.1 带宽管理：** Full-Speed 总线（12 Mbps）对同步传输调度限制严格。输出优先保证 4 通道 16 位 PCM（`ASIOSTInt16LSB`，44.1 kHz：Ch 1-2 主输出 RCA + Ch 3-4 耳机 Cue），通过原子环形缓冲区与宿主 ASIO 缓冲区大小解耦，并利用硬件时钟跟踪消除爆音。

### 关键标识符与端点

| 参数 | 值 / 实现方式 |
| :--- | :--- |
| **USB 硬件 ID** | `VID_1397&PID_00BD` |
| **WinUSB 设备接口 GUID** | `{DEE824EF-729B-4A0E-9C14-B7117D33A817}` |
| **ASIO 驱动名称 / CLSID** | `Behringer BCD2000 ASIO 64-bit` / `{B2D20000-1397-00BD-A510-000000000064}` |
| **WinMM MIDI 设备名称** | `Behringer BCD2000 MIDI`（安装于 `System32` 与 `Drivers32`） |
| **音频端点** | EP `0x02` 同步 OUT（4 通道 S16_LE @ 44.1 kHz），EP `0x83` 同步 IN（用于隐式 DAC 时钟跟踪） |
| **控制 / MIDI 端点** | EP `0x01` OUT（64 字节封装帧，LED 与初始化），EP `0x81` IN（Running-Status MIDI 事件） |
| **IPC 命名管道** | `\\.\pipe\BCD2000_ASIO_MIDI_V1`（双向 MIDI 桥接），`\\.\pipe\BCD2000_MIDI_CTRL_V1`（WinUSB 热交接 `'R' -> 'K'`） |

---

## 2. 驱动架构图

```mermaid
flowchart TB
    subgraph HOST["64 位宿主应用 (Mixxx, Ableton Live, REAPER, Traktor)"]
        DAW_AUDIO["ASIO 宿主客户端 (IASIO COM)"]
        DAW_MIDI["WinMM MME MIDI 客户端 (midiIn / midiOut)"]
    end

    subgraph USERMODE["64 位用户态驱动层 (C++17 /MT)"]
        subgraph ASIO_DLL["bcd2000_asio.dll (进程内 COM 服务器)"]
            IASIO_API["Bcd2000AsioDriver (IASIO)<br/>4 In / 4 Out · ASIOSTInt16LSB · 64..4096"]
            RING["32,768 帧交织环形缓冲区"]
            ASIO_THREAD["ASIO 宿主回调线程<br/>(MMCSS Pro Audio)"]
            USB_AUDIO_THREAD["USB 同步流传输线程<br/>(WinUsb_WriteIsochPipeAsap)"]
            CLK_TRACK["EP 0x83 IN 隐式时钟跟踪器<br/>(消除 USB 1.1 晶振漂移)"]
            PIPE_SRV["MIDI 桥接服务器<br/>\\.\pipe\BCD2000_ASIO_MIDI_V1"]
        end

        subgraph MIDI_DLL["bcd2000_midi.dll (System32 / Drivers32)"]
            WINMM["DriverProc / midMessage / modMessage"]
            DUAL["双模式传输引擎<br/>Mode A: ASIO 管道客户端<br/>Mode B: 独立 WinUSB + 控制管道"]
            PARSER["MidiRunningStatusParser"]
        end
    end

    subgraph KERNEL["Windows 内核与驱动绑定"]
        WINUSB["WinUSB.sys (微软系统内置)<br/>通过本地 SHA-256 .cat/.inf 绑定<br/>(bcd2000_winusb_setup.exe)"]
    end

    subgraph HW["Behringer BCD2000 硬件 (VID 0x1397 / PID 0x00BD · USB 1.1)"]
        EP_AUDIO["音频同步管道<br/>EP 0x02 OUT · EP 0x83 IN"]
        EP_CTRL["MIDI / 控制管道<br/>EP 0x01 OUT (64B 帧) · EP 0x81 IN"]
    end

    DAW_AUDIO <-->|"COM bufferSwitch"| IASIO_API
    IASIO_API <--> ASIO_THREAD
    ASIO_THREAD <--> RING
    RING <--> USB_AUDIO_THREAD
    CLK_TRACK -.->|"DAC 实际帧率 (Frames/ms)"| USB_AUDIO_THREAD

    DAW_MIDI <-->|"MME 回调"| WINMM
    WINMM <--> PARSER
    PARSER <--> DUAL
    DUAL <-->|"Mode A: 双向 MIDI"| PIPE_SRV
    IASIO_API -.->|"热交接 Mode B -> A ('R'->'K')<br/>\\.\pipe\BCD2000_MIDI_CTRL_V1"| DUAL

    USB_AUDIO_THREAD -->|"同步 URB"| WINUSB
    CLK_TRACK <--|"EP 0x83 ReadIsoch"| WINUSB
    PIPE_SRV <-->|"EP 0x01 / 0x81 重叠 I/O"| WINUSB
    DUAL <-->|"Mode B (ASIO 空闲时)"| WINUSB

    WINUSB <--> EP_AUDIO
    WINUSB <--> EP_CTRL
```

---

## 3. 开发历程：分阶段工程演进

本项目从 [`docs/ROADMAP_VERIFICATION.md`](../ROADMAP_VERIFICATION.md) 中最初定义的渐进式验证路线图演进而来，现已扩展为完整的生产级驱动套件。每个阶段都在独立诊断工具中隔离验证子系统后再整合进最终 DLL。

### 阶段 1 — USB 枚举与激活握手（`bcd2000_diag_usb.exe`）
* **挑战：** BCD2000 在 8051 MCU 通过端点 `0x01` 收到特定的 52 字节专有握手包前处于静默状态。
* **实现：** 使用 `SetupDiGetClassDevsW` 匹配 `kWinUsbGenericGuid` 与 `vid_1397` / `pid_00bd`，完成 `WinUsb_Initialize` 并发送 52 字节 `kInitSequence`（源自 Linux `snd-bcd2000`）。
* **验证：** 管道正常列举，控制台初次点亮 LED。

### 阶段 2 — 控件遥测、Running Status 与双向 LED 引擎（`bcd2000_diag_midi.exe`）
* **挑战：** EP `0x81` IN 在 64 字节 USB 包中传输变长 **MIDI Running Status** 数据；而 EP `0x01` OUT 发送 LED 状态必须封装 `[0x03, 0x00, len, ...]` 帧头且不能阻塞并发读取。
* **实现：** 在 [`src/common/bcd2000_usb_protocol.h`](../../src/common/bcd2000_usb_protocol.h) 中构建 `MidiRunningStatusParser` 与 `SendBcdMidiOverlapped`，验证所有推子、旋钮、搓盘、按键及 LED CC（`0xB0`, `CC 0x00..0x19`）。

### 阶段 3 — 同步 PCM 传输与正弦波测试（`bcd2000_diag_audio.exe`）
* **挑战：** 在 Windows 线程调度抖动下，通过 USB 1.1 持续传输 4 通道 16 位 PCM（`8 bytes/frame` @ 44.1 kHz）而不发生欠载断音。
* **实现：** 构建多 URB 重叠传输管线（`360 bytes/ms` 上限，每 URB `16` 包，`6` 个并发 URB），提升至 **MMCSS `"Pro Audio"`**（`avrt.dll` / `AVRT_PRIORITY_CRITICAL`），并通过 440 Hz 纯正弦波在 Master RCA 与耳机输出完成验证。

### 阶段 4 — 64 位 COM ASIO 服务器与隐式时钟跟踪（`bcd2000_asio.dll`、`hostsample.exe`）
* **挑战：** DAW 期望 2 的幂次缓冲（`64..4096`），而 USB 1.1 以 1 ms 帧工作（`44.1 frames/ms`）；固定或理论分频发送会与物理 DAC 晶振产生漂移，引发约 2–3 Hz 的周期性咔哒声。
* **实现：** 实现 64 位进程内 COM 服务器（`Bcd2000AsioDriver`，[`src/asio/dllmain_registry.cpp`](../../src/asio/dllmain_registry.cpp)）、**32,768 帧环形缓冲区**，并新增 **EP `0x83` IN 隐式时钟跟踪线程**（`WinUsb_ReadIsochPipeAsap`），实时测量硬件 `frames/ms` 并动态调节 `44`/`45` 帧包长。

### 阶段 5 — 64 位 WinMM MIDI 驱动与双模式无缝热交接（`bcd2000_midi.dll`、`hostsample_midi.exe`）
* **挑战：** `WinUSB.sys` 仅允许单个句柄**独占**访问设备。
* **实现：** 在 [`src/midi/bcd2000_midi_driver.cpp`](../../src/midi/bcd2000_midi_driver.cpp) 与 [`src/asio/bcd2000_asio_driver.cpp`](../../src/asio/bcd2000_asio_driver.cpp) 之间设计 **Dual-Mode 传输**：**Mode A** 连接 `\\.\pipe\BCD2000_ASIO_MIDI_V1`，**Mode B** 独立持有 WinUSB 并在 ASIO 启动时通过 `\\.\pipe\BCD2000_MIDI_CTRL_V1` 执行 `'R' -> 'K'` 热交接，**无需关闭 DAW 中已打开的 WinMM 句柄**。

### 阶段 6 — 免 Zadig 安装器、NSIS 打包与 Mixxx 安全预设
* **免 Zadig WinUSB 绑定（`bcd2000_winusb_setup.exe`）：** [`tools/bcd2000_winusb_setup.cpp`](../../tools/bcd2000_winusb_setup.cpp) 通过 `wintrust.dll` 生成并用本地 2048 位自签名证书签署 SHA-256 `.inf`/`.cat`，再调用 `DiInstallDriverW` 绑定。
* **单文件 NSIS 安装器（`Behringer_BCD2000_Setup_x64.exe`）：** [`installer/bcd2000_setup.nsi`](../../installer/bcd2000_setup.nsi) 一键完成 WinUSB 绑定、ASIO COM 注册、`System32\bcd2000_midi.dll` 注册、工具部署与卸载支持。
* **Mixxx 安全预设（`presets/mixxx/`）：** [`Behringer-BCD2000-safe-scripts.js`](../../presets/mixxx/Behringer-BCD2000-safe-scripts.js) 移除了启动全量旋钮查询（`0xB0 0x64 0x7F`）并增加逐 CC *soft-takeover*。

---

## 4. 配置编译环境与构建测试

### 环境要求 (Bill of Materials)
1. **操作系统：** Windows 10 或 Windows 11 (x64)。
2. **编译器：** Visual Studio 2022（或 Build Tools 2022），包含 **使用 C++ 的桌面开发** 组件（`cl.exe` 目标 **x64**，C++17）。
3. **Windows SDK：** `10.0.19041.0` 或更高版本（含 `winusb`、`setupapi`、`avrt`、`wintrust`、`crypt32`、`newdev`、`winmm`、`ole32`）。
4. **构建工具：** CMake `3.20` 或更高版本。
5. **第三方 SDK：** Steinberg ASIO SDK 2.3 置于 `third_party/asio-sdk/`。
6. **打包工具（可选）：** NSIS 3.x（位于 `.tools/nsis/nsis-3.13/Bin` 或系统 `PATH`），用于构建 `bcd2000_installer` 目标。

### 配置与编译 (PowerShell)

```powershell
# 1. 配置为 64 位（CMakeLists.txt 严格校验 x64 架构）
cmake -B build -A x64

# 2. 编译所有驱动 DLL 与诊断工具（Release 或 RelWithDebInfo）
cmake --build build --config Release

# 3. (可选) 构建单文件 NSIS 安装程序
cmake --build build --config Release --target bcd2000_installer
```

构建产物存放于 `build\Release\`（或 `build\RelWithDebInfo\`）。

### 本地注册与测试

```powershell
# 注册 COM ASIO DLL（需要管理员权限终端）
.\scripts\install_driver.ps1 -Config Release

# 运行各阶段硬件/宿主测试工具
.\build\Release\bcd2000_diag_usb.exe
.\build\Release\bcd2000_diag_midi.exe
.\build\Release\bcd2000_diag_audio.exe --frequency 440 --duration 10
.\third_party\asio-sdk\host\pc\hostsample.exe
.\build\Release\hostsample_midi.exe

# 注销 COM ASIO DLL
.\scripts\uninstall_driver.ps1 -Config Release
```

### 运行时诊断与常见问题排查
* **实时跟踪日志：** `bcd2000_asio.dll` 与 `bcd2000_midi.dll` 会通过 `bcd2000::TraceLog` 写入带时间戳的日志（`pid`, `tid`, tag）至 **`%TEMP%\bcd2000_trace.log`**。
* **USB 数据包分析：** 使用 **Wireshark + USBPcap** 检查 EP `0x01` / `0x81` 控制帧及 EP `0x02` / `0x83` 同步 URB。
* **常见解决措施：**
  * *WinUSB 管道打开失败：* 以管理员身份重新执行 `bcd2000_winusb_setup.exe` 并重新插拔 USB 连接线。
  * *USB 1.1 总线饱和或声音断续：* 连接至主板原生 USB 口（避免共享集线器），将缓冲区大小设置为 `64` 的倍数（推荐 `256` 或 `512`）。
  * *无推子/按键输入事件：* 确认 52 字节握手包已成功发送（`bcd2000_diag_usb.exe`）。

---

## 5. 开发者与贡献者守则

热烈欢迎提交 Pull Request、缺陷报告、各种 USB 主控与 Windows 版本的硬件测试，以及新型 DAW/DJ 脚本映射！为保持驱动的磐石级稳定性，请遵循以下规范：

### 架构不变性原则
1. **严格限定 64 位用户态：** 禁止引入 `.sys` 内核驱动或 32 位构建目标（`CMakeLists.txt` 会自动中止非 64 位构建）。
2. **保持静态 CRT（`/MT`）：** 严禁将 `CMAKE_MSVC_RUNTIME_LIBRARY` 改为动态库 `/MD`。外部宿主加载的 DLL 必须自包含。
3. **实时音频线程安全：** 在 `AsioHostThreadMain` 与 `UsbStreamingThreadMain` 中严禁堆内存分配、与 UI/IPC 线程争用的阻塞互斥锁，以及同步文件 I/O。
4. **单一协议真源：** 所有 VID/PID、`kInitSequence`、管道名称、`MidiRunningStatusParser` 与数据帧辅助函数必须集中维护于 [`src/common/bcd2000_usb_protocol.h`](../../src/common/bcd2000_usb_protocol.h)。
5. **保护双模式热交接：** 任何 WinUSB 句柄改动都必须严格保持 `'R' -> 'K'` 协议与 Mode A/B 状态切换，且不得破坏宿主已打开的 WinMM 句柄。

### 代码规范与 PR 检查清单
* **严格 C++17 标准：** 在 `/W4 /permissive- /utf-8` 下保持零新增编译器警告。
* **必要构建验证：** 在发起 PR 前确保 `RelWithDebInfo` 与 `Release`（x64）均编译通过。
* **功能验证测试：**
  * ASIO 变更：使用 `hostsample.exe` 与至少一款真实 64 位 DAW（REAPER、Ableton Live 或 Mixxx）测试。
  * WinMM MIDI 变更：使用 `hostsample_midi.exe` 测试并发 ASIO+MIDI 启动与关闭切换。
* **多语言文档同步：** 任何用户可见行为、构建步骤或预设变更，必须同步更新 [`README.zh.md`](README.zh.md) / [`TECHNICS.zh.md`](TECHNICS.zh.md)（以及 `docs/i18n/` 下的所有语言版本）。
* **AI 代理守则：** 自动化助手与 AI 编码代理必须严格遵守 [`AGENTS.md`](../../AGENTS.md)。

---

[![EN](flags/gb.svg) English](../../TECHNICS.md) | [![ES](flags/es.svg) Español](TECHNICS.es.md) | [![PT](flags/br.svg) Português](TECHNICS.pt.md) | [![DE](flags/de.svg) Deutsch](TECHNICS.de.md) | [![FR](flags/fr.svg) Français](TECHNICS.fr.md) | [![ZH](flags/cn.svg) **中文**](TECHNICS.zh.md) | [![JA](flags/jp.svg) 日本語](TECHNICS.ja.md)
