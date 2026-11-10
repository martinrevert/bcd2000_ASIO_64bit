param (
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DllCandidate = Join-Path $ScriptDir "..\build\$Config\bcd2000_asio.dll"
$DllPath = (Resolve-Path $DllCandidate -ErrorAction SilentlyContinue).Path

$RegKeyHKLM = "HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
$RegKeyHKCU = "HKCU:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"

if ($DllPath -and (Test-Path $DllPath)) {
    Write-Host "Desregistrando servidor COM ASIO: $DllPath" -ForegroundColor Yellow

    # 1. Intentar desregistro directo en la sesión actual
    & regsvr32.exe /u /s "$DllPath"

    # 2. Si la clave en HKLM sigue existiendo (consola sin privilegios de Administrador), elevar vía UAC
    if (Test-Path $RegKeyHKLM) {
        Write-Host "Elevando permisos (UAC) para eliminar las claves de HKLM:\SOFTWARE\ASIO..." -ForegroundColor Cyan
        Start-Process -FilePath "regsvr32.exe" -ArgumentList "/u /s `"$DllPath`"" -Verb RunAs -Wait
    }

    if (-not (Test-Path $RegKeyHKLM) -and -not (Test-Path $RegKeyHKCU)) {
        Write-Host "[OK] Driver eliminado completamente de SOFTWARE\ASIO y HKCR\CLSID." -ForegroundColor Green
    } else {
        Write-Warning "La clave aún permanece en el Registro ($RegKeyHKLM). Asegúrese de aceptar la ventana UAC."
    }
} else {
    Write-Host "No se encontró el binario en $DllCandidate para desregistrar." -ForegroundColor DarkGray
}
