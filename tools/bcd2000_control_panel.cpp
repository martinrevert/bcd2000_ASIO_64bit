/**
 * @file bcd2000_control_panel.cpp
 * @brief Standalone Desktop Launcher for Behringer BCD2000 ASIO & MIDI Control Panel (x64)
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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include "../src/ui/bcd2000_control_panel_gui.h"

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/,
                    PWSTR /*pCmdLine*/, int /*nCmdShow*/) {
    // Run standalone control panel window
    bcd2000::ShowBcd2000ControlPanel(nullptr, true, nullptr);
    return 0;
}

