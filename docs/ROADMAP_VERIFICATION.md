# Especificación Técnica y Roadmap de Verificación: Driver ASIO Moderno de 64 bits para Behringer BCD2000

Este documento define la arquitectura técnica, el inventario de herramientas de desarrollo (BOM) y el protocolo de pruebas funcionales incrementales para la construcción de un controlador ASIO de 64 bits para la consola Behringer BCD2000 utilizando su firmware de fábrica original.

---

## 1. Arquitectura del Sistema y Parámetros de Hardware

El proyecto implementa un modelo de controlador de audio en modo usuario (User-Mode Audio Driver) desacoplado del espacio de kernel, evitando los requerimientos de certificados extendidos de firma digital (EV/WHQL) en versiones modernas de Windows de 64 bits.

### Especificaciones Base del Dispositivo
* Identificador de Fabricante (VID): `0x1397`
* Identificador de Producto (PID): `0x00BD`
* Bus y Velocidad: USB 1.1 Full-Speed (12 Mbps)
* Microcontrolador Interno: Núcleo 8051 (firmware propietario de fábrica con descriptores no compatibles con USB Audio Class estándar ni USB-MIDI)
* Capa de Enlace Físico: Interfaz USB de acceso directo gestionada mediante el controlador nativo de Windows `WinUSB.sys`
* Capa de Audio Profesional: Librería dinámica de 64 bits (`bcd2000_asio.dll`) que expone la interfaz COM del Steinberg ASIO SDK 2.3

### Gestión de Ancho de Banda y Limitaciones Físicas
La experiencia documentada en el controlador Linux `snd-bcd2000` evidencia que el bus USB Full-Speed impone restricciones severas de ancho de banda. Cuando se solicita simultáneamente la captura y reproducción dúplex a altas tasas de transferencia, el planificador USB puede fallar al reservar ancho de banda isócrono. Para maximizar la estabilidad y reducir la latencia, la arquitectura prioriza una configuración de salida estéreo dual (Canales 1-2 Master, Canales 3-4 Auriculares), manteniendo el subsistema de entrada analógica como un módulo condicional.

---

## 2. Bill of Materials (BOM) para Antigravity IDE y Windows x64

Para que Antigravity IDE pueda compilar, enlazar, depurar y ejecutar las pruebas de hardware, el entorno del sistema operativo debe contar con las siguientes herramientas y librerías:

### Componentes de Compilación y SDKs del Sistema
* Visual Studio Build Tools 2022 o superior:
  * Compilador de C++ (`cl.exe`) configurado para destino `x64`
  * Conjunto de herramientas compatible con C++17 o C++20
* Windows 10/11 SDK (versión 10.0.19041.0 o superior):
  * Cabeceras del sistema y soporte COM: `windows.h`, `ole2.h`
  * API y librerías de enlace WinUSB: `winusb.h`, `winusb.lib`
  * Enumeración de dispositivos USB: `setupapi.h`, `setupapi.lib`
* Sistema de Compilación:
  * CMake 3.20 o superior
  * Generador Ninja o MSBuild (x64)

### Componentes de Audio y Librerías de Terceros
* Steinberg ASIO SDK versión 2.3:
  * Cabecera principal de interfaz COM: `common/iasiodrv.h`
  * Implementación base del driver: `host/asiodrvr.h`, `asiosmpl.cpp`
  * Rutinas de registro en el sistema: `register.cpp`
* Microsoft Low-Latency Audio Reference:
  * Patrones de asignación de hilos y priorización multimedia mediante el servicio MMCSS (Multimedia Class Scheduler Service)

### Herramientas de Diagnóstico y Hardware
* Zadig (o utilidad basada en libwdi): Para asociar el dispositivo físico (`1397:00bd`) al driver `WinUSB` sin requerir paquetes de catálogo firmados durante la fase de desarrollo.
* Wireshark con soporte UsbPcap: Para monitoreo en vivo de los paquetes URB transferidos en los pipes USB.
* Software de prueba:
  * `hostsample.exe` (compilado en x64 desde el SDK de Steinberg)
  * DAW o software DJ x64 (Reaper, Mixxx o Traktor)

