<#
 Behringer BCD2000 64-bit ASIO and MIDI Driver Suite
 Copyright (C) 2024-2026 Martin Revert <martinrevert@gmail.com> and contributors

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <https://www.gnu.org/licenses/>.

 SPDX-License-Identifier: GPL-3.0-or-later
#>

param (
    [string]$Config = "RelWithDebInfo",
    [switch]$ElevatedChild
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$AsioCandidate = Join-Path $ScriptDir "..\build\$Config\bcd2000_asio.dll"
$MidiCandidate = Join-Path $ScriptDir "..\build\$Config\bcd2000_midi.dll"
$AsioDllPath = (Resolve-Path $AsioCandidate -ErrorAction SilentlyContinue).Path
$MidiDllPath = (Resolve-Path $MidiCandidate -ErrorAction SilentlyContinue).Path
$System32MidiPath = Join-Path $env:WINDIR "System32\bcd2000_midi.dll"

$RegKeyHKLM   = "HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$RegKeyHKCU   = "HKCU:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$ClsidKeyHKLM = "HKLM:\SOFTWARE\Classes\CLSID\{B2D20000-1397-00BD-A510-000000000064}"
$ClsidKeyHKCU = "HKCU:\SOFTWARE\Classes\CLSID\{B2D20000-1397-00BD-A510-000000000064}"
$Drivers32Key = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32"

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "   Desinstalador Driver ASIO COM + WinMM MIDI 64-bit (BCD2000)" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

# Limpiar claves ASIO en HKCU (no requiere elevacion)
if (Test-Path $RegKeyHKCU) {
    Remove-Item -Path $RegKeyHKCU -Recurse -Force -ErrorAction SilentlyContinue
}
if (Test-Path $ClsidKeyHKCU) {
    Remove-Item -Path $ClsidKeyHKCU -Recurse -Force -ErrorAction SilentlyContinue
}

$IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($IsAdmin) {
    if ($AsioDllPath -and (Test-Path $AsioDllPath)) {
        Start-Process -FilePath "$env:WINDIR\System32\regsvr32.exe" -ArgumentList "/u /s `"$AsioDllPath`"" -Wait | Out-Null
    }
    if (Test-Path $System32MidiPath) {
        Start-Process -FilePath "$env:WINDIR\System32\regsvr32.exe" -ArgumentList "/u /s `"$System32MidiPath`"" -Wait | Out-Null
        Remove-Item -Path $System32MidiPath -Force -ErrorAction SilentlyContinue
    } elseif ($MidiDllPath -and (Test-Path $MidiDllPath)) {
        Start-Process -FilePath "$env:WINDIR\System32\regsvr32.exe" -ArgumentList "/u /s `"$MidiDllPath`"" -Wait | Out-Null
    }
    if (Test-Path $RegKeyHKLM) {
        Remove-Item -Path $RegKeyHKLM -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $ClsidKeyHKLM) {
        Remove-Item -Path $ClsidKeyHKLM -Recurse -Force -ErrorAction SilentlyContinue
    }
} else {
    $HasMidiReg = Test-Path $System32MidiPath
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
    if ((Test-Path $RegKeyHKLM) -or (Test-Path $ClsidKeyHKLM) -or $HasMidiReg) {
        if (-not $ElevatedChild) {
            Write-Host "Solicitando elevacion UAC para desinstalar HKLM y System32..." -ForegroundColor Yellow
            $Ps64 = Join-Path $env:WINDIR "System32\WindowsPowerShell\v1.0\powershell.exe"
            $ElevArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Config `"$Config`" -ElevatedChild"
            Start-Process -FilePath $Ps64 -ArgumentList $ElevArgs -Verb RunAs -Wait | Out-Null
        }
    }
}

if (-not (Test-Path $RegKeyHKLM) -and -not (Test-Path $RegKeyHKCU) -and -not (Test-Path $ClsidKeyHKLM) -and -not (Test-Path $ClsidKeyHKCU)) {
    Write-Host "[OK] Drivers ASIO y WinMM MIDI desregistrados completamente." -ForegroundColor Green
} else {
    Write-Warning "Alguna clave permanece en el Registro ($RegKeyHKLM). Asegurese de aceptar UAC."
}
