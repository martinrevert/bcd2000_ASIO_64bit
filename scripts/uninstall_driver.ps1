param (
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DllCandidate = Join-Path $ScriptDir "..\build\$Config\bcd2000_asio.dll"
$DllPath = (Resolve-Path $DllCandidate -ErrorAction SilentlyContinue).Path

$RegKeyHKLM   = "HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$RegKeyHKCU   = "HKCU:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$ClsidKeyHKLM = "HKLM:\SOFTWARE\Classes\CLSID\{B2D20000-1397-00BD-A510-000000000064}"
$ClsidKeyHKCU = "HKCU:\SOFTWARE\Classes\CLSID\{B2D20000-1397-00BD-A510-000000000064}"

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host "   Desinstalador del Servidor COM ASIO 64-bit (Behringer BCD2000)" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

if ($DllPath -and (Test-Path $DllPath)) {
    Write-Host "[1/3] Ejecutando regsvr32 /u sobre: $DllPath" -ForegroundColor Yellow
    & regsvr32.exe /u /s "$DllPath"
} else {
    Write-Host "[1/3] Binario no encontrado en $DllCandidate; limpiando claves de Registro directamente..." -ForegroundColor Yellow
}

# Limpiar claves en HKCU (no requiere elevacion)
Write-Host "[2/3] Verificando y limpiando claves en HKEY_CURRENT_USER..." -ForegroundColor Yellow
if (Test-Path $RegKeyHKCU) {
    Remove-Item -Path $RegKeyHKCU -Recurse -Force -ErrorAction SilentlyContinue
}
if (Test-Path $ClsidKeyHKCU) {
    Remove-Item -Path $ClsidKeyHKCU -Recurse -Force -ErrorAction SilentlyContinue
}

# Verificar y limpiar claves en HKLM (requiere elevacion UAC si la consola no es Admin)
Write-Host "[3/3] Verificando y limpiando claves en HKEY_LOCAL_MACHINE..." -ForegroundColor Yellow
if ((Test-Path $RegKeyHKLM) -or (Test-Path $ClsidKeyHKLM)) {
    Write-Host "      Solicitando elevacion UAC para eliminar claves de HKLM:\SOFTWARE\ASIO..." -ForegroundColor Cyan
    $ElevatedCmd = "reg.exe delete `"HKLM\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit`" /f /reg:64 & reg.exe delete `"HKLM\SOFTWARE\Classes\CLSID\{B2D20000-1397-00BD-A510-000000000064}`" /f /reg:64"
    if ($DllPath -and (Test-Path $DllPath)) {
        $ElevatedCmd = "regsvr32.exe /u /s `"$DllPath`" & " + $ElevatedCmd
    }
    Start-Process -FilePath "cmd.exe" -ArgumentList "/c $ElevatedCmd" -Verb RunAs -WindowStyle Hidden -Wait
}

if (-not (Test-Path $RegKeyHKLM) -and -not (Test-Path $RegKeyHKCU) -and -not (Test-Path $ClsidKeyHKLM) -and -not (Test-Path $ClsidKeyHKCU)) {
    Write-Host "[OK] Driver desregistrado y eliminado completamente del Registro de Windows." -ForegroundColor Green
} else {
    Write-Warning "El proceso finalizo pero alguna clave permanece en el Registro ($RegKeyHKLM). Asegurese de aceptar la ventana UAC."
}
