[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) **Español**](README.es.md) | [![PT](flags/br.svg) Português](README.pt.md) | [![DE](flags/de.svg) Deutsch](README.de.md) | [![FR](flags/fr.svg) Français](README.fr.md) | [![ZH](flags/cn.svg) 中文](README.zh.md) | [![JA](flags/jp.svg) 日本語](README.ja.md)

# Behringer BCD2000 — Driver Nativo ASIO y MIDI de 64 bits (Windows 10/11)

**Una nueva suite de drivers open-source de 64 bits que devuelve a la vida a la clásica controladora DJ Behringer BCD2000 en DAWs y software DJ modernos.**

[Documentación Técnica y Arquitectura (TECHNICS.es.md)](TECHNICS.es.md)


---

## ¡Bienvenida de nuevo, BCD2000!

Si todavía tienes una **Behringer B-Control Deejay BCD2000** sobre tu escritorio o guardada en su maleta de transporte, seguro conoces esa sensación agridulce: el hardware sigue funcionando como un tanque —sus faders, potenciómetros de EQ, jog wheels, preamplificador de auriculares y salidas RCA están intactos—, pero los ordenadores modernos la trataban como un pisapapeles.

**Hoy eso cambia.** Este proyecto le da a la BCD2000 algo que Behringer nunca llegó a lanzar: un **verdadero driver de audio ASIO de 64 bits y driver MIDI de 64 bits**, construidos desde cero para **Windows 10 y Windows 11 (x64)**.

Conéctala, ejecuta un solo instalador y vuelve a disfrutar tu BCD2000 en aplicaciones modernas de 64 bits como **Mixxx**, **Ableton Live**, **REAPER**, **FL Studio**, **Traktor** y más.

---

## De dónde venimos y por qué nace esta iniciativa

Behringer lanzó la BCD2000 a mediados de la década de 2000, en la era de Windows XP y Vista, y **dejó el soporte oficial estancado en drivers de 32 bits**. A diferencia de los dispositivos USB Audio Class o USB-MIDI estándar, la BCD2000 utiliza un protocolo propietario en su microcontrolador 8051 (`VID 0x1397`, `PID 0x00BD`). Sin un driver específico, Windows 10 y Windows 11 simplemente no saben cómo comunicarse con ella.

Durante años, quienes conservábamos la consola estábamos condenados a soluciones incómodas: mantener un viejo PC de 32 bits con Windows XP/7, montar máquinas virtuales inestables o resignarnos a dejar en un cajón una controladora que funciona perfectamente.

Esta iniciativa nace de una convicción simple: **el buen hardware no debería convertirse en basura electrónica solo porque el fabricante lo abandonó.** Apoyándonos en la ingeniería inversa documentada por el driver libre de Linux (`snd-bcd2000`) y las APIs USB modernas en modo usuario de Windows, desarrollamos desde cero una pila de drivers nativa de 64 bits para que toda la comunidad BCD2000 pueda volver a mezclar y producir en equipos actuales.

---

## Por qué esto NO es usar viejos drivers con "Modo de Compatibilidad de Windows 7"

Una duda muy habitual es: *"¿Y por qué no instalar el viejo driver de Behringer usando el Modo de Compatibilidad de Windows 7?"*

La respuesta clara es que eso es técnicamente imposible en Windows moderno de 64 bits:

1. **El Modo de Compatibilidad no funciona con drivers de kernel.** La pestaña de compatibilidad de Windows solo ajusta aplicaciones antiguas en modo usuario (`.exe`). Un kernel de Windows de 64 bits **no puede cargar bajo ninguna circunstancia drivers `.sys` de 32 bits**; no existe ningún modo de compatibilidad que lo permita.
2. **Los DAWs de 64 bits no pueden cargar DLLs ASIO de 32 bits.** El software moderno de producción y DJ corre como procesos nativos de 64 bits y exige una librería COM ASIO de 64 bits (`x64`) y puertos MIDI nativos de 64 bits.
3. **Aquí no se reutiliza ni un solo archivo antiguo de Behringer.** Este proyecto **no** parchea ni envuelve el software abandonado de 2006. Es **código C++ de 64 bits 100 % nuevo**, ejecutándose en modo usuario sobre **`WinUSB.sys`** (el driver USB oficial integrado en Windows 10/11). Sin trucos en el kernel, sin desactivar la firma de controladores ni activar el modo de pruebas.

---

## Qué incluye el paquete (lo básico)

