# Behringer BCD2000 64-bit ASIO Driver & WinUSB Diagnostic Suite

Modern 64-bit user-mode driver and diagnostic suite for the **Behringer BCD2000 DJ Controller** (USB VID `0x1397`, PID `0x00BD`) on Windows x64 using **WinUSB** and the **Steinberg ASIO SDK**.

---

## Architecture Overview

* **Hardware Target:** Behringer B-Control Deejay BCD2000 (8051 Microcontroller, USB 1.1 Full-Speed 12 Mbps).
* **Physical Transport Layer:** Native Windows `WinUSB.sys` driver (no kernel-mode driver signing requirements).
* **Audio Layer:** 64-bit in-process COM server exposing the Steinberg ASIO 2.3 interface.
* **Control / MIDI Protocol:** Custom framed bulk/interrupt USB protocol (Endpoint `0x01` OUT, Endpoint `0x81` IN) derived from reverse engineering and the Linux kernel ALSA driver (`snd-bcd2000`).

---

## Milestone Verification Roadmap

1. **[Milestone 1] USB Detection & Link:** Physical enumeration, WinUSB binding, 52-byte initialization handshake, and LED feedback verification (`bcd2000_diag_usb.exe`).
2. **[Milestone 2] MIDI Control Telemetry:** Asynchronous input polling loop on Endpoint `0x81` (faders, knobs, jog wheels, buttons) and bi-directional LED state updates (`bcd2000_diag_midi.exe`).
3. **[Milestone 3] PCM Audio Transmission:** Continuous isochronous PCM linear streaming and 440 Hz test tone validation (`bcd2000_diag_audio.exe`).
4. **[Milestone 4] ASIO COM Server:** 64-bit dynamic link library (`bcd2000_asio.dll`) implementing `IASIO`, system registry installation, and sample host negotiation.
5. **[Milestone 5] DAW Integration:** Multi-channel low-latency streaming (Master + Headphone monitoring) in Reaper, Traktor, and Mixxx.

For technical specifications, see [`docs/ROADMAP_VERIFICATION.md`](docs/ROADMAP_VERIFICATION.md).

---

## Building

### Requirements
* Visual Studio Build Tools 2022 (MSVC x64 C++17)
* Windows 10/11 SDK (WinUSB & SetupAPI)
* CMake 3.20+

### Build Instructions
```powershell
cmake -B build -A x64
cmake --build build --config RelWithDebInfo --target bcd2000_diag_usb
```

### Running Diagnostics
```powershell
.\build\RelWithDebInfo\bcd2000_diag_usb.exe
```

