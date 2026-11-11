[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) Español](README.es.md) | [![PT](flags/br.svg) Português](README.pt.md) | [![DE](flags/de.svg) **Deutsch**](README.de.md) | [![FR](flags/fr.svg) Français](README.fr.md) | [![ZH](flags/cn.svg) 中文](README.zh.md) | [![JA](flags/jp.svg) 日本語](README.ja.md)

# Behringer BCD2000 — Nativer 64-Bit-ASIO- & MIDI-Treiber (Windows 10/11)

**Eine brandneue, quelloffene 64-Bit-Treibersuite, die den klassischen Behringer BCD2000 DJ-Controller auf modernen 64-Bit-DAWs und DJ-Programmen wieder zum Leben erweckt.**

[Technische Dokumentation & Architektur (TECHNICS.de.md)](TECHNICS.de.md)


---

## Willkommen zurück, BCD2000!

Wenn du noch eine **Behringer B-Control Deejay BCD2000** auf dem Schreibtisch stehen oder gut verpackt im Case liegen hast, kennst du das bittersüße Gefühl: Die Hardware ist gebaut wie ein Panzer — Fader, EQ-Potis, Jog-Wheels, Kopfhörervorverstärker und Cinch-Ausgänge funktionieren tadellos —, doch moderne Rechner behandelten sie wie einen Briefbeschwerer.

**Das ändert sich heute.** Dieses Projekt gibt der BCD2000 etwas, das Behringer selbst nie veröffentlicht hat: einen **echten, nativen 64-Bit-ASIO-Audiotreiber und 64-Bit-MIDI-Treiber**, von Grund auf neu entwickelt für **Windows 10 und Windows 11 (x64)**.

Einfach anschließen, ein einziges Setup ausführen und deine BCD2000 wieder in moderner 64-Bit-Software wie **Mixxx**, **Ableton Live**, **REAPER**, **FL Studio**, **Traktor** und vielen mehr nutzen.

---

## Woher wir kommen & warum es diese Initiative gibt

Behringer brachte die BCD2000 Mitte der 2000er-Jahre in der Windows-XP-/Vista-Ära auf den Markt und **beendete den offiziellen Treibersupport bei reinen 32-Bit-Windows-Treibern**. Im Gegensatz zu standardkonformen USB-Audio- oder USB-MIDI-Geräten nutzt die BCD2000 ein proprietäres Protokoll auf ihrem 8051-Mikrocontroller (`VID 0x1397`, `PID 0x00BD`). Ohne passenden Treiber können Windows 10 und Windows 11 schlicht nicht mit ihr kommunizieren.

Jahrelang blieben Besitzern nur unbequeme Umwege: einen alten 32-Bit-PC mit Windows XP/7 bereithalten, wackelige virtuelle Maschinen einrichten oder ein einwandfrei funktionierendes Gerät aufgeben.

Diese Initiative entstand aus einer einfachen Überzeugung: **Gute Hardware darf kein Elektroschrott werden, nur weil der Hersteller den Support eingestellt hat.** Aufbauend auf den Reverse-Engineering-Erkenntnissen des freien Linux-ALSA-Treibers (`snd-bcd2000`) und modernen Windows-User-Mode-USB-APIs haben wir einen sauberen 64-Bit-Treiberstack von Grund auf neu programmiert, damit die BCD2000-Community wieder auf aktuellen Systemen mixen und produzieren kann.

---

## Warum das KEIN „alter Treiber im Windows-7-Kompatibilitätsmodus“ ist

Eine sehr häufige Frage lautet: *„Warum installiert man nicht einfach den alten Behringer-Treiber im Windows-7-Kompatibilitätsmodus?“*

Hier ist die klare technische Antwort:

1. **Der Kompatibilitätsmodus gilt nicht für Kernel-Treiber.** Der Windows-Kompatibilitätsmodus passt ausschließlich alte Benutzerprogramme (`.exe`) an. Ein 64-Bit-Windows-Kernel **kann grundsätzlich keine 32-Bit-`.sys`-Kernel-Treiber laden** — es gibt keinen Schalter, der das ermöglicht.
2. **64-Bit-DAWs können keine 32-Bit-ASIO-DLLs laden.** Moderne Audio- und DJ-Software läuft nativ in 64 Bit und benötigt zwingend eine native 64-Bit-COM-ASIO-Bibliothek (`x64`) sowie native 64-Bit-MIDI-Ports.
3. **Hier steckt kein einziger alter Behringer-Code.** Dieses Projekt patcht oder verpackt **keine** veraltete Software von 2006. Es besteht zu **100 % aus neuem 64-Bit-C++-Code**, der sicher im Windows-User-Mode auf Microsofts integriertem **`WinUSB.sys`** läuft. Keine Kernel-Hacks, kein Deaktivieren der Treibersignaturpflicht und kein Test-Signing-Modus nötig.

---

## Was du bekommst (die Grundlagen)

* **Natives 64-Bit-ASIO-Audio (`Behringer BCD2000 ASIO 64-bit`):**
  * 4 Ausgangskanäle: **Master Links/Rechts (Cinch-Rückseite)** + **Kopfhörer Links/Rechts (Front-Cue 6,3-mm-Klinke)** mit 44,1 kHz / 16-Bit PCM.
  * 4 über ASIO bereitgestellte Eingangskanäle (`In 1–4`).
  * Echtzeit-Streaming mit niedriger Latenz und Hardware-Taktverfolgung für klaren, knackfreien Klang.
