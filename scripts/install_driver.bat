@echo off
rem Behringer BCD2000 64-bit ASIO and MIDI Driver Suite
rem Copyright (C) 2024-2026 Martin Revert <martinrevert@gmail.com> and contributors
rem SPDX-License-Identifier: GPL-3.0-or-later

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_driver.ps1" %*