---

## 3. Matriz de Hitos de Verificación Incremental

El desarrollo se estructura en cinco hitos secuenciales. Cada hito produce un binario ejecutable independiente diseñado para aislar y validar un subsistema específico antes de avanzar al siguiente nivel de complejidad.

### Hito 1: Enumeración e Inicialización USB
* Binario: `bcd2000_diag_usb.exe`
* Objetivo: Detectar la consola física mediante la API de configuración de dispositivos de Windows, abrir los identificadores de WinUSB y transmitir la trama de inicialización documentada en la ingeniería inversa de `snd-bcd2000`.
* Criterio de Aceptación:
  1. El programa localiza la ruta del dispositivo asociada al GUID de interfaz WinUSB.
  2. `WinUsb_Initialize` retorna código de éxito sobre el handle del dispositivo.
  3. Se envía el paquete de control de inicialización sin errores de pipe stall.
  4. La consola responde encendiendo su secuencia inicial de LEDs.
* Ejecución de Prueba:
```powershell
cmake --build build --config RelWithDebInfo --target bcd2000_diag_usb
.\build\RelWithDebInfo\bcd2000_diag_usb.exe
```

### Hito 2: Telemetría de Controles Físicos y Retorno a LEDs
* Binario: `bcd2000_diag_midi.exe`
* Objetivo: Implementar un bucle de lectura asíncrono sobre el endpoint de entrada de control para procesar cambios en faders, potenciómetros, ruedas jog y botones, así como enviar paquetes de actualización de estado hacia los LEDs indicadores.
* Criterio de Aceptación:
  1. Cada movimiento de fader o perilla imprime en la consola estándar el canal, control ID y valor correspondiente (rango 0 a 127).
  2. La pulsación de botones (como Play o Cue) produce eventos inmediatos sin pérdida de paquetes.
  3. El envío de comandos de salida activa y desactiva los LEDs de los pulsadores correspondientes en el hardware.
* Ejecución de Prueba:
```powershell
cmake --build build --config RelWithDebInfo --target bcd2000_diag_midi
.\build\RelWithDebInfo\bcd2000_diag_midi.exe
```

### Hito 3: Transmisión PCM y Síntesis de Tono Senoidal
* Binario: `bcd2000_diag_audio.exe`
* Objetivo: Validar la capa de transporte de audio transmitiendo tramas de datos PCM lineales directamente a los endpoints de salida de la BCD2000 mediante transferencias continuas de WinUSB.
* Criterio de Aceptación:
  1. El programa sintetiza internamente una onda senoidal pura a 440 Hz (16 bits estéreo, 44.1 kHz).
  2. Los buffers se envían de forma continua asegurando que no existan discontinuidades temporales o caídas de buffer.
  3. El tono es claramente audible a través de la salida Master RCA y la salida frontal de auriculares sin artefactos digitales ni chasquidos.
* Ejecución de Prueba:
```powershell
cmake --build build --config RelWithDebInfo --target bcd2000_diag_audio
.\build\RelWithDebInfo\bcd2000_diag_audio.exe --frequency 440 --duration 10
```

### Hito 4: Servidor COM ASIO 64-bit y Test de Handshake
* Binario: `bcd2000_asio.dll`
* Objetivo: Empaquetar la lógica de transporte validada en una librería dinámica que implemente la interfaz `IASIO` de Steinberg, registre las claves necesarias en el Registro de Windows y complete el ciclo de callbacks con un anfitrión de prueba.
* Criterio de Aceptación:
  1. Compilación exitosa de la librería dinámica en arquitectura x64.
  2. Registro correcto en `HKEY_LOCAL_MACHINE\SOFTWARE\ASIO` y `HKEY_CLASSES_ROOT\CLSID`.
  3. El ejecutable oficial `hostsample.exe` (x64) reconoce el driver por su nombre, consulta canales, negocia tamaños de buffer (64 a 512 muestras) y ejecuta el callback `bufferSwitch` de manera estable.