* **Gleichzeitiges 64-Bit-MIDI IN & OUT (`Behringer BCD2000 MIDI`):**
  * Volle Unterstützung für Fader, Regler, Jog-Wheels, Tasten und **bidirektionales LED-Feedback** — **zeitgleich** zum ASIO-Audio ohne Konflikte um exklusiven Zugriff.
* **Ein-Klick-Installer (`Behringer_BCD2000_Setup_x64.exe`):**
  * Bindet automatisch Microsofts nativen `WinUSB`-Treiber an deine BCD2000 (du musst Zadig **nicht** manuell herunterladen oder einrichten).
  * Registriert sowohl den 64-Bit-ASIO-Treiber als auch den 64-Bit-MIDI-Treiber sauber inklusive Standard-Deinstallationsprogramm.
* **Sicheres Mixxx-Preset inklusive (`Behringer BCD2000 (Driver Safe)`):**
  * Optionales Preset für Mixxx 2.x mit *Soft-Takeover*, das verhindert, dass auf Null stehende EQ- oder Master-Regler beim Start von Mixxx den Ton stummschalten.

---

## Schnellstart-Anleitung

### Downloads & Release-Dateien
Lade die neueste verifizierte Version von der offiziellen **[GitHub Releases](https://github.com/martinrevert/bcd2000_ASIO_64bit/releases)**-Seite herunter:
* **`Behringer_BCD2000_Setup_x64.exe`**: Empfohlener automatischer All-in-One-Installer (WinUSB, ASIO/MIDI-DLLs und Mixxx-Preset).
* **`bcd2000_v2.x.x_binaries_x64.zip`**: Portables Paket für manuelle Installationen.
* **`SHA256SUMS.txt`**: Kryptografische SHA-256-Prüfsummen zur Integritätsprüfung.

### Voraussetzungen
* Ein PC mit **Windows 10 oder Windows 11 (64-Bit / x64)**.
* Eine **Behringer BCD2000**, idealerweise direkt an einem USB-Port des PCs angeschlossen (passive USB-Hubs vermeiden).

### Installationsschritte
1. **Schließe** alle geöffneten DAW- oder DJ-Programme (Mixxx, Ableton Live, REAPER usw.).
2. Starte **`Behringer_BCD2000_Setup_x64.exe`** als Administrator und folge dem Assistenten.
   * *(Optional)* Wähle das sichere Mixxx-Preset aus, wenn du Mixxx nutzt.
3. Falls deine BCD2000 während der Installation bereits angeschlossen war, **ziehe das USB-Kabel einmal ab und stecke es wieder ein**, damit Windows die Bindung sauber übernimmt.
4. Öffne deine DAW oder DJ-Software und wähle:
   * **Audiogerät (ASIO):** `Behringer BCD2000 ASIO 64-bit`
   * **MIDI-Controller (IN & OUT):** `Behringer BCD2000 MIDI`
5. **Tipp für Mixxx-Nutzer:** Wähle das mitgelieferte Mapping **`Behringer BCD2000 (Driver Safe)`**. Wenn du stattdessen Mixxx' eingebautes Standard-Preset verwendest, stelle die physischen EQ-Regler (Low / Mid / High) vor der Aktivierung in die Mittelstellung.

---

## Für Entwickler & Mitwirkende

Du möchtest das Architekturdiagramm sehen, erfahren wie wir USB-1.1-Takt-Drift und gleichzeitiges WinUSB-Audio+MIDI gelöst haben, selbst kompilieren oder Code beisteuern?

👉 Lies die vollständige technische Spezifikation in **[TECHNICS.de.md](TECHNICS.de.md)**.
🤖 KI-Assistenten und Coding-Agenten müssen **[AGENTS.md](../../AGENTS.md)** beachten.

---

## Lizenz & Rechtliche Hinweise

Dieses Projekt ist freie Software unter der **[GNU General Public License v3 oder neuer (GPL-3.0-or-later)](../../LICENSE)** mit der **ASIO & Host-Verknüpfungsausnahme** (GPLv3 Abschnitt 7).

* **Kompatibilität mit DAWs und DJ-Software:** Es wird ausdrücklich gestattet, `bcd2000_asio.dll` und `bcd2000_midi.dll` dynamisch über COM und WinMM mit beliebiger Audio-Host-Software (wie Mixxx, Ableton Live, FL Studio, REAPER, Traktor) zu verknüpfen und zu laden, unabhängig von den Lizenzbedingungen der Host-Anwendung.
* **Clean-Room ASIO-Header:** Die enthaltenen ASIO-Header sind unabhängige Clean-Room-Deklarationen der öffentlichen COM-Spezifikation für binäre Interoperabilität und enthalten keinen proprietären Code aus Steinbergs geschlossenem SDK.
* **Markenhinweis:** ASIO ist ein eingetragenes Warenzeichen und Software der Steinberg Media Technologies GmbH.
* **Autoren & Danksagung:** Siehe [AUTHORS](../../AUTHORS) für Copyright-Inhaber und Danksagungen für Reverse-Engineering (einschließlich Mario Kicherers Linux-ALSA-Forschung und des Mixxx-Teams).

---

[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) Español](README.es.md) | [![PT](flags/br.svg) Português](README.pt.md) | [![DE](flags/de.svg) **Deutsch**](README.de.md) | [![FR](flags/fr.svg) Français](README.fr.md) | [![ZH](flags/cn.svg) 中文](README.zh.md) | [![JA](flags/jp.svg) 日本語](README.ja.md)
