# AI Agent Guidelines & Engineering Guardrails (`AGENTS.md`)

This document serves as the **mandatory operational instruction manual and guardrail specification for any AI agent** (or automated coding assistant) interacting with, modifying, or extending the **Behringer BCD2000 64-bit ASIO & MIDI Driver** codebase.

All AI agents working on this repository **must read, understand, and strictly follow** these rules before making any modifications.

---

## 1. Project Context & Mission

The Behringer BCD2000 (`VID 0x1397`, `PID 0x00BD`) is a legacy DJ controller originally manufactured with USB 1.1 Full-Speed (12 Mbps) hardware, an 8051 microcontroller, a TI TUSB3200 streaming interface, and a Cirrus Logic CS4220 codec. Behringer permanently abandoned official driver development at 32-bit Windows.

This project delivers what never existed officially: **a modern, native 64-bit user-mode ASIO audio driver (`bcd2000_asio.dll`), a native 64-bit WinMM MIDI driver (`bcd2000_midi.dll`), an automated zero-Zadig WinUSB installer (`bcd2000_winusb_setup.exe`), and an NSIS setup bundle (`Behringer_BCD2000_Setup_x64.exe`)** running on Windows 10 and Windows 11 (x64).

---

## 2. Immutable Architecture Invariants (ALWAYS Obey)

Any proposed modification that violates any of the following invariants is considered a critical regression and **must be rejected immediately**:

### 1. Strict 64-bit (`x64`) Only
* The project target is strictly Windows 64-bit (`x64`).
* Never remove the architecture check in [`CMakeLists.txt`](CMakeLists.txt) (`CMAKE_SIZEOF_VOID_P EQUAL 8`).
* Never introduce 32-bit target configurations or WoW64 bridging hacks.

### 2. 100% User-Mode Driver Architecture (Zero Kernel Drivers)
* The hardware link is managed exclusively through Microsoft's inbox **`WinUSB.sys`**.
* **Never** attempt to write, add, or require a kernel-mode `.sys` driver.
* **Never** require the user to disable Windows Driver Signature Enforcement, enable Test Signing mode, or purchase an EV/WHQL code-signing certificate.
* The driver installation model must remain self-contained via [`tools/bcd2000_winusb_setup.cpp`](tools/bcd2000_winusb_setup.cpp) (ephemeral local self-signed certificate installed into `LocalMachine\Root` and `TrustedPublisher` + `DiInstallDriverW`). Never force the end-user back to manual Zadig configuration.

### 3. Static MSVC Runtime (`/MT` and `/MTd`)
* [`CMakeLists.txt`](CMakeLists.txt) strictly enforces `set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")`.
* **Never** change this to `/MD` or `/MDd` (dynamic runtime).
* `bcd2000_asio.dll` and `bcd2000_midi.dll` are loaded in-process by third-party DAWs and DJ host applications (Mixxx, Ableton Live, REAPER, FL Studio, Traktor). They must **never** fail to load because a user's machine lacks a specific version of `VCRUNTIME140.dll` or `MSVCP140.dll`.

### 4. Dual-Mode WinUSB Sharing & Named Pipe IPC
* `WinUSB.sys` permits only one open handle to a device interface at any given time.
* To allow simultaneous ASIO audio and WinMM MIDI without conflicts, the dual-mode architecture must remain intact:
  * **Mode A (ASIO Pipe Client):** When ASIO is running, `bcd2000_asio.dll` owns the exclusive WinUSB handle and hosts the bidirectional MIDI pipe `\\.\pipe\BCD2000_ASIO_MIDI_V1`.
  * **Mode B (Standalone WinUSB + Hot Handoff):** When ASIO is idle, `bcd2000_midi.dll` owns WinUSB directly and serves `\\.\pipe\BCD2000_MIDI_CTRL_V1`. When ASIO starts, it sends `'R'`, MIDI immediately releases WinUSB, replies `'K'`, and transitions to Mode A **without closing open WinMM handles in the host application**.
* **Never** break the `'R' -> 'K'` handoff protocol.
* **Never** implement exclusive locking that starves either audio or MIDI.

