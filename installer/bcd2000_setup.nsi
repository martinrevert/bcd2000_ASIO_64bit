; Behringer BCD2000 64-bit ASIO + WinMM MIDI + WinUSB NSIS Installer
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
  !define APP_VERSION "1.0.0"
!endif

!define APP_NAME "Behringer BCD2000 ASIO & MIDI Driver (x64)"
!define APP_PUBLISHER "BCD2000 64-bit Open Driver Project"
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

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Instalador de Drivers Behringer BCD2000 (64-bit)"
!define MUI_WELCOMEPAGE_TEXT "Este asistente instalará el paquete completo en modo usuario (x64) para tu controlador Behringer BCD2000:$\r$\n$\r$\n  • Driver USB WinUSB automático (no necesitás Zadig)$\r$\n  • Servidor COM ASIO 64-bit (4 canales de salida, baja latencia)$\r$\n  • Driver WinMM MIDI 64-bit simultáneo IN/OUT con puente ASIO$\r$\n$\r$\nIMPORTANTE: Cerrá Mixxx, Ableton Live u otros programas de audio/MIDI antes de continuar."

!define MUI_FINISHPAGE_NOREBOOTSUPPORT
!define MUI_FINISHPAGE_TITLE "Instalación completada"
!define MUI_FINISHPAGE_TEXT "Los drivers ASIO 64-bit, WinMM MIDI y WinUSB para Behringer BCD2000 quedaron instalados.$\r$\n$\r$\n• Si tu BCD2000 estaba conectada, desconectá y volvé a conectar el cable USB antes de abrir tu DAW.$\r$\n• Si usás Mixxx con el preset original de fábrica (en vez de nuestro 'Behringer BCD2000 (Driver Safe)'), dejá las perillas de EQ (Low/Mid/High) en el centro antes de activar el controlador MIDI."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Spanish"

LangString DESC_SecDrivers ${LANG_SPANISH} "Instala el driver WinUSB (vía catálogo firmado local), registra bcd2000_asio.dll (COM ASIO 64-bit), instala bcd2000_midi.dll en System32 (Drivers32) e incluye herramientas de diagnóstico."
LangString DESC_SecMixxxPreset ${LANG_SPANISH} "Instala 'Behringer BCD2000 (Driver Safe)' en %LOCALAPPDATA%\Mixxx\controllers\. Evita que las perillas físicas de EQ o Master dejadas en 0 corten el audio al iniciar Mixxx y agrega soft-takeover."

Section "Drivers BCD2000 (WinUSB + ASIO + MIDI x64)" SecDrivers
    SectionIn RO

    SetRegView 64
    ${DisableX64FSRedirection}

    ; Core driver binaries and helper in Program Files (64-bit)
    SetOutPath "$INSTDIR"
    File "${BUILD_BIN_DIR}\bcd2000_asio.dll"
    File "${BUILD_BIN_DIR}\bcd2000_midi.dll"
    File "${BUILD_BIN_DIR}\bcd2000_winusb_setup.exe"
    File "${REPO_DIR}\README.md"

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
    DetailPrint "Configurando driver WinUSB para Behringer BCD2000 (VID_1397 / PID_00BD)..."
    nsExec::ExecToLog '"$INSTDIR\bcd2000_winusb_setup.exe"'
    Pop $0
    ${If} $0 != 0
        DetailPrint "Advertencia: bcd2000_winusb_setup devolvió código $0."
    ${EndIf}

    ; 2. Register 64-bit ASIO COM Server (HKLM)
    DetailPrint "Registrando servidor COM ASIO 64-bit..."
    nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\bcd2000_asio.dll"'
    Pop $0
    ${If} $0 != 0
        DetailPrint "Advertencia: regsvr32 ASIO devolvió código $0."
    ${EndIf}

    ; 3. Install & register 64-bit WinMM MIDI driver into real System32 + Drivers32
    DetailPrint "Instalando y registrando driver WinMM MIDI 64-bit en System32..."
    SetOutPath "$SYSDIR"
    File "${BUILD_BIN_DIR}\bcd2000_midi.dll"
    nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /s "$SYSDIR\bcd2000_midi.dll"'
    Pop $0
    ${If} $0 != 0
        DetailPrint "Advertencia: regsvr32 MIDI devolvió código $0."
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
    WriteRegStr HKLM "${REG_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "${REG_UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegDWORD HKLM "${REG_UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${REG_UNINST_KEY}" "NoRepair" 1
SectionEnd

Section /o "Preset MIDI seguro para Mixxx 2.x (evita audio en 0)" SecMixxxPreset
    SetShellVarContext current
    DetailPrint "Instalando preset 'Behringer BCD2000 (Driver Safe)' en $LOCALAPPDATA\Mixxx\controllers..."
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

    DetailPrint "Desregistrando servidor COM ASIO 64-bit..."
    ${If} ${FileExists} "$INSTDIR\bcd2000_asio.dll"
        nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\bcd2000_asio.dll"'
        Pop $0
    ${EndIf}

    DetailPrint "Desregistrando driver WinMM MIDI 64-bit..."
    ${If} ${FileExists} "$SYSDIR\bcd2000_midi.dll"
        nsExec::ExecToLog '"$SYSDIR\regsvr32.exe" /u /s "$SYSDIR\bcd2000_midi.dll"'
        Pop $0
        Delete /REBOOTOK "$SYSDIR\bcd2000_midi.dll"
    ${EndIf}

    DetailPrint "Limpiando paquete WinUSB..."
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
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"

    DeleteRegKey HKLM "${REG_UNINST_KEY}"
    DeleteRegKey HKLM "${REG_APP_KEY}"
SectionEnd

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP|MB_OK "Este instalador requiere Windows 64-bit (x64)."
        Abort
    ${EndIf}
    SetRegView 64
FunctionEnd

Function un.onInit
    SetRegView 64
FunctionEnd
