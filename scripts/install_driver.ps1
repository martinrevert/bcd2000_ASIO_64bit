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

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "   Instalador Driver ASIO COM + WinMM MIDI 64-bit (Behringer BCD2000)" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "Configuracion: $Config" -ForegroundColor Gray

if (-not $AsioDllPath -or -not (Test-Path $AsioDllPath)) {
    Write-Error "El archivo $AsioCandidate no existe. Realice la compilacion del proyecto antes de registrar."
    exit 1
}

$HasMidiDll = ($MidiDllPath -and (Test-Path $MidiDllPath))
$Drivers32Key = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32"
$System32MidiPath = Join-Path $env:WINDIR "System32\bcd2000_midi.dll"

function Test-MidiRegistered {
    $props = Get-ItemProperty -Path $Drivers32Key -ErrorAction SilentlyContinue
    if (-not $props) { return $null }
    foreach ($i in 0..9) {
        $name = if ($i -eq 0) { "midi" } else { "midi$i" }
        $val = $props.$name
        if ($val -and ($val -like "*bcd2000_midi.dll")) {
            return "$name = $val"
        }
    }
    return $null
}

$IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($IsAdmin) {
    Write-Host "[1/2] Registrando servidor COM ASIO 64-bit: $AsioDllPath" -ForegroundColor Cyan
    $p1 = Start-Process -FilePath "$env:WINDIR\System32\regsvr32.exe" -ArgumentList "/s `"$AsioDllPath`"" -Wait -PassThru
    if ($p1.ExitCode -ne 0) {
        Write-Warning "regsvr32 ASIO devolvio codigo $($p1.ExitCode)."
    }

    if ($HasMidiDll) {
        Write-Host "[2/2] Copiando e instalando driver WinMM MIDI 64-bit en System32..." -ForegroundColor Cyan
        Copy-Item -Path $MidiDllPath -Destination $System32MidiPath -Force
        $p2 = Start-Process -FilePath "$env:WINDIR\System32\regsvr32.exe" -ArgumentList "/s `"$System32MidiPath`"" -Wait -PassThru
        if ($p2.ExitCode -ne 0) {
            Write-Warning "regsvr32 MIDI devolvio codigo $($p2.ExitCode)."
        }
    }
} else {
    Write-Host "[1/2] Registrando ASIO en HKCU (sin elevacion)..." -ForegroundColor Cyan
    Start-Process -FilePath "$env:WINDIR\System32\regsvr32.exe" -ArgumentList "/s `"$AsioDllPath`"" -Wait | Out-Null

    if (-not $ElevatedChild) {
        Write-Host "[2/2] Solicitando elevacion UAC para HKLM y System32 (ASIO + Drivers32 MIDI)..." -ForegroundColor Yellow
        $Ps64 = Join-Path $env:WINDIR "System32\WindowsPowerShell\v1.0\powershell.exe"
        $ElevArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Config `"$Config`" -ElevatedChild"
        try {
            $proc = Start-Process -FilePath $Ps64 -ArgumentList $ElevArgs -Verb RunAs -Wait -PassThru
            if ($proc.ExitCode -ne 0) {
                Write-Warning "El instalador elevado finalizo con codigo $($proc.ExitCode)."
            }
        } catch {
            Write-Warning "No se concedio elevacion UAC: $($_.Exception.Message)"
        }
    }
}

$RegKeyHKLM = "HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$RegKeyHKCU = "HKCU:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"

if (Test-Path $RegKeyHKLM) {
    Write-Host "[OK] ASIO registrado en $RegKeyHKLM" -ForegroundColor Green
} elseif (Test-Path $RegKeyHKCU) {
    Write-Warning "ASIO solo se registro en HKCU (no HKLM); Ableton/DAWs pueden requerir HKLM."
} else {
    Write-Warning "La clave ASIO no aparece en $RegKeyHKLM ni en $RegKeyHKCU."
}

if ($HasMidiDll) {
    $midiSlot = Test-MidiRegistered
    if ($midiSlot) {
        Write-Host "[OK] WinMM MIDI registrado en $Drivers32Key ($midiSlot)." -ForegroundColor Green
    } else {
        Write-Warning "bcd2000_midi.dll no aparece en $Drivers32Key (requiere aceptar elevacion UAC)."
    }
}
