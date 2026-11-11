/**
 * @file bcd2000_control_panel_gui.cpp
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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "bcd2000_control_panel_gui.h"
#include "../common/bcd2000_shared_state.h"
#include "../common/bcd2000_usb_protocol.h"

#include <windows.h>
#include <objbase.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace bcd2000 {

namespace {

constexpr int kWindowWidth  = 640;
constexpr int kWindowHeight = 710;

// High-Contrast Color Palette (Dark Theme / Professional Pro Audio)
constexpr COLORREF kClrBackground    = RGB(18, 20, 24);    // Deep dark background
constexpr COLORREF kClrCardBg        = RGB(28, 32, 40);    // Clean dark card container
constexpr COLORREF kClrCardBorder    = RGB(48, 54, 68);    // Subtle 1px card outline
constexpr COLORREF kClrTextWhite     = RGB(250, 252, 255); // High-contrast crisp white text
constexpr COLORREF kClrSubText       = RGB(185, 192, 205); // High-contrast light slate text
constexpr COLORREF kClrHeaderGold    = RGB(245, 155, 40);  // Behringer signature amber header
constexpr COLORREF kClrCyanReadout   = RGB(0, 210, 255);   // Crisp cyan for frequency
constexpr COLORREF kClrStatusGreen   = RGB(46, 204, 113);  // Vibrant lock green
constexpr COLORREF kClrStatusYellow  = RGB(241, 196, 15);  // Warning yellow
constexpr COLORREF kClrStatusRed     = RGB(245, 60, 60);   // Alert red
constexpr COLORREF kClrMeterTrough   = RGB(14, 16, 20);    // Meter trough

enum ControlIds : int {
    IDC_BUFFER_COMBO = 201,
    IDC_APPLY_BUFFER_BTN,
    IDC_HEADROOM_COMBO,
    IDC_SWAP_CHANNELS_CHK,
    IDC_MIRROR_PHONES_CHK,
    IDC_MONO_DOWNMIX_CHK,
    IDC_OUTPUT_TRIM_COMBO,
    IDC_MASTER_MUTE_CHK,
    IDC_PHONES_MUTE_CHK,
    IDC_CLOCK_DYNAMIC_RADIO,
    IDC_CLOCK_DYNAMIC_LABEL,
    IDC_CLOCK_FIXED_RADIO,
    IDC_CLOCK_FIXED_LABEL,
    IDC_ALWAYS_ON_TOP_CHK,
    IDC_DIRECT_MONITOR_CHK,
    IDC_RESET_GLITCH_BTN,
    IDC_FLASH_LEDS_BTN,
    IDC_ABOUT_BTN,
    IDC_CLOSE_BTN,
    IDC_METER_VIEW
};

struct CardRects {
    RECT rcBuffer{15, 65, 615, 175};
    RECT rcRouting{15, 185, 615, 305};
    RECT rcClock{15, 315, 615, 410};
    RECT rcTelemetry{15, 420, 615, 595};
};

struct DialogContext {
    HWND hMainWnd{nullptr};
    HWND hMeterWnd{nullptr};
    HWND hParentWnd{nullptr};
    bool standalone{false};
    void *pDriverInstance{nullptr};

    HANDLE hSharedMem{nullptr};
    Bcd2000SharedState *pSharedState{nullptr};

    HFONT hFontNormal{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontTitle{nullptr};
    HFONT hFontSmall{nullptr};

    HBRUSH hBrushBg{nullptr};
    HBRUSH hBrushCard{nullptr};
    HPEN   hPenCardBorder{nullptr};

    // Control handles
    HWND hBufferCombo{nullptr};
    HWND hHeadroomCombo{nullptr};
    HWND hTrimCombo{nullptr};
    HWND hSwapChk{nullptr};
    HWND hMirrorChk{nullptr};
    HWND hMonoChk{nullptr};
    HWND hMasterMuteChk{nullptr};
    HWND hPhonesMuteChk{nullptr};
    HWND hClockDynamicRadio{nullptr};
    HWND hClockFixedRadio{nullptr};
    HWND hAlwaysOnTopChk{nullptr};
    HWND hDirectMonitorChk{nullptr};

    // Telemetry labels
    HWND hHardwareStatusPill{nullptr};
    HWND hHostInfoLabel{nullptr};
    HWND hClockLiveLabel{nullptr};
    HWND hSyncStatusLabel{nullptr};

    CardRects cards{};
};

std::atomic<HWND> g_hMainControlPanelWnd{nullptr};

// Converts linear peak 0..32767 to decibels (-48 dB .. 0 dB)
double PeakToDb(int16_t peak) {
    if (peak <= 10) return -48.0;
    double ratio = static_cast<double>(peak) / 32767.0;
    double db = 20.0 * std::log10(ratio);
    if (db < -48.0) db = -48.0;
    if (db > 0.0) db = 0.0;
    return db;
}

void DrawMeterChannel(HDC memDC, int x, int y, int width, int height,
                      const wchar_t *label, int16_t peakValue) {
    // Label
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, kClrTextWhite);
    RECT rcLabel{x, y, x + 72, y + height};
    DrawTextW(memDC, label, -1, &rcLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int barX = x + 74;
    const int barW = width - 135;
    const int valX = barX + barW + 8;

    // Meter Trough
    RECT rcTrough{barX, y + 2, barX + barW, y + height - 2};
    HBRUSH hTroughBrush = CreateSolidBrush(kClrMeterTrough);
    FillRect(memDC, &rcTrough, hTroughBrush);
    DeleteObject(hTroughBrush);

    // Level calculation
    double db = PeakToDb(peakValue);
    double fraction = (db + 48.0) / 48.0; // 0.0 to 1.0
    int fillW = static_cast<int>(fraction * static_cast<double>(barW));
    if (fillW < 0) fillW = 0;
    if (fillW > barW) fillW = barW;

    if (fillW > 0) {
        int greenW = (std::min)(fillW, static_cast<int>(barW * 0.75));
        int yellowW = 0;
        int redW = 0;

        if (fillW > static_cast<int>(barW * 0.75)) {
            yellowW = (std::min)(fillW, static_cast<int>(barW * 0.93)) - static_cast<int>(barW * 0.75);
        }
        if (fillW > static_cast<int>(barW * 0.93)) {
            redW = fillW - static_cast<int>(barW * 0.93);
        }

        int curX = barX;
        if (greenW > 0) {
            RECT rcG{curX, y + 2, curX + greenW, y + height - 2};
            HBRUSH hG = CreateSolidBrush(kClrStatusGreen);
            FillRect(memDC, &rcG, hG);
            DeleteObject(hG);
            curX += greenW;
        }
        if (yellowW > 0) {
            RECT rcY{curX, y + 2, curX + yellowW, y + height - 2};
            HBRUSH hY = CreateSolidBrush(kClrStatusYellow);
            FillRect(memDC, &rcY, hY);
            DeleteObject(hY);
            curX += yellowW;
        }
        if (redW > 0) {
            RECT rcR{curX, y + 2, curX + redW, y + height - 2};
            HBRUSH hR = CreateSolidBrush(kClrStatusRed);
            FillRect(memDC, &rcR, hR);
            DeleteObject(hR);
        }
    }

    // Clip Indicator
    bool isClip = (peakValue >= 32700);
    RECT rcClip{barX + barW - 6, y + 2, barX + barW, y + height - 2};
    HBRUSH hClipBrush = CreateSolidBrush(isClip ? kClrStatusRed : RGB(36, 40, 50));
    FillRect(memDC, &rcClip, hClipBrush);
    DeleteObject(hClipBrush);

    // Numeric dB readout
    wchar_t szDb[16];
    if (db <= -47.9) {
        std::swprintf(szDb, sizeof(szDb) / sizeof(wchar_t), L"-inf");
    } else {
        std::swprintf(szDb, sizeof(szDb) / sizeof(wchar_t), L"%.1f dB", db);
    }
    SetTextColor(memDC, isClip ? kClrStatusRed : kClrTextWhite);
    RECT rcVal{valX, y, valX + 52, y + height};
    DrawTextW(memDC, szDb, -1, &rcVal, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// Meter View Custom Window Procedure
LRESULT CALLBACK MeterWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogContext *ctx = reinterpret_cast<DialogContext *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (!ctx && msg == WM_CREATE) {
        LPCREATESTRUCTW cs = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        ctx = reinterpret_cast<DialogContext *>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    }

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

        // Background matching card
        HBRUSH hBg = CreateSolidBrush(kClrCardBg);
        FillRect(memDC, &rc, hBg);
        DeleteObject(hBg);

        if (ctx && ctx->hFontSmall) {
            SelectObject(memDC, ctx->hFontSmall);
        }

        int16_t pkMasterL = ctx && ctx->pSharedState ? ctx->pSharedState->peakMasterL : 0;
        int16_t pkMasterR = ctx && ctx->pSharedState ? ctx->pSharedState->peakMasterR : 0;
        int16_t pkPhonesL = ctx && ctx->pSharedState ? ctx->pSharedState->peakPhonesL : 0;
        int16_t pkPhonesR = ctx && ctx->pSharedState ? ctx->pSharedState->peakPhonesR : 0;
        uint32_t ringFill = ctx && ctx->pSharedState ? ctx->pSharedState->ringFillPercent : 0;
        uint64_t xruns    = ctx && ctx->pSharedState ? ctx->pSharedState->xrunCount : 0;

        int rowH = 18;
        int startY = 4;
        int w = rc.right - 10;

        DrawMeterChannel(memDC, 6, startY + 0 * rowH, w, rowH, L"Master L", pkMasterL);
        DrawMeterChannel(memDC, 6, startY + 1 * rowH, w, rowH, L"Master R", pkMasterR);
        DrawMeterChannel(memDC, 6, startY + 2 * rowH + 4, w, rowH, L"Phones L", pkPhonesL);
        DrawMeterChannel(memDC, 6, startY + 3 * rowH + 4, w, rowH, L"Phones R", pkPhonesR);

        // Ring buffer fill gauge
        int gaugeY = startY + 4 * rowH + 12;
        SetTextColor(memDC, kClrTextWhite);
        RECT rcGaugeLbl{6, gaugeY, 6 + 72, gaugeY + 16};
        DrawTextW(memDC, L"Ring Buffer", -1, &rcGaugeLbl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        int barX = 6 + 74;
        int barW = w - 135;
        RECT rcGaugeTrough{barX, gaugeY + 2, barX + barW, gaugeY + 14};
        HBRUSH hTroughBrush = CreateSolidBrush(kClrMeterTrough);
        FillRect(memDC, &rcGaugeTrough, hTroughBrush);
        DeleteObject(hTroughBrush);

        int fillW = static_cast<int>((static_cast<double>(ringFill) / 100.0) * static_cast<double>(barW));
        if (fillW > barW) fillW = barW;
        if (fillW > 0) {
            RECT rcGFill{barX, gaugeY + 2, barX + fillW, gaugeY + 14};
            COLORREF fillClr = (ringFill > 80 || ringFill < 5) ? kClrStatusRed
                             : (ringFill > 60) ? kClrStatusYellow : kClrCyanReadout;
            HBRUSH hF = CreateSolidBrush(fillClr);
            FillRect(memDC, &rcGFill, hF);
            DeleteObject(hF);
        }

        wchar_t szGauge[32];
        std::swprintf(szGauge, sizeof(szGauge) / sizeof(wchar_t), L"%u%% (Stable)", ringFill);
        SetTextColor(memDC, kClrTextWhite);
        RECT rcGaugeVal{barX + barW + 8, gaugeY, barX + barW + 80, gaugeY + 16};
        DrawTextW(memDC, szGauge, -1, &rcGaugeVal, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Dropout / Glitch counter
        int glitchY = gaugeY + 20;
        wchar_t szGlitch[96];
        if (xruns == 0) {
            std::swprintf(szGlitch, sizeof(szGlitch) / sizeof(wchar_t), L"Audio Engine Health: ZERO DROPOUTS (100%% Glitch-Free)");
            SetTextColor(memDC, kClrStatusGreen);
        } else {
            std::swprintf(szGlitch, sizeof(szGlitch) / sizeof(wchar_t), L"Audio Engine Health: %llu XRuns / Buffer Underruns Detected!", xruns);
            SetTextColor(memDC, kClrStatusRed);
        }
        RECT rcGlitch{6, glitchY, rc.right - 6, glitchY + 18};
        DrawTextW(memDC, szGlitch, -1, &rcGlitch, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void RegisterMeterWindowClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MeterWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"BCD2000_MeterView";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);
}

// About Window Procedure displaying AUTHORS metadata
LRESULT CALLBACK AboutWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogContext *ctx = reinterpret_cast<DialogContext *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (!ctx && msg == WM_CREATE) {
        LPCREATESTRUCTW cs = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        ctx = reinterpret_cast<DialogContext *>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        HWND hOkBtn = CreateWindowExW(0, L"BUTTON", L"OK",
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      (580 - 100) / 2, 452, 100, 32,
                                      hWnd, reinterpret_cast<HMENU>(IDOK), cs->hInstance, nullptr);
        SetFocus(hOkBtn);
        return 0;
    }

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        HBRUSH hBg = CreateSolidBrush(kClrBackground);
        FillRect(hdc, &rcClient, hBg);
        DeleteObject(hBg);

        SetBkMode(hdc, TRANSPARENT);
        if (ctx && ctx->hFontTitle) {
            SelectObject(hdc, ctx->hFontTitle);
        }
        SetTextColor(hdc, kClrHeaderGold);
        TextOutW(hdc, 24, 14, L"BEHRINGER BCD2000", 17);

        if (ctx && ctx->hFontBold) {
            SelectObject(hdc, ctx->hFontBold);
        }
        SetTextColor(hdc, kClrTextWhite);
        TextOutW(hdc, 24, 38, L"Native 64-bit ASIO 2.3 & WinMM MIDI Driver Suite - v2.0.0", 57);

        // Dark card container
        RECT rcCard{20, 65, 560, 440};
        HPEN hPen = CreatePen(PS_SOLID, 1, kClrCardBorder);
        HBRUSH hCardBrush = CreateSolidBrush(kClrCardBg);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, hPen));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, hCardBrush));
        RoundRect(hdc, rcCard.left, rcCard.top, rcCard.right, rcCard.bottom, 8, 8);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(hCardBrush);
        DeleteObject(hPen);

        if (ctx) {
            // Section 1: Primary Author & Copyright
            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, kClrHeaderGold);
            TextOutW(hdc, 36, 78, L"PRIMARY AUTHOR & LEAD DEVELOPER", 31);

            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, kClrTextWhite);
            TextOutW(hdc, 46, 98, L"Martin Revert <martinrevert@gmail.com>", 38);

            SelectObject(hdc, ctx->hFontNormal);
            SetTextColor(hdc, kClrSubText);
            RECT rcAuth{46, 118, 545, 160};
            DrawTextW(hdc,
                L"Architecture, design, and implementation of the native 64-bit user-mode ASIO driver (bcd2000_asio.dll), WinMM MIDI driver (bcd2000_midi.dll), Zero-Zadig WinUSB setup, dashboard, diagnostic test suite, and NSIS installer.",
                -1, &rcAuth, DT_LEFT | DT_WORDBREAK);

            // Section 2: Acknowledgments & Prior Work
            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, kClrHeaderGold);
            TextOutW(hdc, 36, 172, L"ACKNOWLEDGMENTS & PRIOR WORK", 28);

            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, kClrTextWhite);
            TextOutW(hdc, 46, 192, L"Mario Kicherer <dev@kicherer.org>", 33);

            SelectObject(hdc, ctx->hFontNormal);
            SetTextColor(hdc, kClrSubText);
            RECT rcMario{46, 212, 545, 252};
            DrawTextW(hdc,
                L"Author of the Linux kernel ALSA driver (sound/usb/bcd2000), whose foundational reverse-engineering uncovered the 52-byte hardware initialization handshake (kInitSequence).",
                -1, &rcMario, DT_LEFT | DT_WORDBREAK);

            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, kClrTextWhite);
            TextOutW(hdc, 46, 256, L"Golzo & The Mixxx Development Team", 35);

            SelectObject(hdc, ctx->hFontNormal);
            SetTextColor(hdc, kClrSubText);
            RECT rcMixxx{46, 276, 545, 316};
            DrawTextW(hdc,
                L"Authors of the original Behringer BCD2000 Mixxx controller mapping, operational foundation for the safe preset scripts (Behringer-BCD2000-safe-scripts.js).",
                -1, &rcMixxx, DT_LEFT | DT_WORDBREAK);

            // Section 3: Open-Source License & Trademarks
            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, kClrHeaderGold);
            TextOutW(hdc, 36, 324, L"OPEN-SOURCE LICENSE & TRADEMARKS", 32);

            SelectObject(hdc, ctx->hFontNormal);
            SetTextColor(hdc, kClrSubText);
            RECT rcLic{46, 344, 545, 380};
            DrawTextW(hdc,
                L"Distributed under the GNU General Public License v3.0 or later (GPL-3.0-or-later) with Steinberg ASIO and Host Application Linking Exception (Section 7).",
                -1, &rcLic, DT_LEFT | DT_WORDBREAK);

            SelectObject(hdc, ctx->hFontSmall);
            SetTextColor(hdc, RGB(140, 148, 162));
            RECT rcTm{46, 386, 545, 432};
            DrawTextW(hdc,
                L"ASIO is a registered trademark of Steinberg Media Technologies GmbH.\nBehringer and BCD2000 are trademarks of Music Tribe Global Brands Ltd.\nWindows is a registered trademark of Microsoft Corporation.",
                -1, &rcTm, DT_LEFT | DT_WORDBREAK);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (!dis) break;
        bool isPressed = (dis->itemState & ODS_SELECTED);
        COLORREF bgClr = isPressed ? RGB(185, 90, 15) : RGB(230, 120, 20); // Behringer Signature Orange
        COLORREF borderClr = RGB(255, 150, 45);
        COLORREF textClr = RGB(255, 255, 255);

        HBRUSH hBtnBrush = CreateSolidBrush(bgClr);
        HPEN hBtnPen = CreatePen(PS_SOLID, 1, borderClr);
        HPEN oldPen = static_cast<HPEN>(SelectObject(dis->hDC, hBtnPen));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dis->hDC, hBtnBrush));

        RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 6, 6);

        SelectObject(dis->hDC, oldBrush);
        SelectObject(dis->hDC, oldPen);
        DeleteObject(hBtnBrush);
        DeleteObject(hBtnPen);

        wchar_t text[32]{};
        GetWindowTextW(dis->hwndItem, text, 31);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, textClr);
        if (ctx && ctx->hFontBold) {
            SelectObject(dis->hDC, ctx->hFontBold);
        }
        DrawTextW(dis->hDC, text, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE: {
        DestroyWindow(hWnd);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ShowAboutDialog(HWND hParent, HINSTANCE hInst, DialogContext *mainCtx) {
    if (!hParent || !mainCtx) return;

    static std::once_flag aboutClassOnce;
    std::call_once(aboutClassOnce, [hInst]() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AboutWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"BCD2000_AboutWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HICON hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
        if (!hIcon) {
            hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        }
        wc.hIcon = hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(hInst ? hInst : GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        wc.hbrBackground = CreateSolidBrush(kClrBackground);
        RegisterClassExW(&wc);
    });

    constexpr int kAboutW = 580;
    constexpr int kAboutH = 535;

    RECT rcParent{};
    GetWindowRect(hParent, &rcParent);
    int x = rcParent.left + ((rcParent.right - rcParent.left) - kAboutW) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - kAboutH) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND hAbout = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"BCD2000_AboutWindow",
        L"About Behringer BCD2000 Driver Suite",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, kAboutW, kAboutH,
        hParent, nullptr, hInst, mainCtx
    );

    if (!hAbout) return;

    EnableWindow(hParent, FALSE);
    ShowWindow(hAbout, SW_SHOW);
    UpdateWindow(hAbout);

    MSG msg;
    while (IsWindow(hAbout) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN)) {
            DestroyWindow(hAbout);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
}

// Standalone LED Flash helper
void FlashAllHardwareLedsStandalone(DialogContext *ctx) {
    if (!ctx) return;
    if (ctx->pSharedState && ctx->pSharedState->driverActive) {
        ctx->pSharedState->flashLedsTrigger = 1;
        return;
    }

    HANDLE hDev = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE hWinUsb = nullptr;

    GUID winusbGuid = kWinUsbGenericGuid;
    HDEVINFO devInfo = SetupDiGetClassDevsW(&winusbGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &winusbGuid, i, &ifData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        if (reqSize == 0) continue;

        std::vector<uint8_t> buf(reqSize);
        auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, reqSize, nullptr, nullptr)) {
            std::wstring path = detail->DevicePath;
            if (path.find(L"vid_1397") != std::wstring::npos && path.find(L"pid_00bd") != std::wstring::npos) {
                hDev = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (hDev != INVALID_HANDLE_VALUE) {
                    if (WinUsb_Initialize(hDev, &hWinUsb)) {
                        break;
                    }
                    CloseHandle(hDev);
                    hDev = INVALID_HANDLE_VALUE;
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    if (hWinUsb) {
        ULONG written = 0;
        WinUsb_WritePipe(hWinUsb, 0x01, const_cast<PUCHAR>(kInitSequence), sizeof(kInitSequence), &written, nullptr);
        Sleep(50);

        for (uint8_t cc = 0x00; cc <= 0x19; ++cc) {
            uint8_t midi[3] = {0xB0, cc, 0x7F};
            SendBcdMidiOverlapped(hWinUsb, 0x01, midi, sizeof(midi));
        }
        Sleep(400);

        for (uint8_t cc = 0x00; cc <= 0x19; ++cc) {
            uint8_t midi[3] = {0xB0, cc, 0x00};
            SendBcdMidiOverlapped(hWinUsb, 0x01, midi, sizeof(midi));
        }

        WinUsb_Free(hWinUsb);
        CloseHandle(hDev);
    }
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogContext *ctx = reinterpret_cast<DialogContext *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (!ctx && msg == WM_CREATE) {
        LPCREATESTRUCTW cs = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        ctx = reinterpret_cast<DialogContext *>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->hMainWnd = hWnd;
    }

    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = GetModuleHandleW(nullptr);

        ctx->hFontNormal = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ctx->hFontBold   = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ctx->hFontTitle  = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        ctx->hFontSmall  = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        ctx->hBrushBg    = CreateSolidBrush(kClrBackground);
        ctx->hBrushCard  = CreateSolidBrush(kClrCardBg);
        ctx->hPenCardBorder = CreatePen(PS_SOLID, 1, kClrCardBorder);

        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

        // 1. Header Section
        HWND hTitle = CreateWindowExW(0, L"STATIC", L"BEHRINGER BCD2000",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      20, 12, 280, 24, hWnd, nullptr, hInst, nullptr);
        SendMessageW(hTitle, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontTitle), TRUE);

        HWND hSubTitle = CreateWindowExW(0, L"STATIC", L"64-bit Native ASIO & WinMM MIDI Control Panel",
                                         WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         20, 36, 320, 18, hWnd, nullptr, hInst, nullptr);
        SendMessageW(hSubTitle, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

        ctx->hHardwareStatusPill = CreateWindowExW(0, L"STATIC", L"[ CONNECTED (VID 1397, PID 00BD) ]",
                                                   WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                                   320, 15, 290, 20, hWnd, nullptr, hInst, nullptr);
        SendMessageW(ctx->hHardwareStatusPill, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hHostInfoLabel = CreateWindowExW(0, L"STATIC", L"DAW: Standalone (No DAW Active)",
                                              WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                              320, 36, 290, 18, hWnd, nullptr, hInst, nullptr);
        SendMessageW(ctx->hHostInfoLabel, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

        // 2. Section 1 Controls: ASIO Buffer & Latency
        HWND lblBuf = CreateWindowExW(0, L"STATIC", L"ASIO Buffer Size:",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      30, 99, 125, 20, hWnd, nullptr, hInst, nullptr);
        SendMessageW(lblBuf, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hBufferCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                            160, 96, 265, 200, hWnd,
                                            reinterpret_cast<HMENU>(IDC_BUFFER_COMBO), hInst, nullptr);
        SendMessageW(ctx->hBufferCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
        SetWindowTheme(ctx->hBufferCombo, L"DarkMode_CFD", nullptr);

        static const wchar_t *kBufOptions[] = {
            L"64 samples  (1.45 ms) - Ultra-Low Latency",
            L"128 samples (2.90 ms) - Low Latency",
            L"256 samples (5.80 ms) - Recommended / Balanced",
            L"512 samples (11.61 ms) - Safe / Default",
            L"1024 samples (23.22 ms) - Relaxed / Mixing",
            L"2048 samples (46.44 ms) - High Stability",
            L"4096 samples (92.88 ms) - Maximum Buffer"
        };
        for (const wchar_t *opt : kBufOptions) {
            ComboBox_AddString(ctx->hBufferCombo, opt);
        }

        // Apply Button (Owner-drawn for crisp contrast)
        CreateWindowExW(0, L"BUTTON", L"Apply (Hot Reset)",
                        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                        435, 95, 165, 28, hWnd,
                        reinterpret_cast<HMENU>(IDC_APPLY_BUFFER_BTN), hInst, nullptr);

        HWND lblMargin = CreateWindowExW(0, L"STATIC", L"USB Safety Margin:",
                                         WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         30, 137, 125, 20, hWnd, nullptr, hInst, nullptr);
        SendMessageW(lblMargin, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hHeadroomCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                              WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                              160, 134, 440, 120, hWnd,
                                              reinterpret_cast<HMENU>(IDC_HEADROOM_COMBO), hInst, nullptr);
        SendMessageW(ctx->hHeadroomCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
        SetWindowTheme(ctx->hHeadroomCombo, L"DarkMode_CFD", nullptr);

        ComboBox_AddString(ctx->hHeadroomCombo, L"Aggressive (1 buffer / ~2 ms prefill - Fast Scratch)");
        ComboBox_AddString(ctx->hHeadroomCombo, L"Balanced (3 buffers / ~10 ms prefill - Default)");
        ComboBox_AddString(ctx->hHeadroomCombo, L"Relaxed (5 buffers / ~25 ms prefill - Heavy DPC)");

        // 3. Section 2 Controls: Routing Matrix
        ctx->hSwapChk = CreateWindowExW(0, L"BUTTON", L"Swap Master (RCA 1-2) & Phones (Front 3-4)",
                                        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                        30, 220, 310, 20, hWnd,
                                        reinterpret_cast<HMENU>(IDC_SWAP_CHANNELS_CHK), hInst, nullptr);
        SendMessageW(ctx->hSwapChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
        SetWindowTheme(ctx->hSwapChk, L"DarkMode_Explorer", nullptr);

        ctx->hMirrorChk = CreateWindowExW(0, L"BUTTON", L"Mirror Master to Phones (Cue Follows Master)",
                                          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                          30, 245, 310, 20, hWnd,
                                          reinterpret_cast<HMENU>(IDC_MIRROR_PHONES_CHK), hInst, nullptr);
        SendMessageW(ctx->hMirrorChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
        SetWindowTheme(ctx->hMirrorChk, L"DarkMode_Explorer", nullptr);

        ctx->hMonoChk = CreateWindowExW(0, L"BUTTON", L"Mono Downmix (Master RCA L+R)",
                                        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                        30, 270, 250, 20, hWnd,
                                        reinterpret_cast<HMENU>(IDC_MONO_DOWNMIX_CHK), hInst, nullptr);
        SendMessageW(ctx->hMonoChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
        SetWindowTheme(ctx->hMonoChk, L"DarkMode_Explorer", nullptr);

        HWND lblTrim = CreateWindowExW(0, L"STATIC", L"Output Trim:",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       350, 221, 80, 20, hWnd, nullptr, hInst, nullptr);
        SendMessageW(lblTrim, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hTrimCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                          440, 218, 160, 120, hWnd,
                                          reinterpret_cast<HMENU>(IDC_OUTPUT_TRIM_COMBO), hInst, nullptr);
        SendMessageW(ctx->hTrimCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
        SetWindowTheme(ctx->hTrimCombo, L"DarkMode_CFD", nullptr);

        ComboBox_AddString(ctx->hTrimCombo, L"0 dB (Unity / Max)");
        ComboBox_AddString(ctx->hTrimCombo, L"-3 dB (Clip Protection)");
        ComboBox_AddString(ctx->hTrimCombo, L"-6 dB (DJ Safe Pad)");
        ComboBox_AddString(ctx->hTrimCombo, L"-12 dB (Attenuated)");

        ctx->hMasterMuteChk = CreateWindowExW(0, L"BUTTON", L"Mute Master (1-2)",
                                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                              350, 248, 125, 20, hWnd,
                                              reinterpret_cast<HMENU>(IDC_MASTER_MUTE_CHK), hInst, nullptr);
        SendMessageW(ctx->hMasterMuteChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
        SetWindowTheme(ctx->hMasterMuteChk, L"DarkMode_Explorer", nullptr);

        ctx->hPhonesMuteChk = CreateWindowExW(0, L"BUTTON", L"Mute Phones (3-4)",
                                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                              485, 248, 120, 20, hWnd,
                                              reinterpret_cast<HMENU>(IDC_PHONES_MUTE_CHK), hInst, nullptr);
        SendMessageW(ctx->hPhonesMuteChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
        SetWindowTheme(ctx->hPhonesMuteChk, L"DarkMode_Explorer", nullptr);

        // 4. Section 3 Controls: Clock
        // Note: Windows 10/11 DarkMode_Explorer does not skin BS_AUTORADIOBUTTON text to white,
        // leaving text rendered in black (RGB(0,0,0)). We decouple the radio glyph button (width 18)
        // from a dedicated STATIC label (SS_NOTIFY) so the text is 100% guaranteed high-contrast white.
        ctx->hClockDynamicRadio = CreateWindowExW(0, L"BUTTON", L"",
                                                  WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                                  30, 345, 18, 18, hWnd,
                                                  reinterpret_cast<HMENU>(IDC_CLOCK_DYNAMIC_RADIO), hInst, nullptr);
        SetWindowTheme(ctx->hClockDynamicRadio, L"DarkMode_Explorer", nullptr);

        HWND lblRadioDyn = CreateWindowExW(0, L"STATIC", L"Dynamic Hardware Crystal Lock (EP 0x83 IN Live Clock Feedback)",
                                           WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
                                           54, 345, 550, 18, hWnd,
                                           reinterpret_cast<HMENU>(IDC_CLOCK_DYNAMIC_LABEL), hInst, nullptr);
        SendMessageW(lblRadioDyn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        ctx->hClockFixedRadio = CreateWindowExW(0, L"BUTTON", L"",
                                                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                                30, 368, 18, 18, hWnd,
                                                reinterpret_cast<HMENU>(IDC_CLOCK_FIXED_RADIO), hInst, nullptr);
        SetWindowTheme(ctx->hClockFixedRadio, L"DarkMode_Explorer", nullptr);

        HWND lblRadioFix = CreateWindowExW(0, L"STATIC", L"Fixed USB 44/45 Cadence (Synthetic Fallback Mode)",
                                           WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
                                           54, 368, 550, 18, hWnd,
                                           reinterpret_cast<HMENU>(IDC_CLOCK_FIXED_LABEL), hInst, nullptr);
        SendMessageW(lblRadioFix, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);

        ctx->hClockLiveLabel = CreateWindowExW(0, L"STATIC", L"Live DAC Clock: 44,098.4 Hz  (-36 PPM)",
                                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                                               30, 390, 270, 18, hWnd, nullptr, hInst, nullptr);
        SendMessageW(ctx->hClockLiveLabel, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        ctx->hSyncStatusLabel = CreateWindowExW(0, L"STATIC", L"Sync Status: [ LOCKED * ]",
                                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                310, 390, 280, 18, hWnd, nullptr, hInst, nullptr);
        SendMessageW(ctx->hSyncStatusLabel, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

        // 5. Section 4: Telemetry Child View & Pin/Always-On-Top Toggle
        ctx->hAlwaysOnTopChk = CreateWindowExW(0, L"BUTTON", L"Always On Top",
                                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                               490, 426, 115, 18, hWnd,
                                               reinterpret_cast<HMENU>(IDC_ALWAYS_ON_TOP_CHK), hInst, nullptr);
        SendMessageW(ctx->hAlwaysOnTopChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);
        SetWindowTheme(ctx->hAlwaysOnTopChk, L"DarkMode_Explorer", nullptr);

        ctx->hMeterWnd = CreateWindowExW(0, L"BCD2000_MeterView", nullptr,
                                         WS_CHILD | WS_VISIBLE,
                                         25, 450, 580, 135, hWnd,
                                         reinterpret_cast<HMENU>(IDC_METER_VIEW), hInst, ctx);

        // 6. Section 5: Footer Buttons (Owner-drawn for maximum contrast)
        CreateWindowExW(0, L"BUTTON", L"Reset Glitch Counter",
                        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                        15, 612, 140, 32, hWnd,
                        reinterpret_cast<HMENU>(IDC_RESET_GLITCH_BTN), hInst, nullptr);

        CreateWindowExW(0, L"BUTTON", L"Flash LEDs",
                        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                        162, 612, 140, 32, hWnd,
                        reinterpret_cast<HMENU>(IDC_FLASH_LEDS_BTN), hInst, nullptr);

        ctx->hDirectMonitorChk = CreateWindowExW(0, L"BUTTON", L"Direct Input Monitor",
                                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                 310, 618, 140, 20, hWnd,
                                                 reinterpret_cast<HMENU>(IDC_DIRECT_MONITOR_CHK), hInst, nullptr);
        SendMessageW(ctx->hDirectMonitorChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontNormal), TRUE);
        SetWindowTheme(ctx->hDirectMonitorChk, L"DarkMode_Explorer", nullptr);

        CreateWindowExW(0, L"BUTTON", L"About...",
                        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                        458, 612, 75, 32, hWnd,
                        reinterpret_cast<HMENU>(IDC_ABOUT_BTN), hInst, nullptr);

        CreateWindowExW(0, L"BUTTON", L"Close",
                        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                        540, 612, 75, 32, hWnd,
                        reinterpret_cast<HMENU>(IDC_CLOSE_BTN), hInst, nullptr);

        // Populate initial values
        if (ctx->pSharedState) {
            int bufIdx = 2; // Default 256
            int bSize = ctx->pSharedState->bufferSize;
            if (bSize <= 64) bufIdx = 0;
            else if (bSize <= 128) bufIdx = 1;
            else if (bSize <= 256) bufIdx = 2;
            else if (bSize <= 512) bufIdx = 3;
            else if (bSize <= 1024) bufIdx = 4;
            else if (bSize <= 2048) bufIdx = 5;
            else bufIdx = 6;
            ComboBox_SetCurSel(ctx->hBufferCombo, bufIdx);

            int hrIdx = (ctx->pSharedState->safetyHeadroom == 1) ? 0 :
                        (ctx->pSharedState->safetyHeadroom == 5) ? 2 : 1;
            ComboBox_SetCurSel(ctx->hHeadroomCombo, hrIdx);

            int trimIdx = 0;
            if (ctx->pSharedState->outputTrimDb == -3) trimIdx = 1;
            else if (ctx->pSharedState->outputTrimDb == -6) trimIdx = 2;
            else if (ctx->pSharedState->outputTrimDb == -12) trimIdx = 3;
            ComboBox_SetCurSel(ctx->hTrimCombo, trimIdx);

            Button_SetCheck(ctx->hSwapChk, (ctx->pSharedState->routingMode == Routing_Swap) ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(ctx->hMirrorChk, (ctx->pSharedState->routingMode == Routing_Mirror) ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(ctx->hMonoChk, ctx->pSharedState->monoDownmix ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(ctx->hMasterMuteChk, ctx->pSharedState->masterMute ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(ctx->hPhonesMuteChk, ctx->pSharedState->phonesMute ? BST_CHECKED : BST_UNCHECKED);

            Button_SetCheck(ctx->hClockDynamicRadio, (ctx->pSharedState->clockMode == Clock_DynamicLock) ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(ctx->hClockFixedRadio, (ctx->pSharedState->clockMode == Clock_FixedCadence) ? BST_CHECKED : BST_UNCHECKED);

            Button_SetCheck(ctx->hDirectMonitorChk, ctx->pSharedState->directMonitor ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(ctx->hAlwaysOnTopChk, (ctx->pSharedState->alwaysOnTop != 0) ? BST_CHECKED : BST_UNCHECKED);
        }

        SetTimer(hWnd, 1001, 33, nullptr);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        // Fill Main Window Background
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        HBRUSH hBgBrush = CreateSolidBrush(kClrBackground);
        FillRect(hdc, &rcClient, hBgBrush);
        DeleteObject(hBgBrush);

        if (ctx) {
            HPEN hPen = CreatePen(PS_SOLID, 1, kClrCardBorder);
            HBRUSH hCardBrush = CreateSolidBrush(kClrCardBg);
            HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, hPen));
            HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, hCardBrush));

            // Draw Section Cards with rounded corners
            RoundRect(hdc, ctx->cards.rcBuffer.left, ctx->cards.rcBuffer.top,
                      ctx->cards.rcBuffer.right, ctx->cards.rcBuffer.bottom, 8, 8);
            RoundRect(hdc, ctx->cards.rcRouting.left, ctx->cards.rcRouting.top,
                      ctx->cards.rcRouting.right, ctx->cards.rcRouting.bottom, 8, 8);
            RoundRect(hdc, ctx->cards.rcClock.left, ctx->cards.rcClock.top,
                      ctx->cards.rcClock.right, ctx->cards.rcClock.bottom, 8, 8);
            RoundRect(hdc, ctx->cards.rcTelemetry.left, ctx->cards.rcTelemetry.top,
                      ctx->cards.rcTelemetry.right, ctx->cards.rcTelemetry.bottom, 8, 8);

            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(hCardBrush);
            DeleteObject(hPen);

            // Draw Crisp High-Contrast Card Titles
            SelectObject(hdc, ctx->hFontBold);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, kClrHeaderGold);

            TextOutW(hdc, ctx->cards.rcBuffer.left + 15, ctx->cards.rcBuffer.top + 8,
                     L"ASIO BUFFER SIZE & TIMING", 25);
            TextOutW(hdc, ctx->cards.rcRouting.left + 15, ctx->cards.rcRouting.top + 8,
                     L"AUDIO ROUTING & OUTPUT MATRIX (ON-THE-FLY)", 42);
            TextOutW(hdc, ctx->cards.rcClock.left + 15, ctx->cards.rcClock.top + 8,
                     L"CLOCK & HARDWARE DRIFT PROTECTION", 33);
            TextOutW(hdc, ctx->cards.rcTelemetry.left + 15, ctx->cards.rcTelemetry.top + 8,
                     L"REAL-TIME SIGNAL MONITORS (30 FPS)", 34);
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (!dis || !ctx) break;

        bool isPressed = (dis->itemState & ODS_SELECTED);

        COLORREF bgClr = isPressed ? RGB(36, 40, 52) : RGB(46, 52, 66);
        COLORREF borderClr = RGB(70, 78, 98);
        COLORREF textClr = kClrTextWhite;

        if (dis->CtlID == IDC_APPLY_BUFFER_BTN) {
            bgClr = isPressed ? RGB(185, 90, 15) : RGB(230, 120, 20); // Behringer Signature Orange
            borderClr = RGB(255, 150, 45);
            textClr = RGB(255, 255, 255);
        } else if (dis->CtlID == IDC_CLOSE_BTN || dis->CtlID == IDC_ABOUT_BTN) {
            bgClr = isPressed ? RGB(40, 46, 58) : RGB(56, 64, 78);
            borderClr = RGB(85, 96, 116);
            textClr = RGB(255, 255, 255);
        }

        HBRUSH hBtnBrush = CreateSolidBrush(bgClr);
        HPEN hBtnPen = CreatePen(PS_SOLID, 1, borderClr);
        HPEN oldPen = static_cast<HPEN>(SelectObject(dis->hDC, hBtnPen));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dis->hDC, hBtnBrush));

        RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 6, 6);

        SelectObject(dis->hDC, oldBrush);
        SelectObject(dis->hDC, oldPen);
        DeleteObject(hBtnBrush);
        DeleteObject(hBtnPen);

        wchar_t text[128]{};
        GetWindowTextW(dis->hwndItem, text, 127);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, textClr);
        SelectObject(dis->hDC, ctx->hFontBold);
        DrawTextW(dis->hDC, text, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }
    case WM_TIMER: {
        if (!ctx) return 0;
        if (ctx->pSharedState) {
            if (ctx->pSharedState->hardwareConnected) {
                SetWindowTextW(ctx->hHardwareStatusPill, L"[ CONNECTED (VID 1397, PID 00BD) ]");
            } else {
                SetWindowTextW(ctx->hHardwareStatusPill, L"[ DISCONNECTED / SIMULATION ]");
            }

            if (ctx->pSharedState->driverActive && ctx->pSharedState->dawProcessName[0] != L'\0') {
                wchar_t szHost[128];
                std::swprintf(szHost, sizeof(szHost) / sizeof(wchar_t),
                              L"DAW: %s (PID: %u)",
                              ctx->pSharedState->dawProcessName,
                              ctx->pSharedState->activePid);
                SetWindowTextW(ctx->hHostInfoLabel, szHost);
            } else {
                SetWindowTextW(ctx->hHostInfoLabel, L"DAW: Standalone (No DAW Active)");
            }

            wchar_t szClock[128];
            std::swprintf(szClock, sizeof(szClock) / sizeof(wchar_t),
                          L"Live DAC Clock: %.1f Hz  (%+.1f PPM)",
                          ctx->pSharedState->liveDacRate,
                          ctx->pSharedState->clockDriftPpm);
            SetWindowTextW(ctx->hClockLiveLabel, szClock);

            if (ctx->pSharedState->clockLocked) {
                SetWindowTextW(ctx->hSyncStatusLabel, L"Sync Status: [ LOCKED * ]");
            } else {
                SetWindowTextW(ctx->hSyncStatusLabel, L"Sync Status: [ SEARCHING ]");
            }
        }

        if (ctx->hMeterWnd) {
            InvalidateRect(ctx->hMeterWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (!ctx || !ctx->pSharedState) break;

        if (id == IDC_APPLY_BUFFER_BTN && code == BN_CLICKED) {
            int sel = ComboBox_GetCurSel(ctx->hBufferCombo);
            static const int kSizes[] = {64, 128, 256, 512, 1024, 2048, 4096};
            if (sel >= 0 && sel < 7) {
                int newSize = kSizes[sel];
                ctx->pSharedState->requestedBufferSize = static_cast<uint32_t>(newSize);
                ctx->pSharedState->bufferSize = newSize;
                SaveSettingsToRegistry(ctx->pSharedState);
            }
        } else if (id == IDC_HEADROOM_COMBO && code == CBN_SELCHANGE) {
            int sel = ComboBox_GetCurSel(ctx->hHeadroomCombo);
            int hr = (sel == 0) ? 1 : (sel == 2) ? 5 : 3;
            ctx->pSharedState->safetyHeadroom = hr;
            SaveSettingsToRegistry(ctx->pSharedState);
        } else if (id == IDC_OUTPUT_TRIM_COMBO && code == CBN_SELCHANGE) {
            int sel = ComboBox_GetCurSel(ctx->hTrimCombo);
            static const int kTrims[] = {0, -3, -6, -12};
            if (sel >= 0 && sel < 4) {
                ctx->pSharedState->outputTrimDb = kTrims[sel];
                SaveSettingsToRegistry(ctx->pSharedState);
            }
        } else if (id == IDC_SWAP_CHANNELS_CHK && code == BN_CLICKED) {
            bool checked = (Button_GetCheck(ctx->hSwapChk) == BST_CHECKED);
            if (checked) {
                Button_SetCheck(ctx->hMirrorChk, BST_UNCHECKED);
                ctx->pSharedState->routingMode = Routing_Swap;
            } else {
                ctx->pSharedState->routingMode = Routing_Normal;
            }
            SaveSettingsToRegistry(ctx->pSharedState);
        } else if (id == IDC_MIRROR_PHONES_CHK && code == BN_CLICKED) {
            bool checked = (Button_GetCheck(ctx->hMirrorChk) == BST_CHECKED);
            if (checked) {
                Button_SetCheck(ctx->hSwapChk, BST_UNCHECKED);
                ctx->pSharedState->routingMode = Routing_Mirror;
            } else {
                ctx->pSharedState->routingMode = Routing_Normal;
            }
            SaveSettingsToRegistry(ctx->pSharedState);
        } else if (id == IDC_MONO_DOWNMIX_CHK && code == BN_CLICKED) {
            bool checked = (Button_GetCheck(ctx->hMonoChk) == BST_CHECKED);
            ctx->pSharedState->monoDownmix = checked ? 1 : 0;
            SaveSettingsToRegistry(ctx->pSharedState);
        } else if (id == IDC_MASTER_MUTE_CHK && code == BN_CLICKED) {
            bool checked = (Button_GetCheck(ctx->hMasterMuteChk) == BST_CHECKED);
            ctx->pSharedState->masterMute = checked ? 1 : 0;
        } else if (id == IDC_PHONES_MUTE_CHK && code == BN_CLICKED) {
            bool checked = (Button_GetCheck(ctx->hPhonesMuteChk) == BST_CHECKED);
            ctx->pSharedState->phonesMute = checked ? 1 : 0;
        } else if ((id == IDC_CLOCK_DYNAMIC_RADIO && code == BN_CLICKED) ||
                   (id == IDC_CLOCK_DYNAMIC_LABEL && code == STN_CLICKED)) {
            Button_SetCheck(ctx->hClockDynamicRadio, BST_CHECKED);
            Button_SetCheck(ctx->hClockFixedRadio, BST_UNCHECKED);
            ctx->pSharedState->clockMode = Clock_DynamicLock;
            SaveSettingsToRegistry(ctx->pSharedState);
        } else if ((id == IDC_CLOCK_FIXED_RADIO && code == BN_CLICKED) ||
                   (id == IDC_CLOCK_FIXED_LABEL && code == STN_CLICKED)) {
            Button_SetCheck(ctx->hClockDynamicRadio, BST_UNCHECKED);
            Button_SetCheck(ctx->hClockFixedRadio, BST_CHECKED);
            ctx->pSharedState->clockMode = Clock_FixedCadence;
            SaveSettingsToRegistry(ctx->pSharedState);
        } else if (id == IDC_ABOUT_BTN && code == BN_CLICKED) {
            ShowAboutDialog(hWnd, GetModuleHandleW(nullptr), ctx);
        } else if (id == IDC_ALWAYS_ON_TOP_CHK && code == BN_CLICKED) {
            bool checked = (Button_GetCheck(ctx->hAlwaysOnTopChk) == BST_CHECKED);
            if (ctx->pSharedState) {
                ctx->pSharedState->alwaysOnTop = checked ? 1 : 0;
                SaveSettingsToRegistry(ctx->pSharedState);
            }
            SetWindowPos(hWnd, checked ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (id == IDC_DIRECT_MONITOR_CHK && code == BN_CLICKED) {
            bool checked = (Button_GetCheck(ctx->hDirectMonitorChk) == BST_CHECKED);
            ctx->pSharedState->directMonitor = checked ? 1 : 0;
            SaveSettingsToRegistry(ctx->pSharedState);
        } else if (id == IDC_RESET_GLITCH_BTN && code == BN_CLICKED) {
            ctx->pSharedState->resetGlitchCounter = 1;
            ctx->pSharedState->xrunCount = 0;
        } else if (id == IDC_FLASH_LEDS_BTN && code == BN_CLICKED) {
            FlashAllHardwareLedsStandalone(ctx);
        } else if (id == IDC_CLOSE_BTN && code == BN_CLICKED) {
            SendMessageW(hWnd, WM_CLOSE, 0, 0);
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND hCtrl = reinterpret_cast<HWND>(lParam);

        if (ctx) {
            SetBkMode(hdc, TRANSPARENT);

            if (hCtrl == ctx->hHardwareStatusPill) {
                bool conn = (ctx->pSharedState && ctx->pSharedState->hardwareConnected);
                SetTextColor(hdc, conn ? kClrStatusGreen : kClrStatusYellow);
                return reinterpret_cast<LRESULT>(ctx->hBrushBg);
            }
            if (hCtrl == ctx->hHostInfoLabel) {
                SetTextColor(hdc, kClrSubText);
                return reinterpret_cast<LRESULT>(ctx->hBrushBg);
            }
            if (hCtrl == ctx->hClockLiveLabel) {
                SetTextColor(hdc, kClrCyanReadout);
                return reinterpret_cast<LRESULT>(ctx->hBrushCard);
            }
            if (hCtrl == ctx->hSyncStatusLabel) {
                bool lck = (ctx->pSharedState && ctx->pSharedState->clockLocked);
                SetTextColor(hdc, lck ? kClrStatusGreen : kClrStatusYellow);
                return reinterpret_cast<LRESULT>(ctx->hBrushCard);
            }

            // High-contrast crisp white text for all field labels, checkboxes and radio buttons
            SetTextColor(hdc, kClrTextWhite);

            RECT rcCtrl;
            GetWindowRect(hCtrl, &rcCtrl);
            POINT pt{rcCtrl.left, rcCtrl.top};
            ScreenToClient(hWnd, &pt);

            if (pt.y >= 65 && pt.y < 595) {
                return reinterpret_cast<LRESULT>(ctx->hBrushCard);
            }
            return reinterpret_cast<LRESULT>(ctx->hBrushBg);
        }
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        if (ctx) {
            SetBkColor(hdc, RGB(38, 42, 52));
            SetTextColor(hdc, kClrTextWhite);
            return reinterpret_cast<LRESULT>(ctx->hBrushCard);
        }
        break;
    }
    case WM_CLOSE: {
        KillTimer(hWnd, 1001);
        DestroyWindow(hWnd);
        return 0;
    }
    case WM_DESTROY: {
        g_hMainControlPanelWnd.store(nullptr, std::memory_order_release);
        if (ctx) {
            if (ctx->hFontNormal) DeleteObject(ctx->hFontNormal);
            if (ctx->hFontBold)   DeleteObject(ctx->hFontBold);
            if (ctx->hFontTitle)  DeleteObject(ctx->hFontTitle);
            if (ctx->hFontSmall)  DeleteObject(ctx->hFontSmall);
            if (ctx->hBrushBg)    DeleteObject(ctx->hBrushBg);
            if (ctx->hBrushCard)  DeleteObject(ctx->hBrushCard);
            if (ctx->hPenCardBorder) DeleteObject(ctx->hPenCardBorder);
            CloseSharedState(ctx->hSharedMem, ctx->pSharedState);
            ctx->hSharedMem = nullptr;
            ctx->pSharedState = nullptr;
            ctx->hMainWnd = nullptr;
        }
        // Exits message loop on dedicated UI thread or standalone process
        PostQuitMessage(0);
        return 0;
    }
    case WM_NCDESTROY: {
        if (ctx) {
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
            delete ctx;
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void RunControlPanelWindow(HWND hParent, bool standalone, void *pDriverInstance) {
    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static std::once_flag registerOnce;
    std::call_once(registerOnce, [hInst]() {
        RegisterMeterWindowClass(hInst);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = MainWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"BCD2000_ControlPanelWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HICON hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
        if (!hIcon) {
            hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        }
        wc.hIcon = hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(hInst ? hInst : GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        wc.hbrBackground = CreateSolidBrush(kClrBackground);
        RegisterClassExW(&wc);
    });

    DialogContext *ctx = new DialogContext();
    ctx->hParentWnd = hParent;
    ctx->standalone = standalone;
    ctx->pDriverInstance = pDriverInstance;
    ctx->pSharedState = OpenOrCreateSharedState(&ctx->hSharedMem, false);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - kWindowWidth) / 2;
    int posY = (screenH - kWindowHeight) / 2;
    if (posX < 0) posX = 0;
    if (posY < 0) posY = 0;

    // Use nullptr parent so the control panel window is a completely uncoupled, top-level floating window
    // with its own taskbar presence, preventing cross-thread Win32 message queue attachments or hangs.
    DWORD exStyle = WS_EX_APPWINDOW;
    if (!ctx->pSharedState || ctx->pSharedState->alwaysOnTop != 0) {
        exStyle |= WS_EX_TOPMOST;
    }

    HWND hWnd = CreateWindowExW(
        exStyle,
        L"BCD2000_ControlPanelWindow",
        L"Behringer BCD2000 ASIO Control Panel (64-bit)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        posX, posY, kWindowWidth, kWindowHeight,
        nullptr, nullptr, hInst, ctx
    );

    if (!hWnd) {
        CloseSharedState(ctx->hSharedMem, ctx->pSharedState);
        delete ctx;
        return;
    }

    HICON hBig = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!hBig) hBig = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    HICON hSm = reinterpret_cast<HICON>(LoadImageW(hInst ? hInst : GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    if (hBig) SendMessageW(hWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hBig));
    if (hSm) SendMessageW(hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSm));

    g_hMainControlPanelWnd.store(hWnd, std::memory_order_release);

    ShowWindow(hWnd, SW_SHOWNORMAL);
    UpdateWindow(hWnd);

    // Dedicated message loop (runs either on dedicated thread in DAW or on main thread in standalone)
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace

void ShowBcd2000ControlPanel(HWND hParent, bool standalone, void *pDriverInstance) {
    // If window is already open, bring it to the foreground immediately
    HWND existing = g_hMainControlPanelWnd.load(std::memory_order_acquire);
    if (existing && IsWindow(existing)) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return;
    }

    if (standalone) {
        // Standalone executable: run message loop synchronously on this thread
        RunControlPanelWindow(nullptr, true, nullptr);
    } else {
        // In-process inside Ableton Live / DAW:
        // Launch a dedicated, isolated UI thread so Ableton's UI thread is NEVER blocked,
        // NEVER hijacked by an inner message loop, and NEVER receives a WM_QUIT message!
        std::thread([hParent, pDriverInstance]() {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            RunControlPanelWindow(hParent, false, pDriverInstance);
            CoUninitialize();
        }).detach();
    }
}

} // namespace bcd2000