### 5. Real-Time Audio Thread Safety
* Inside `AsioHostThreadMain` and `UsbStreamingThreadMain` ([`src/asio/bcd2000_asio_driver.cpp`](src/asio/bcd2000_asio_driver.cpp)):
  * **Never** allocate or free heap memory (`malloc`, `free`, `new`, `delete`, resizing `std::vector` or `std::string`).
  * **Never** acquire blocking locks (mutexes) that can contend with UI, worker, or IPC threads.
  * **Never** perform synchronous disk I/O, file logging, or console printing.
  * Audio transfer must remain decoupled via the lock-free/atomic **32,768-frame ring buffer** (`m_outRingBuffer`).
  * Thread scheduling must leverage Windows Multimedia Class Scheduler Service (**MMCSS `"Pro Audio"`** via `avrt.dll`).

### 6. Implicit Clock Tracking & Crystal Drift Protection
* USB 1.1 Full-Speed runs on 1 ms frames (`44.1 frames/ms`), while the BCD2000 DAC operates on an unsynchronized physical crystal.
* Slicing fixed 45-frame packets causes periodic ~2–3 Hz click artifacts.
* The implicit clock tracking engine on EP `0x83` IN (`WinUsb_ReadIsochPipeAsap`) dynamically measures actual hardware frame consumption and modulates `44`/`45`-frame output packets.
* **Never** remove or bypass this clock-tracking synchronization loop.

### 7. Single Source of Truth for Hardware Protocol
* All device constants (`VID 0x1397`, `PID 0x00BD`), the 52-byte initialization handshake (`kInitSequence`), named pipe definitions, `MidiRunningStatusParser`, and overlapped frame helpers **must remain centralized in [`src/common/bcd2000_usb_protocol.h`](src/common/bcd2000_usb_protocol.h)**.
* Do not duplicate hardware packet geometry or handshake sequences across multiple source files.

### 8. Strict Open-Source License Invariance (`GPL-3.0-or-later`)
* The entire repository is licensed under the **GNU General Public License v3 or later (`GPL-3.0-or-later`)** with the **ASIO & Host Application Linking Exception** (Section 7).
* All source files (`.cpp`, `.h`, `.def`, `.bat`, `.ps1`, `.nsi`, `.js`, `.xml`, `CMakeLists.txt`) must maintain the standard SPDX identifier (`SPDX-License-Identifier: GPL-3.0-or-later`) and canonical copyright notice.
* Clean-room ASIO headers in [`third_party/asio-sdk`](third_party/asio-sdk) must never be replaced with proprietary Steinberg SDK code that restricts open redistribution.

---

## 3. Strict Prohibitions (NEVER Do This)

> [!CAUTION]
> The following actions are strictly forbidden for AI agents working in this repository:

1. **DO NOT** introduce kernel-mode `.sys` drivers, driver signing workarounds, or scripts that disable Windows security features (`bcdedit /set testsigning on`, etc.).
2. **DO NOT** convert compiler runtime settings to dynamic `/MD`.
3. **DO NOT** introduce external runtime dependencies (Boost, Qt, heavy third-party frameworks). Standard C++17 and the Windows SDK are the sole allowed dependencies.
4. **DO NOT** modify the COM Class ID (`{B2D20000-1397-00BD-A510-000000000064}`) or registered driver names (`Behringer BCD2000 ASIO 64-bit`, `Behringer BCD2000 MIDI`) unless an explicit, user-instructed breaking change requires it.
5. **DO NOT** commit untested modifications to the USB streaming or MIDI parsing pipelines. Always verify builds with `Release` and `RelWithDebInfo` configurations and execute the diagnostic suite.
6. **DO NOT** remove or alter the Mixxx safe-preset logic ([`presets/mixxx/Behringer-BCD2000-safe-scripts.js`](presets/mixxx/Behringer-BCD2000-safe-scripts.js)) that prevents pots at zero from muting audio on startup.

---

## 4. Mandatory Multilingual Documentation Maintenance

