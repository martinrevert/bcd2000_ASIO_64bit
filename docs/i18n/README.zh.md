[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) Español](README.es.md) | [![PT](flags/br.svg) Português](README.pt.md) | [![DE](flags/de.svg) Deutsch](README.de.md) | [![FR](flags/fr.svg) Français](README.fr.md) | [![ZH](flags/cn.svg) **中文**](README.zh.md) | [![JA](flags/jp.svg) 日本語](README.ja.md)

# Behringer BCD2000 — 原生 64 位 ASIO 与 MIDI 驱动 (Windows 10/11)

**一套全新的开源 64 位驱动套件，让经典的 Behringer BCD2000 DJ 控制器在现代 64 位 DAW 和 DJ 软件上重获新生。**

[技术文档与架构说明 (TECHNICS.zh.md)](TECHNICS.zh.md)


---

## 欢迎回来，BCD2000！

如果你桌上还放着一台 **Behringer B-Control Deejay BCD2000**，或者它一直躺在你的航空箱里吃灰，你一定懂得那种既可惜又无奈的心情：这台机器的硬件做工依然像坦克一样扎实——推子、EQ 旋钮、搓盘（Jog Wheel）、耳机放大器和 RCA 接口全部完好如初——但现代电脑却只能把它当成一块砖头。

**今天，这一切结束了。** 本项目为 BCD2000 带来了 Behringer 官方从未发布过的东西：**专为 Windows 10 和 Windows 11 (x64) 从零构建的真正原生 64 位 ASIO 音频驱动与 64 位 MIDI 驱动**。

插上 USB 线，运行一个安装程序，即可在 **Mixxx**、**Ableton Live**、**REAPER**、**FL Studio**、**Traktor** 等现代 64 位软件中重新畅快使用你的 BCD2000。

---

## 缘起与项目初衷

Behringer 于 2000 年代中期（Windows XP / Vista 时代）推出了 BCD2000，但**官方驱动支持在 32 位 Windows 时代便停止更新**。与标准的 USB Audio Class 或 USB-MIDI 免驱设备不同，BCD2000 内部的 8051 微控制器使用了专有协议（`VID 0x1397`，`PID 0x00BD`）。没有专用驱动，Windows 10 和 Windows 11 根本无法与它通信。

多年来，老用户只能借助各种折腾的权宜之计：保留一台老旧的 32 位 Windows XP/7 电脑、搭建不稳定的虚拟机，或者忍痛放弃一台完全正常的控制器。

这个项目的初衷非常简单：**优秀的硬件不应该仅仅因为厂商停止支持就沦为电子垃圾。** 借助开源 Linux ALSA 驱动（`snd-bcd2000`）积累的逆向工程成果，以及现代 Windows 用户态 USB API，我们从零构建了一套干净、可靠的 64 位 Windows 驱动栈，让 BCD2000 社区能够在现代电脑上继续混音与创作。

---

## 为什么这不是“在 Windows 7 兼容模式下运行旧驱动”

大家最常问的一个问题是：*“为什么不直接用 Windows 7 兼容模式运行 Behringer 的旧安装包呢？”*

事实很明确，这在技术上是行不通的：

1. **兼容模式对内核驱动无效。** Windows 兼容模式只能调整旧版用户态应用程序（`.exe`）的行为。64 位 Windows 内核**绝对无法加载 32 位 `.sys` 内核驱动**——没有任何开关或兼容层可以做到这一点。
2. **64 位 DAW 无法加载 32 位 ASIO DLL。** 现代音频与 DJ 软件都是原生 64 位进程，必须使用原生的 64 位 COM ASIO 库（`x64`）和原生 64 位 Windows MIDI 端口。
3. **本驱动不包含任何 Behringer 旧版二进制文件。** 我们**没有**修补或封装 2006 年的废弃软件。这是**100% 原创的 64 位 C++ 代码**，安全运行在 Windows 用户态，底层直接使用微软系统自带的 **`WinUSB.sys`** 驱动。无需任何内核破解，无需禁用驱动程序强制签名，也无需开启测试签名模式。

---

## 你将获得的核心功能（基础概览）

* **原生 64 位 ASIO 音频（`Behringer BCD2000 ASIO 64-bit`）：**
  * 4 路输出通道：**主输出左/右（背部 RCA）** + **耳机监听左/右（前置 1/4" TRS Cue）**，支持 44.1 kHz / 16-bit PCM。
  * 通过 ASIO 暴露 4 路输入通道（`In 1–4`）。
  * 低延迟实时音频流，并带硬件晶振时钟跟踪，确保音质纯净、无爆音。
* **同步运行的 64 位 MIDI IN/OUT（`Behringer BCD2000 MIDI`）：**
  * 完整支持推子、旋钮、搓盘、按键以及**双向 LED 指示灯反馈**——与 ASIO 音频**同时工作**，不存在独占设备冲突。
* **一键安装包（`Behringer_BCD2000_Setup_x64.exe`）：**
  * 自动为 BCD2000 绑定 Windows 原生 `WinUSB` 驱动（**无需**手动下载或配置 Zadig）。
  * 自动注册 64 位 ASIO 驱动与 64 位 MIDI 驱动，并提供标准卸载程序。
* **内置 Mixxx 安全预设（`Behringer BCD2000 (Driver Safe)`）：**
  * 适用于 Mixxx 2.x 的可选映射，增加 *soft-takeover*（软接管）机制，防止物理 EQ 或 Master 旋钮停留在 0 位时一打开 Mixxx 就把声音静音。

---

## 快速上手指南

### 系统要求
* 运行 **Windows 10 或 Windows 11（64 位 / x64）** 的电脑。
* 一台 **Behringer BCD2000**（建议直连主板 USB 接口，避免使用无源 USB 集线器）。

### 安装步骤
1. **关闭**所有正在运行的 DAW 或 DJ 软件（Mixxx、Ableton Live、REAPER 等）。
2. 以管理员身份运行 **`Behringer_BCD2000_Setup_x64.exe`** 并按向导完成安装。
   * *(可选)* 如果你使用 Mixxx，请勾选安装 Mixxx 安全预设。
3. 如果安装过程中 BCD2000 已插在电脑上，请**拔下并重新插上一次 USB 线**，让 Windows 干净地应用新的驱动绑定。
4. 打开你的 DAW 或 DJ 软件，选择：
   * **音频设备 (ASIO)：** `Behringer BCD2000 ASIO 64-bit`
   * **MIDI 控制器 (IN & OUT)：** `Behringer BCD2000 MIDI`
5. **Mixxx 用户提示：** 建议选用安装程序自带的 **`Behringer BCD2000 (Driver Safe)`** 映射。如果使用 Mixxx 自带的旧预设，请在启用控制器前先把物理 EQ 旋钮（Low / Mid / High）拧到中间位置。

---

## 面向开发者与贡献者

想查看内部架构图、了解我们如何解决 USB 1.1 晶振时钟漂移与 WinUSB 音频+MIDI 共享、从源码编译，或参与代码贡献？

👉 请阅读完整的技术规格文档：**[TECHNICS.zh.md](TECHNICS.zh.md)**。
🤖 AI 助手与自动化编码代理必须遵循 **[AGENTS.md](../../AGENTS.md)**。

---

[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) Español](README.es.md) | [![PT](flags/br.svg) Português](README.pt.md) | [![DE](flags/de.svg) Deutsch](README.de.md) | [![FR](flags/fr.svg) Français](README.fr.md) | [![ZH](flags/cn.svg) **中文**](README.zh.md) | [![JA](flags/jp.svg) 日本語](README.ja.md)