* **Audio ASIO Nativo de 64 bits (`Behringer BCD2000 ASIO 64-bit`):**
  * 4 canales de salida: **Master Izquierdo/Derecho (RCA traseros)** + **Auriculares Izquierdo/Derecho (Cue frontal TRS 1/4")** a 44.1 kHz / 16 bits PCM.
  * 4 canales de entrada expuestos vía ASIO (`In 1–4`).
  * Streaming en tiempo real de baja latencia con seguimiento de reloj por hardware para un audio limpio y sin chasquidos.
* **MIDI IN y OUT Simultáneo de 64 bits (`Behringer BCD2000 MIDI`):**
  * Soporte completo para faders, perillas, jog wheels, botones y **retorno bidireccional de LEDs**, funcionando **al mismo tiempo** que el audio ASIO sin conflictos de acceso exclusivo.
* **Instalador en Un Clic (`Behringer_BCD2000_Setup_x64.exe`):**
  * Asocia automáticamente el driver nativo `WinUSB` de Windows a tu BCD2000 (**no** necesitas descargar ni configurar Zadig manualmente).
  * Registra el driver ASIO de 64 bits y el driver MIDI de 64 bits de forma limpia e incluye desinstalador estándar.
* **Preset Seguro para Mixxx Incluido (`Behringer BCD2000 (Driver Safe)`):**
  * Preset opcional para Mixxx 2.x que añade *soft-takeover* y evita que las perillas físicas de EQ o Master dejadas en cero silencien el audio al abrir Mixxx.

---

## Guía Rápida de Instalación y Uso

### Descargas y Artefactos de Release
Descarga la última versión verificada desde la sección oficial de **[GitHub Releases](https://github.com/martinrevert/bcd2000_ASIO_64bit/releases)**:
* **`Behringer_BCD2000_Setup_x64.exe`**: Instalador único automatizado recomendado (incluye driver WinUSB, DLLs ASIO/MIDI y preset para Mixxx).
* **`bcd2000_v2.x.x_binaries_x64.zip`**: Paquete portable independiente para instalaciones manuales o avanzadas.
* **`SHA256SUMS.txt`**: Sumas criptográficas SHA-256 para verificar la integridad de las descargas.

### Requisitos
* PC con **Windows 10 o Windows 11 (64 bits / x64)**.
* **Behringer BCD2000** conectada preferiblemente a un puerto USB directo (evita hubs USB sin alimentación).

### Pasos de Instalación
1. **Cierra** cualquier DAW o software DJ que tengas abierto (Mixxx, Ableton Live, REAPER, etc.).
2. Ejecuta **`Behringer_BCD2000_Setup_x64.exe`** como Administrador y sigue el asistente.
   * *(Opcional)* Marca la opción del preset seguro si utilizas Mixxx.
3. Si la BCD2000 ya estaba conectada durante la instalación, **desconecta y vuelve a conectar el cable USB** una vez para que Windows aplique el enlace limpiamente.
4. Abre tu DAW o programa DJ y selecciona:
   * **Dispositivo de Audio (ASIO):** `Behringer BCD2000 ASIO 64-bit`
   * **Controlador MIDI (IN y OUT):** `Behringer BCD2000 MIDI`
5. **Consejo para usuarios de Mixxx:** Elige el mapeo **`Behringer BCD2000 (Driver Safe)`** incluido en el instalador. Si usas el preset original de fábrica de Mixxx, recuerda dejar las perillas físicas de EQ (Low / Mid / High) en el centro antes de activar el controlador.

---

## Para Desarrolladores y Colaboradores

¿Quieres ver el diagrama de arquitectura, conocer cómo resolvimos la deriva de reloj USB 1.1 y el uso simultáneo de audio+MIDI sobre WinUSB, compilar desde el código fuente o colaborar en el proyecto?

👉 Consulta la documentación técnica completa en **[TECHNICS.es.md](TECHNICS.es.md)**.
🤖 Los asistentes de IA y agentes de código deben seguir **[AGENTS.md](../../AGENTS.md)**.

---

## Licencia y Marco Legal

Este proyecto es software libre distribuido bajo la licencia **[GNU General Public License v3 o posterior (GPL-3.0-or-later)](../../LICENSE)** con la **Excepción de Vinculación para Hosts y DAWs** (Sección 7 de GPLv3).

* **Compatibilidad con DAWs y Programas DJ:** Se otorga permiso expreso para vincular y cargar `bcd2000_asio.dll` y `bcd2000_midi.dll` desde cualquier software host de audio (Mixxx, Ableton Live, FL Studio, REAPER, Traktor, etc.) sin importar los términos de licencia del host.
* **Encabezados ASIO Clean-Room:** Las definiciones de interfaz ASIO incluidas son declaraciones libres e independientes de la ABI pública COM, desarrolladas para interoperabilidad binaria sin contener código privativo del SDK de Steinberg.
* **Aviso de Marca:** ASIO es una marca registrada y software de Steinberg Media Technologies GmbH.
* **Autores y Agradecimientos:** Consulta [AUTHORS](../../AUTHORS) para la lista de autores y reconocimientos de ingeniería inversa (incluyendo la investigación pionera de Mario Kicherer en Linux ALSA y el equipo de Mixxx).

---

[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) **Español**](README.es.md) | [![PT](flags/br.svg) Português](README.pt.md) | [![DE](flags/de.svg) Deutsch](README.de.md) | [![FR](flags/fr.svg) Français](README.fr.md) | [![ZH](flags/cn.svg) 中文](README.zh.md) | [![JA](flags/jp.svg) 日本語](README.ja.md)