* Ejecución de Prueba:
```powershell
regsvr32 /s .\build\RelWithDebInfo\bcd2000_asio.dll
.\third_party\asio-sdk\host\pc\hostsample.exe
```

### Hito 5: Validación Operativa en Estación de Trabajo de Audio Digital (DAW)
* Binario: `bcd2000_asio.dll` (Versión Release)
* Objetivo: Integrar el controlador en un entorno de mezcla en tiempo real bajo Windows 64 bits.
* Criterio de Aceptación:
  1. Aplicaciones como Reaper, Traktor o Mixxx detectan la BCD2000 en su selector de dispositivos ASIO.
  2. Reproducción multicanal simultánea (monitoreo en auriculares independiente de la salida principal).
  3. Latencia estable de entrada a salida reportada y verificada en el rango de 3 ms a 8 ms sin caídas de audio bajo carga moderada de CPU.

---

## 4. Scripts de Automatización del Ciclo de Desarrollo

Los siguientes scripts permiten registrar, desregistrar y verificar las versiones de la librería DLL compilada en Windows PowerShell durante las iteraciones de trabajo en Antigravity IDE.

### Script de Instalación y Registro: `scripts/install_driver.ps1`
```powershell
param (
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DllPath = Join-Path $ScriptDir "..\build\$Config\bcd2000_asio.dll"

if (-not (Test-Path $DllPath)) {
    Write-Error "El archivo $DllPath no existe. Realice la compilación del proyecto antes de registrar."
    exit 1
}

Write-Host "Registrando servidor COM ASIO en 64 bits: $DllPath" -ForegroundColor Cyan
Start-Process -FilePath "regsvr32.exe" -ArgumentList "/s `"$DllPath`"" -Verb RunAs -Wait

$RegKey = "HKLM:\SOFTWARE\ASIO\Behringer BCD2000 ASIO 64-bit"
if (Test-Path $RegKey) {
    Write-Host "[OK] Driver registrado exitosamente en el Registro de Windows." -ForegroundColor Green
} else {
    Write-Warning "El proceso finalizó pero la clave de registro no se encuentra en $RegKey."
}
```

### Script de Desinstalación: `scripts/uninstall_driver.ps1`
```powershell
param (
    [string]$Config = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DllPath = Join-Path $ScriptDir "..\build\$Config\bcd2000_asio.dll"

if (Test-Path $DllPath) {
    Write-Host "Desregistrando servidor COM ASIO: $DllPath" -ForegroundColor Yellow
    Start-Process -FilePath "regsvr32.exe" -ArgumentList "/u /s `"$DllPath`"" -Verb RunAs -Wait
    Write-Host "[OK] Driver desregistrado del sistema." -ForegroundColor Green
} else {
    Write-Host "No se encontró el binario en $DllPath para desregistrar." -ForegroundColor DarkGray
}
```

---

## 5. Protocolo de Diagnóstico y Mitigación de Errores Comunes

Durante las pruebas de hardware pueden presentarse situaciones anómalas derivadas de la naturaleza del bus USB 1.1 y el estado del microcontrolador:

1. Falla al abrir el pipe WinUSB:
   * Causa: Dispositivo en estado no configurado o driver asociado incorrectamente.
   * Mitigación: Abrir Zadig, habilitar "List All Devices", verificar que el VID sea `1397` y el PID `00BD`, y reasignar el driver específicamente a `WinUSB (v6.1.7600.16385 o superior)`.
2. Saturación del bus o pérdida de sincronización de audio:
   * Causa: Sobrecarga en la tasa de refresco de transferencias isócronas o bulk combinada con tráfico de control.
   * Mitigación: Desactivar transferencias de entrada no esenciales; asegurar que el tamaño del buffer de transferencia en WinUSB sea un múltiplo exacto del tamaño de paquete reportado por el endpoint en los descriptores USB de la consola.
3. Ausencia de eventos en el endpoint de control:
   * Causa: La consola requiere la secuencia inicial de activación previa a la emisión de estados de faders y potenciómetros.
   * Mitigación: Reejecutar el procedimiento de inicialización del Hito 1 antes de conectar el listener de control.
