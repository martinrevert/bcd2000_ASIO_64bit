; Behringer BCD2000 64-bit ASIO + WinMM MIDI + WinUSB NSIS Installer
; Copyright (C) 2024-2026 Martin Revert <martinrevert@gmail.com> and contributors
; SPDX-License-Identifier: GPL-3.0-or-later
;
; Build via:
;   makensis.exe /DREPO_DIR=".." /DBUILD_BIN_DIR="..\build\Release" /DOUT_FILE="..\build\Release\Behringer_BCD2000_Setup_x64.exe" bcd2000_setup.nsi

!ifndef REPO_DIR
  !define REPO_DIR ".."
!endif
!ifndef BUILD_BIN_DIR
  !define BUILD_BIN_DIR "..\build\Release"
!endif
!ifndef OUT_FILE
  !define OUT_FILE "..\build\Release\Behringer_BCD2000_Setup_x64.exe"
!endif
!ifndef APP_VERSION
  !define APP_VERSION "2.0.0"
!endif

!define APP_NAME "Behringer BCD2000 ASIO & MIDI Driver (x64)"
!define APP_PUBLISHER "Martin Revert & BCD2000 Open Source Project"
!define APP_URL "https://github.com/martinrevert/bcd2000_ASIO_64bit"
!define APP_ISSUES "https://github.com/martinrevert/bcd2000_ASIO_64bit/issues"
!define REG_APP_KEY "Software\Behringer BCD2000 Driver"
!define REG_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\BehringerBCD2000Driver"

Unicode True
Name "${APP_NAME}"
OutFile "${OUT_FILE}"
InstallDir "$PROGRAMFILES64\Behringer BCD2000 Driver"
InstallDirRegKey HKLM "${REG_APP_KEY}" "InstallDir"
RequestExecutionLevel admin
ShowInstDetails show
ShowUnInstDetails show
SetCompressor /SOLID lzma

; Windows PE Version Information Header
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey "FileVersion" "${APP_VERSION}.0"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "FileDescription" "Behringer BCD2000 64-bit ASIO and MIDI Driver Suite Installer"
VIAddVersionKey "LegalCopyright" "Copyright (C) 2024-2026 Martin Revert and contributors"

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!define MUI_ABORTWARNING

; Language selection persistence between Installer and Uninstaller
!define MUI_LANGDLL_REGISTRY_ROOT "HKLM"
!define MUI_LANGDLL_REGISTRY_KEY "${REG_APP_KEY}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "InstallerLanguage"

!define MUI_WELCOMEPAGE_TITLE $(WELCOME_TITLE)
!define MUI_WELCOMEPAGE_TEXT $(WELCOME_TEXT)

!define MUI_FINISHPAGE_NOREBOOTSUPPORT
!define MUI_FINISHPAGE_TITLE $(FINISH_TITLE)
!define MUI_FINISHPAGE_TEXT $(FINISH_TEXT)

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${REPO_DIR}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; Supported project languages (7 languages)
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Spanish"
!insertmacro MUI_LANGUAGE "PortugueseBR"
!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "French"
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "Japanese"

!include "languages.nsh"

