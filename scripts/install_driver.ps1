param (
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$AsioCandidate = Join-Path $ScriptDir "..\build\$Config\bcd2000_asio.dll"
$MidiCandidate = Join-Path $ScriptDir "..\build\$Config\bcd2000_midi.dll"
$AsioDllPath = (Resolve-Path $AsioCandidate -ErrorAction SilentlyContinue).Path
$MidiDllPath = (Resolve-Path $MidiCandidate -ErrorAction SilentlyContinue).Path

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "   Instalador Driver ASIO COM + WinMM MIDI 64-bit (Behringer BCD2000)" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

if (-not $AsioDllPath -or -not (Test-Path $AsioDllPath)) {
    Write-Error "El archivo $AsioCandidate no existe. Realice la compilacion del proyecto antes de registrar."
    exit 1
}

$HasMidiDll = ($MidiDllPath -and (Test-Path $MidiDllPath))
$Drivers32Key = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32"

function Test-MidiRegistered {
    $props = Get-ItemProperty -Path $Drivers32Key -ErrorAction SilentlyContinue
    if (-not $props) { return $false }
    foreach ($i in 0..9) {
        $name = if ($i -eq 0) { "midi" } else { "midi$i" }
        $val = $props.$name
        if ($val -and ($val -like "*bcd2000_midi.dll")) {
            return $name
        }
    }
    return $null
}

$IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($IsAdmin) {
    Write-Host "[1/2] Registrando servidor COM ASIO 64-bit: $AsioDllPath" -ForegroundColor Cyan
    & regsvr32.exe /s "$AsioDllPath"
    if ($HasMidiDll) {
        Write-Host "[2/2] Registrando driver WinMM MIDI 64-bit: $MidiDllPath" -ForegroundColor Cyan
        & regsvr32.exe /s "$MidiDllPath"
    }
} else {
    Write-Host "[1/2] Registrando ASIO en HKCU (sin elevacion)..." -ForegroundColor Cyan
    & regsvr32.exe /s "$AsioDllPath"
    Write-Host "[2/2] Solicitando elevacion UAC para HKLM (ASIO + Drivers32 MIDI)..." -ForegroundColor Yellow
    $Cmd = "regsvr32.exe /s `"$AsioDllPath`""
    if ($HasMidiDll) {
        $Cmd = "$Cmd & regsvr32.exe /s `"$MidiDllPath`""
    }
    try {
        Start-Process -FilePath "cmd.exe" -ArgumentList "/c $Cmd" -Verb RunAs -WindowStyle Hidden -Wait
    } catch {
        Write-Warning "No se concedio elevacion UAC para HKLM."
    }
}

$RegKeyHKLM = "HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$RegKeyHKCU = "HKCU:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"

if (Test-Path $RegKeyHKLM) {
    Write-Host "[OK] ASIO registrado en $RegKeyHKLM" -ForegroundColor Green
} elseif (Test-Path $RegKeyHKCU) {
    Write-Host "[OK] ASIO registrado en $RegKeyHKCU" -ForegroundColor Green
} else {
    Write-Warning "La clave ASIO no aparece en $RegKeyHKLM ni en $RegKeyHKCU."
}

if ($HasMidiDll) {
    $midiSlot = Test-MidiRegistered
    if ($midiSlot) {
        Write-Host "[OK] WinMM MIDI registrado en $Drivers32Key ($midiSlot)." -ForegroundColor Green
    } else {
        Write-Warning "bcd2000_midi.dll no aparece aun en $Drivers32Key (requiere elevacion UAC)."
    }
}
