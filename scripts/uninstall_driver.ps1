param (
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$AsioCandidate = Join-Path $ScriptDir "..\build\$Config\bcd2000_asio.dll"
$MidiCandidate = Join-Path $ScriptDir "..\build\$Config\bcd2000_midi.dll"
$AsioDllPath = (Resolve-Path $AsioCandidate -ErrorAction SilentlyContinue).Path
$MidiDllPath = (Resolve-Path $MidiCandidate -ErrorAction SilentlyContinue).Path

$RegKeyHKLM   = "HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$RegKeyHKCU   = "HKCU:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$ClsidKeyHKLM = "HKLM:\SOFTWARE\Classes\CLSID\{B2D20000-1397-00BD-A510-000000000064}"
$ClsidKeyHKCU = "HKCU:\SOFTWARE\Classes\CLSID\{B2D20000-1397-00BD-A510-000000000064}"
$Drivers32Key = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32"

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "   Desinstalador Driver ASIO COM + WinMM MIDI 64-bit (BCD2000)" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

if ($AsioDllPath -and (Test-Path $AsioDllPath)) {
    Write-Host "[1/3] Ejecutando regsvr32 /u sobre ASIO: $AsioDllPath" -ForegroundColor Yellow
    & regsvr32.exe /u /s "$AsioDllPath"
} else {
    Write-Host "[1/3] Binario ASIO no encontrado en $AsioCandidate; limpiando Registro..." -ForegroundColor Yellow
}

# Limpiar claves ASIO en HKCU (no requiere elevacion)
Write-Host "[2/3] Verificando y limpiando claves en HKEY_CURRENT_USER..." -ForegroundColor Yellow
if (Test-Path $RegKeyHKCU) {
    Remove-Item -Path $RegKeyHKCU -Recurse -Force -ErrorAction SilentlyContinue
}
if (Test-Path $ClsidKeyHKCU) {
    Remove-Item -Path $ClsidKeyHKCU -Recurse -Force -ErrorAction SilentlyContinue
}

$HasMidiReg = $false
$props = Get-ItemProperty -Path $Drivers32Key -ErrorAction SilentlyContinue
if ($props) {
    foreach ($i in 0..9) {
        $name = if ($i -eq 0) { "midi" } else { "midi$i" }
        $val = $props.$name
        if ($val -and ($val -like "*bcd2000_midi.dll")) {
            $HasMidiReg = $true
        }
    }
}

# Limpiar claves HKLM (requiere elevacion UAC si la consola no es Admin)
Write-Host "[3/3] Verificando y limpiando claves en HKEY_LOCAL_MACHINE..." -ForegroundColor Yellow
if ((Test-Path $RegKeyHKLM) -or (Test-Path $ClsidKeyHKLM) -or $HasMidiReg) {
    Write-Host "      Solicitando elevacion UAC para limpiar HKLM (ASIO + Drivers32 MIDI)..." -ForegroundColor Cyan
    $ElevatedCmd = "reg.exe delete `"HKLM\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit`" /f /reg:64 & reg.exe delete `"HKLM\SOFTWARE\Classes\CLSID\{B2D20000-1397-00BD-A510-000000000064}`" /f /reg:64"
    if ($MidiDllPath -and (Test-Path $MidiDllPath)) {
        $ElevatedCmd = "regsvr32.exe /u /s `"$MidiDllPath`" & " + $ElevatedCmd
    }
    if ($AsioDllPath -and (Test-Path $AsioDllPath)) {
        $ElevatedCmd = "regsvr32.exe /u /s `"$AsioDllPath`" & " + $ElevatedCmd
    }
    Start-Process -FilePath "cmd.exe" -ArgumentList "/c $ElevatedCmd" -Verb RunAs -WindowStyle Hidden -Wait
}

if (-not (Test-Path $RegKeyHKLM) -and -not (Test-Path $RegKeyHKCU) -and -not (Test-Path $ClsidKeyHKLM) -and -not (Test-Path $ClsidKeyHKCU)) {
    Write-Host "[OK] Drivers ASIO y WinMM MIDI desregistrados completamente del Registro." -ForegroundColor Green
} else {
    Write-Warning "El proceso finalizo pero alguna clave permanece en el Registro ($RegKeyHKLM). Asegurese de aceptar la ventana UAC."
}