Section $(NAME_SecDrivers) SecDrivers
    SectionIn RO

    SetRegView 64
    ${DisableX64FSRedirection}

    ; Core driver binaries, licenses and docs in Program Files (64-bit)
    SetOutPath "$INSTDIR"
    File "${BUILD_BIN_DIR}\bcd2000_asio.dll"
    File "${BUILD_BIN_DIR}\bcd2000_midi.dll"
    File "${BUILD_BIN_DIR}\bcd2000_winusb_setup.exe"
    File "${REPO_DIR}\README.md"
    File "/oname=LICENSE.txt" "${REPO_DIR}\LICENSE"
    File "/oname=AUTHORS.txt" "${REPO_DIR}\AUTHORS"

    ; Reference copy of Mixxx safe preset + diagnostic tools
    SetOutPath "$INSTDIR\tools"
    File "${BUILD_BIN_DIR}\bcd2000_diag_usb.exe"
    File "${BUILD_BIN_DIR}\bcd2000_diag_midi.exe"
    File "${BUILD_BIN_DIR}\bcd2000_diag_audio.exe"
    File "${BUILD_BIN_DIR}\hostsample.exe"
    File "${BUILD_BIN_DIR}\hostsample_midi.exe"

    SetOutPath "$INSTDIR\presets\mixxx"
    File "${REPO_DIR}\presets\mixxx\Behringer BCD2000 (Driver Safe).midi.xml"
    File "${REPO_DIR}\presets\mixxx\Behringer-BCD2000-safe-scripts.js"

    ; 1. Stage & bind WinUSB driver for VID_1397&PID_00BD cleanly without Zadig
    DetailPrint $(DETAIL_WINUSB_SETUP)
    nsExec::ExecToLog '"$INSTDIR\bcd2000_winusb_setup.exe"'
    Pop $0
    ${If} $0 != 0
        DetailPrint "$(DETAIL_WINUSB_WARN) $0."
    ${EndIf}

    ; 2. Register 64-bit ASIO COM Server (HKLM)
    DetailPrint $(DETAIL_ASIO_REGISTER)
    nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\bcd2000_asio.dll"'
    Pop $0
    ${If} $0 != 0
        DetailPrint "$(DETAIL_ASIO_WARN) $0."
    ${EndIf}

    ; 3. Install & register 64-bit WinMM MIDI driver into real System32 + Drivers32
    DetailPrint $(DETAIL_MIDI_REGISTER)
    SetOutPath "$SYSDIR"
    File "${BUILD_BIN_DIR}\bcd2000_midi.dll"
    nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /s "$SYSDIR\bcd2000_midi.dll"'
    Pop $0
    ${If} $0 != 0
        DetailPrint "$(DETAIL_MIDI_WARN) $0."
    ${EndIf}

    ${EnableX64FSRedirection}

    ; Uninstaller & Add/Remove Programs registration
    SetOutPath "$INSTDIR"
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    WriteRegStr HKLM "${REG_APP_KEY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "${REG_APP_KEY}" "Version" "${APP_VERSION}"

    WriteRegStr HKLM "${REG_UNINST_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKLM "${REG_UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKLM "${REG_UNINST_KEY}" "Publisher" "${APP_PUBLISHER}"
    WriteRegStr HKLM "${REG_UNINST_KEY}" "URLInfoAbout" "${APP_URL}"
    WriteRegStr HKLM "${REG_UNINST_KEY}" "HelpLink" "${APP_ISSUES}"
    WriteRegStr HKLM "${REG_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "${REG_UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegDWORD HKLM "${REG_UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${REG_UNINST_KEY}" "NoRepair" 1
SectionEnd

Section /o $(NAME_SecMixxxPreset) SecMixxxPreset
    SetShellVarContext current
    DetailPrint $(DETAIL_MIXXX_PRESET)
    CreateDirectory "$LOCALAPPDATA\Mixxx\controllers"
    SetOutPath "$LOCALAPPDATA\Mixxx\controllers"
    File "${REPO_DIR}\presets\mixxx\Behringer BCD2000 (Driver Safe).midi.xml"
    File "${REPO_DIR}\presets\mixxx\Behringer-BCD2000-safe-scripts.js"
    WriteRegDWORD HKLM "${REG_APP_KEY}" "InstalledMixxxPreset" 1
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDrivers} $(DESC_SecDrivers)
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMixxxPreset} $(DESC_SecMixxxPreset)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
    SetRegView 64
    ${DisableX64FSRedirection}

    DetailPrint $(UN_DETAIL_ASIO)
    ${If} ${FileExists} "$INSTDIR\bcd2000_asio.dll"
        nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\bcd2000_asio.dll"'
        Pop $0
    ${EndIf}

    DetailPrint $(UN_DETAIL_MIDI)
    ${If} ${FileExists} "$SYSDIR\bcd2000_midi.dll"
        nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /u /s "$SYSDIR\bcd2000_midi.dll"'
        Pop $0
        Delete /REBOOTOK "$SYSDIR\bcd2000_midi.dll"
    ${EndIf}

    DetailPrint $(UN_DETAIL_WINUSB)
    ${If} ${FileExists} "$INSTDIR\bcd2000_winusb_setup.exe"
        nsExec::ExecToLog '"$INSTDIR\bcd2000_winusb_setup.exe" --uninstall'
        Pop $0
    ${EndIf}

    ${EnableX64FSRedirection}

    ; Remove Mixxx user preset only if we installed it
    ReadRegDWORD $0 HKLM "${REG_APP_KEY}" "InstalledMixxxPreset"
    ${If} $0 == 1
        SetShellVarContext current
        Delete "$LOCALAPPDATA\Mixxx\controllers\Behringer BCD2000 (Driver Safe).midi.xml"
        Delete "$LOCALAPPDATA\Mixxx\controllers\Behringer-BCD2000-safe-scripts.js"
    ${EndIf}

    ; Remove leftover HKCU ASIO key if user previously ran per-user registration
    DeleteRegKey HKCU "SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
    DeleteRegKey HKCU "SOFTWARE\Classes\CLSID\{B2D20000-1397-00BD-A510-000000000064}"

    ; Remove installed files & directories
    Delete "$INSTDIR\tools\bcd2000_diag_usb.exe"
    Delete "$INSTDIR\tools\bcd2000_diag_midi.exe"
    Delete "$INSTDIR\tools\bcd2000_diag_audio.exe"
    Delete "$INSTDIR\tools\hostsample.exe"
    Delete "$INSTDIR\tools\hostsample_midi.exe"
    RMDir "$INSTDIR\tools"

    Delete "$INSTDIR\presets\mixxx\Behringer BCD2000 (Driver Safe).midi.xml"
    Delete "$INSTDIR\presets\mixxx\Behringer-BCD2000-safe-scripts.js"
    RMDir "$INSTDIR\presets\mixxx"
    RMDir "$INSTDIR\presets"

    Delete "$INSTDIR\bcd2000_asio.dll"
    Delete "$INSTDIR\bcd2000_midi.dll"
    Delete "$INSTDIR\bcd2000_winusb_setup.exe"
    Delete "$INSTDIR\README.md"
    Delete "$INSTDIR\LICENSE.txt"
    Delete "$INSTDIR\AUTHORS.txt"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"

    DeleteRegKey HKLM "${REG_UNINST_KEY}"
    DeleteRegKey HKLM "${REG_APP_KEY}"
