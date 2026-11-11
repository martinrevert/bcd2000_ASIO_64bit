<div align="center">

🇬🇧 **English** | [🇪🇸 Español](docs/i18n/README.es.md) | [🇧🇷 Português](docs/i18n/README.pt.md) | [🇩🇪 Deutsch](docs/i18n/README.de.md) | [🇫🇷 Français](docs/i18n/README.fr.md) | [🇨🇳 中文](docs/i18n/README.zh.md) | [🇯🇵 日本語](docs/i18n/README.ja.md)

# Behringer BCD2000 — Native 64-bit ASIO & MIDI Driver (Windows 10/11)

**A brand-new, open-source 64-bit driver suite that brings the classic Behringer BCD2000 DJ controller back to life on modern 64-bit DAWs and DJ software.**

[Technical Documentation & Architecture (TECHNICS.md)](TECHNICS.md)

</div>

---

## Welcome Back, BCD2000!

If you still have a **Behringer B-Control Deejay BCD2000** sitting on your desk or packed away in a flight case, you probably know the bittersweet feeling: the hardware still works like a tank—its faders, EQ pots, jog wheels, headphone preamp, and RCA outputs are all intact—yet modern computers treated it like a paperweight.

**Today that changes.** This project gives the BCD2000 something Behringer never released: a **true, native 64-bit ASIO audio driver and 64-bit MIDI driver** built from the ground up for **Windows 10 and Windows 11 (x64)**.

Plug it in, install one package, and use your BCD2000 again in modern 64-bit applications like **Mixxx**, **Ableton Live**, **REAPER**, **FL Studio**, **Traktor**, and more.

---

## Where We Come From & Why This Initiative Exists

Behringer launched the BCD2000 in the mid-2000s during the Windows XP / Vista era and **stopped official driver development at 32-bit Windows drivers**. Unlike standard class-compliant USB audio or USB-MIDI devices, the BCD2000 uses a proprietary 8051 microcontroller protocol (`VID 0x1397`, `PID 0x00BD`). Without a dedicated driver, Windows 10 and Windows 11 simply cannot talk to it.

For years, owners were stuck with awkward workarounds: keeping an old 32-bit Windows XP/7 machine around, spinning up fragile virtual machines, or giving up on a completely functional controller.

This initiative was born from a simple conviction: **great hardware shouldn't become e-waste just because its vendor walked away.** Thanks to the reverse-engineering foundations laid by the open-source Linux ALSA driver (`snd-bcd2000`) and modern Windows user-mode USB APIs, we built a clean, reliable 64-bit Windows driver stack from scratch so the BCD2000 community can keep mixing and producing on modern machines.

---

## Why This Is NOT "Old Drivers in Windows 7 Compatibility Mode"

A very common question is: *"Why couldn't we just run the old Behringer installer in Windows 7 Compatibility Mode?"*

Here is the plain truth:

1. **Compatibility Mode does not work on kernel drivers.** Windows Compatibility Mode only adjusts behavior for old user-mode applications (`.exe`). A 64-bit Windows kernel strictly **cannot load 32-bit `.sys` kernel drivers**—there is no switch or shim that makes a 32-bit driver run inside a 64-bit OS kernel.
2. **64-bit DAWs cannot load 32-bit ASIO DLLs.** Modern audio and DJ applications are native 64-bit processes. They require a native 64-bit COM ASIO library (`x64`) and native 64-bit Windows MIDI ports.
3. **Zero legacy Behringer binaries are used here.** This project does **not** patch, wrap, or repackage Behringer's abandoned 2006 software. It is **100% original 64-bit C++ code** running safely in Windows user mode on top of Microsoft's own built-in **`WinUSB.sys`** driver. No kernel hacks, no disabling Driver Signature Enforcement, and no test-signing mode required.

---

## What You Get (The Basics)

* **Native 64-bit ASIO Audio (`Behringer BCD2000 ASIO 64-bit`):**
  * 4 output channels: **Master Left/Right (Rear RCA)** + **Phones Left/Right (Front Cue 1/4" TRS)** at 44.1 kHz / 16-bit PCM.
  * 4 input channels exposed via ASIO (`In 1–4`).
  * Low-latency real-time streaming with hardware clock tracking to keep playback crisp and click-free.
* **Simultaneous 64-bit MIDI IN & OUT (`Behringer BCD2000 MIDI`):**
  * Full support for faders, knobs, jog wheels, buttons, and **bi-directional LED feedback**—working **at the exact same time** as ASIO audio without exclusive-access conflicts.
* **One-Click Installer (`Behringer_BCD2000_Setup_x64.exe`):**
  * Automatically binds Windows' native `WinUSB` driver to your BCD2000 (you **do not** need to download or configure Zadig manually).
  * Registers both the 64-bit ASIO driver and the 64-bit Windows MIDI driver cleanly with a standard uninstaller.
* **Safe Mixxx Preset Included (`Behringer BCD2000 (Driver Safe)`):**
  * Optional preset for Mixxx 2.x that adds soft-takeover and prevents physical EQ or Master knobs left at zero from muting your audio when Mixxx starts.

---

## Quick Start Guide

### Requirements
* A PC running **Windows 10 or Windows 11 (64-bit / x64)**.
* A **Behringer BCD2000** connected preferably to a direct USB port (avoid unpowered USB hubs).

### Installation Steps
1. **Close** any open DAW or DJ applications (Mixxx, Ableton Live, REAPER, etc.).
2. Run **`Behringer_BCD2000_Setup_x64.exe`** as Administrator and follow the wizard.
   * *(Optional)* Check the Mixxx safe preset option if you use Mixxx.
3. If your BCD2000 was already plugged in during installation, **unplug and reconnect the USB cable** once so Windows applies the new binding cleanly.
4. Open your DAW or DJ software and select:
   * **Audio Device (ASIO):** `Behringer BCD2000 ASIO 64-bit`
   * **MIDI Controller (IN & OUT):** `Behringer BCD2000 MIDI`
5. **Tip for Mixxx users:** Choose the **`Behringer BCD2000 (Driver Safe)`** controller mapping included by the installer. If you prefer Mixxx's built-in legacy preset, make sure your physical EQ knobs (Low / Mid / High) are centered before enabling the controller.

---

## For Developers & Contributors

Want to see the internal architecture diagram, learn how we solved USB 1.1 crystal drift and WinUSB audio+MIDI sharing, compile from source, or contribute code?

👉 Read the full engineering specification in **[TECHNICS.md](TECHNICS.md)**.
🤖 AI agents and automated coding assistants must follow **[AGENTS.md](AGENTS.md)**.
