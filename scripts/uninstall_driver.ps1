param (
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DllPath = Join-Path $ScriptDir "..\build\$Config\bcd2000_asio.dll"

if (Test-Path $DllPath) {
    Write-Host "Desregistrando servidor COM ASIO: $DllPath" -ForegroundColor Yellow
    & regsvr32.exe /u /s "$DllPath"
    try {
        Start-Process -FilePath "regsvr32.exe" -ArgumentList "/u /s `"$DllPath`"" -Verb RunAs -Wait
    } catch {
        # Ignorar si el usuario cancela UAC; el desregistro en HKCU ya se ejecutó
    }
    Write-Host "[OK] Driver desregistrado del sistema." -ForegroundColor Green
} else {
    Write-Host "No se encontró el binario en $DllPath para desregistrar." -ForegroundColor DarkGray
}
