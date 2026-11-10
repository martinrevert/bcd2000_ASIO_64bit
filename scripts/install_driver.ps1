param (
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DllPath = (Resolve-Path (Join-Path $ScriptDir "..\build\$Config\bcd2000_asio.dll") -ErrorAction SilentlyContinue).Path

if (-not $DllPath -or -not (Test-Path $DllPath)) {
    Write-Error "El archivo ..\build\$Config\bcd2000_asio.dll no existe. Realice la compilación del proyecto antes de registrar."
    exit 1
}

Write-Host "Registrando servidor COM ASIO en 64 bits: $DllPath" -ForegroundColor Cyan

# Intentar registro directo primero (funciona en HKCU y en HKLM si la consola ya es Admin)
& regsvr32.exe /s "$DllPath"

$RegKeyHKLM = "HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$RegKeyHKCU = "HKCU:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"

if (-not (Test-Path $RegKeyHKLM)) {
    try {
        Start-Process -FilePath "regsvr32.exe" -ArgumentList "/s `"$DllPath`"" -Verb RunAs -Wait
    } catch {
        Write-Warning "No se elevó UAC para HKLM; verificando registro en HKCU..."
    }
}

if (Test-Path $RegKeyHKLM) {
    Write-Host "[OK] Driver registrado exitosamente en HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit." -ForegroundColor Green
} elseif (Test-Path $RegKeyHKCU) {
    Write-Host "[OK] Driver registrado exitosamente en HKCU:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit." -ForegroundColor Green
} else {
    Write-Warning "El proceso finalizó pero la clave de registro no se encuentra en $RegKeyHKLM ni en $RegKeyHKCU."
}