The project maintains a professional user-facing and developer-facing documentation suite across **7 languages**:

| Code | Language | User Guide | Technical Guide |
| :---: | :--- | :--- | :--- |
| `en` | 🇬🇧 English (Primary) | [`README.md`](README.md) | [`TECHNICS.md`](TECHNICS.md) |
| `es` | 🇪🇸 Español | [`docs/i18n/README.es.md`](docs/i18n/README.es.md) | [`docs/i18n/TECHNICS.es.md`](docs/i18n/TECHNICS.es.md) |
| `pt` | 🇧🇷 Português | [`docs/i18n/README.pt.md`](docs/i18n/README.pt.md) | [`docs/i18n/TECHNICS.pt.md`](docs/i18n/TECHNICS.pt.md) |
| `de` | 🇩🇪 Deutsch | [`docs/i18n/README.de.md`](docs/i18n/README.de.md) | [`docs/i18n/TECHNICS.de.md`](docs/i18n/TECHNICS.de.md) |
| `fr` | 🇫🇷 Français | [`docs/i18n/README.fr.md`](docs/i18n/README.fr.md) | [`docs/i18n/TECHNICS.fr.md`](docs/i18n/TECHNICS.fr.md) |
| `zh` | 🇨🇳 中文 | [`docs/i18n/README.zh.md`](docs/i18n/README.zh.md) | [`docs/i18n/TECHNICS.zh.md`](docs/i18n/TECHNICS.zh.md) |
| `ja` | 🇯🇵 日本語 | [`docs/i18n/README.ja.md`](docs/i18n/README.ja.md) | [`docs/i18n/TECHNICS.ja.md`](docs/i18n/TECHNICS.ja.md) |

### The Documentation Sync Rule
Whenever an AI agent implements or modifies:
* User-facing features, installation steps, requirements, or preset behavior: **must update all 7 `README` files**.
* Internal architecture, endpoints, threading model, build requirements, CMake targets, or developer rules: **must update all 7 `TECHNICS` files**.
* Language navigation bars: **must maintain the SVG flag badges (`docs/i18n/flags/*.svg`) at both the top and bottom of each document**.

> [!IMPORTANT]
> Never update only the English documentation and leave translations out of date. If you lack fluency in a specific target language, produce an accurate, idiomatic translation maintaining technical identifiers (`bcd2000_asio.dll`, `WinUSB`, `IASIO`, endpoint numbers, file paths) intact.

---

## 5. Build, Test & Verification Procedures for AI Agents

Before declaring any coding task complete, an AI agent must run and verify the following workflow in PowerShell:

```powershell
# 1. Clean CMake configure targeting x64
cmake -B build -A x64

# 2. Build Release configuration
cmake --build build --config Release

# 3. Build RelWithDebInfo configuration
cmake --build build --config RelWithDebInfo

# 4. Build single-EXE NSIS installer
cmake --build build --config Release --target bcd2000_installer

# 5. Verify diagnostic tool compilation
# Ensure the following binaries exist in build\Release:
# - bcd2000_diag_usb.exe
# - bcd2000_diag_midi.exe
# - bcd2000_diag_audio.exe
# - hostsample.exe
# - hostsample_midi.exe
# - bcd2000_winusb_setup.exe
# - bcd2000_asio.dll
# - bcd2000_midi.dll
# - Behringer_BCD2000_Setup_x64.exe
```

### Automated Documentation Link & Asset Check
Run the following script to verify that every relative markdown link and SVG image target across all 15 documentation files resolves cleanly:

```powershell
$files = @('README.md', 'TECHNICS.md', 'AGENTS.md', 'docs/ROADMAP_VERIFICATION.md') + (Get-ChildItem 'docs/i18n/*.md' | ForEach-Object { $_.FullName })
$broken = @()
foreach ($f in $files) {
    $dir = Split-Path (Resolve-Path $f) -Parent
    $content = Get-Content $f -Raw -Encoding UTF8
    $matches = [regex]::Matches($content, '\]\(([^)#\s]+)(?:#[^)]+)?\)')
    foreach ($m in $matches) {
        $target = $m.Groups[1].Value
        if ($target -match '^(https?:|mailto:|file:)') { continue }
        $full = Join-Path $dir $target
        if (-not (Test-Path $full)) { $broken += "$f -> $target" }
    }
}
if ($broken.Count -gt 0) { throw "Broken links found: `n$($broken -join "`n")" }
Write-Output "All documentation links and asset references verified successfully."
```

---

## 6. Automated Release, Versioning & Tagging System

The repository features a fully automated Continuous Delivery (CD) pipeline driven by GitHub Actions ([`.github/workflows/release.yml`](.github/workflows/release.yml)) running on `windows-latest`.

### 1. Invariant: Baseline Version `v2.0.0` (Anti-Collision Guardrail)
* **Never** reset or downgrade the project version to `0.x` or `1.x`.
* Behringer originally shipped legacy 32-bit drivers labeled `v1.1.1.0` and `v1.2.0.0`.
* To completely eliminate user confusion and prevent Windows registry or DLL version collisions, this native 64-bit driver suite strictly starts at **`v2.0.0`** and monotonically increments from there.

### 2. Push-to-Main Automatic Tagging & Release Rule
* **Every direct push or merged Pull Request to the `main` branch triggers an automated GitHub release**.
* The workflow automatically:
  1. Inspects Git history and calculates the next Semantic Version tag (`mathieudutour/github-tag-action`).
  2. Tags the commit in the GitHub repository.
  3. Configures and builds all 64-bit Release binaries via CMake and MSVC.
  4. Compiles the single-EXE installer [`Behringer_BCD2000_Setup_x64.exe`](installer/bcd2000_setup.nsi) via NSIS with embedded Windows PE metadata (`ProductVersion` / `FileVersion` matching the release tag).
  5. Bundles a standalone portable ZIP archive (`bcd2000_v<VERSION>_binaries_x64.zip`).
  6. Computes SHA-256 integrity hashes into `SHA256SUMS.txt`.
  7. Publishes the official **GitHub Release** with all assets and auto-generated categorized release notes via [`.github/release.yml`](.github/release.yml). GitHub automatically attaches source code `.zip` and `.tar.gz` to the release.

### 3. Commit Message Conventions for AI Agents
AI agents (and human contributors) should use commit messages to control Semantic Version increments:
* **Default / Patch bump (`2.0.0 -> 2.0.1`):** Default behavior on any push to `main` (e.g. `fix: ...`, `fix(...)`, `docs: ...`, `chore: ...`, or standard commit messages).
* **Minor bump (`2.0.x -> 2.1.0`):** Include `feat: ...`, `feat(...)`, or `#minor` in the commit message or PR title when introducing backward-compatible new features, new DAW presets, or tools.
* **Major bump (`2.x.x -> 3.0.0`):** Include `BREAKING CHANGE: ...`, `feat!:`, or `#major` in the commit message or PR title when introducing breaking architectural modifications.

### 4. Local Build with Version Override
When testing builds locally in CMake or verifying packaging, always specify or verify the target version:
```powershell
cmake -B build -A x64 -DBCD2000_VERSION="2.0.0"
cmake --build build --config Release
cmake --build build --config Release --target bcd2000_installer
```

---

## 7. Self-Evolution: Rules for Modifying `AGENTS.md`

As the driver evolves, new hardware quirks, Windows OS platform changes (e.g., Windows 11 updates, new USB host controller behaviors), or project additions (e.g., automated GitHub Actions CI/CD pipelines, new DAW script mappings) may introduce new constraints.

When an AI agent identifies a new critical constraint, requirement, or anti-pattern:
1. **Document it in this file (`AGENTS.md`)**: Add the new rule under the appropriate section with clear rationale.
2. **Keep it in English**: `AGENTS.md` is strictly maintained in English for uniform parsing by any language model.
3. **Do not dilute existing guardrails**: Never weaken real-time safety, 64-bit-only enforcement, user-mode boundaries, or multilingual documentation requirements.