SectionEnd

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP|MB_OK $(MSG_REQUIRES_X64)
        Abort
    ${EndIf}

    ; Explicit fallback to English if system language is not one of our 7 project languages
    ${If} $LANGUAGE != ${LANG_ENGLISH}
    ${AndIf} $LANGUAGE != ${LANG_SPANISH}
    ${AndIf} $LANGUAGE != ${LANG_PORTUGUESEBR}
    ${AndIf} $LANGUAGE != ${LANG_GERMAN}
    ${AndIf} $LANGUAGE != ${LANG_FRENCH}
    ${AndIf} $LANGUAGE != ${LANG_SIMPCHINESE}
    ${AndIf} $LANGUAGE != ${LANG_JAPANESE}
        StrCpy $LANGUAGE ${LANG_ENGLISH}
    ${EndIf}

    !insertmacro MUI_LANGDLL_DISPLAY
    SetRegView 64
FunctionEnd

Function un.onInit
    !insertmacro MUI_UNGETLANGUAGE

    ; Explicit fallback to English if stored uninstaller language is invalid or unsupported
    ${If} $LANGUAGE != ${LANG_ENGLISH}
    ${AndIf} $LANGUAGE != ${LANG_SPANISH}
    ${AndIf} $LANGUAGE != ${LANG_PORTUGUESEBR}
    ${AndIf} $LANGUAGE != ${LANG_GERMAN}
    ${AndIf} $LANGUAGE != ${LANG_FRENCH}
    ${AndIf} $LANGUAGE != ${LANG_SIMPCHINESE}
    ${AndIf} $LANGUAGE != ${LANG_JAPANESE}
        StrCpy $LANGUAGE ${LANG_ENGLISH}
    ${EndIf}

    SetRegView 64
FunctionEnd
