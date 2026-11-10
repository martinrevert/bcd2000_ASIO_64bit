param (
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DllCandidate = Join-Path $ScriptDir "..\build\$Config\bcd2000_asio.dll"
$DllPath = (Resolve-Path $DllCandidate -ErrorAction SilentlyContinue).Path

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "   Instalador del Servidor COM ASIO 64-bit (Behringer BCD2000)" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

if (-not $DllPath -or -not (Test-Path $DllPath)) {
    Write-Error "El archivo $DllCandidate no existe. Realice la compilacion del proyecto antes de registrar."
    exit 1
}

Write-Host "[1/2] Registrando servidor COM ASIO en 64 bits: $DllPath" -ForegroundColor Cyan
& regsvr32.exe /s "$DllPath"

$RegKeyHKLM = "HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$RegKeyHKCU = "HKCU:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"

if (-not (Test-Path $RegKeyHKLM)) {
    Write-Host "[2/2] Solicitando elevacion UAC para registrar en HKLM:\SOFTWARE\ASIO..." -ForegroundColor Yellow
    try {
        Start-Process -FilePath "regsvr32.exe" -ArgumentList "/s `"$DllPath`"" -Verb RunAs -Wait
    } catch {
        Write-Warning "No se elevo UAC para HKLM; verificando registro en HKCU..."
    }
} else {
    Write-Host "[2/2] Clave en HKLM verificada directamente." -ForegroundColor Cyan
}

if (Test-Path $RegKeyHKLM) {
    Write-Host "[OK] Driver registrado exitosamente en HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit." -ForegroundColor Green
} elseif (Test-Path $RegKeyHKCU) {
    Write-Host "[OK] Driver registrado exitosamente en HKCU:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit." -ForegroundColor Green
} else {
    Write-Warning "El proceso finalizo pero la clave de registro no se encuentra en $RegKeyHKLM ni en $RegKeyHKCU."
}
