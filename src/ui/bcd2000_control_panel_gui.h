/**
 * @file bcd2000_control_panel_gui.h
 * @brief Interactive Native 64-bit Control Panel & Live Dashboard GUI for Behringer BCD2000
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

#ifndef BCD2000_CONTROL_PANEL_GUI_H
#define BCD2000_CONTROL_PANEL_GUI_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace bcd2000 {

/**
 * @brief Opens or brings to front the interactive BCD2000 Control Panel.
 * @param hParent Parent HWND (e.g. from DAW host in IASIO::controlPanel, or nullptr for standalone).
 * @param standalone True if running as standalone executable, false if running in-process in DAW.
 * @param pDriverInstance Optional pointer to in-process Bcd2000AsioDriver for direct hot reset.
 */
void ShowBcd2000ControlPanel(HWND hParent, bool standalone, void *pDriverInstance = nullptr);

} // namespace bcd2000

#endif // BCD2000_CONTROL_PANEL_GUI_H

