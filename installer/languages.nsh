; Behringer BCD2000 64-bit ASIO and MIDI Driver Suite
; Copyright (C) 2024-2026 Martin Revert <martinrevert@gmail.com> and contributors
; SPDX-License-Identifier: GPL-3.0-or-later
;
; Multilingual string definitions for NSIS Installer / Uninstaller (7 languages)

; ==============================================================================
; 1. Welcome Page Title & Text
; ==============================================================================

; --- English ---
LangString WELCOME_TITLE ${LANG_ENGLISH} \
  "Behringer BCD2000 Driver Installer (64-bit)"
LangString WELCOME_TEXT ${LANG_ENGLISH} \
  "This wizard will install the complete 64-bit user-mode driver suite for your Behringer BCD2000 DJ controller:$\r$\n$\r$\n  • Automatic WinUSB driver setup (no Zadig required)$\r$\n  • 64-bit COM ASIO audio driver (4 output channels, low latency)$\r$\n  • 64-bit WinMM MIDI driver (concurrent IN/OUT with ASIO bridge)$\r$\n  • Free software under GNU General Public License v3 (GPLv3)$\r$\n$\r$\nIMPORTANT: Please close Mixxx, Ableton Live, or any other audio/MIDI software before continuing."

; --- Spanish ---
LangString WELCOME_TITLE ${LANG_SPANISH} \
  "Instalador de Drivers Behringer BCD2000 (64-bit)"
LangString WELCOME_TEXT ${LANG_SPANISH} \
  "Este asistente instalará el paquete completo en modo usuario (x64) para tu controlador Behringer BCD2000:$\r$\n$\r$\n  • Driver USB WinUSB automático (no necesitás Zadig)$\r$\n  • Servidor COM ASIO 64-bit (4 canales de salida, baja latencia)$\r$\n  • Driver WinMM MIDI 64-bit simultáneo IN/OUT con puente ASIO$\r$\n  • Licencia libre GNU General Public License v3 (GPLv3)$\r$\n$\r$\nIMPORTANTE: Cerrá Mixxx, Ableton Live u otros programas de audio/MIDI antes de continuar."

; --- Portuguese (Brazil) ---
LangString WELCOME_TITLE ${LANG_PORTUGUESEBR} \
  "Instalador de Drivers Behringer BCD2000 (64 bits)"
LangString WELCOME_TEXT ${LANG_PORTUGUESEBR} \
  "Este assistente instalará o pacote completo em modo de usuário (x64) para o seu controlador Behringer BCD2000:$\r$\n$\r$\n  • Configuração automática do driver WinUSB (sem necessidade de Zadig)$\r$\n  • Servidor COM ASIO 64-bit (4 canais de saída, baixa latência)$\r$\n  • Driver WinMM MIDI 64-bit simultâneo IN/OUT com ponte ASIO$\r$\n  • Software livre sob a GNU General Public License v3 (GPLv3)$\r$\n$\r$\nIMPORTANTE: Feche o Mixxx, Ableton Live ou outros programas de áudio/MIDI antes de continuar."

; --- German ---
LangString WELCOME_TITLE ${LANG_GERMAN} \
  "Behringer BCD2000 Treiber-Installation (64-Bit)"
LangString WELCOME_TEXT ${LANG_GERMAN} \
  "Dieser Assistent installiert die vollständige 64-Bit-Treibersuite im Benutzermodus für Ihren Behringer BCD2000 DJ-Controller:$\r$\n$\r$\n  • Automatische WinUSB-Treiberkonfiguration (kein Zadig erforderlich)$\r$\n  • 64-Bit COM ASIO-Audiotreiber (4 Ausgangskanäle, niedrige Latenz)$\r$\n  • 64-Bit WinMM MIDI-Treiber (gleichzeitiges IN/OUT mit ASIO-Bridge)$\r$\n  • Freie Software unter der GNU General Public License v3 (GPLv3)$\r$\n$\r$\nWICHTIG: Bitte schließen Sie Mixxx, Ableton Live oder andere Audio-/MIDI-Programme, bevor Sie fortfahren."

; --- French ---
LangString WELCOME_TITLE ${LANG_FRENCH} \
  "Installateur de Pilotes Behringer BCD2000 (64 bits)"
LangString WELCOME_TEXT ${LANG_FRENCH} \
  "Cet assistant installera la suite complète de pilotes 64 bits en mode utilisateur pour votre contrôleur DJ Behringer BCD2000 :$\r$\n$\r$\n  • Configuration automatique du pilote WinUSB (aucun Zadig requis)$\r$\n  • Serveur COM ASIO 64 bits (4 canaux de sortie, faible latence)$\r$\n  • Pilote WinMM MIDI 64 bits simultané IN/OUT avec passerelle ASIO$\r$\n  • Logiciel libre sous licence GNU General Public License v3 (GPLv3)$\r$\n$\r$\nIMPORTANT : Fermez Mixxx, Ableton Live ou toute autre application audio/MIDI avant de continuer."

; --- Chinese (Simplified) ---
LangString WELCOME_TITLE ${LANG_SIMPCHINESE} \
  "Behringer BCD2000 驱动安装程序 (64 位)"
LangString WELCOME_TEXT ${LANG_SIMPCHINESE} \
  "本向导将为您的 Behringer BCD2000 DJ 控制器安装完整的 64 位用户模式驱动套件：$\r$\n$\r$\n  • 自动配置 WinUSB 驱动程序（无需手动运行 Zadig）$\r$\n  • 64 位 COM ASIO 音频驱动（4 输出通道，超低延迟）$\r$\n  • 64 位 WinMM MIDI 驱动（同时支持输入/输出并集成 ASIO 桥接）$\r$\n  • 基于 GNU General Public License v3 (GPLv3) 的自由软件$\r$\n$\r$\n重要提示：在继续安装之前，请关闭 Mixxx、Ableton Live 或其他音频/MIDI 宿主程序。"

; --- Japanese ---
LangString WELCOME_TITLE ${LANG_JAPANESE} \
  "Behringer BCD2000 ドライバ インストーラー (64ビット)"
LangString WELCOME_TEXT ${LANG_JAPANESE} \
  "このウィザードは、Behringer BCD2000 DJ コントローラー用 64 ビット ユーザーモード ドライバ スイートをインストールします:$\r$\n$\r$\n  • 自動 WinUSB ドライバ構成（Zadig 不要）$\r$\n  • 64 ビット COM ASIO オーディオ ドライバ（4 出力チャンネル、低レイテンシー）$\r$\n  • 64 ビット WinMM MIDI ドライバ（ASIO ブリッジ経由の同時 IN/OUT）$\r$\n  • GNU General Public License v3 (GPLv3) に準拠した自由ソフトウェア$\r$\n$\r$\n重要: 続行する前に、Mixxx、Ableton Live などのオーディオ／MIDI アプリケーションを閉じてください。"

; ==============================================================================
; 2. Finish Page Title & Text
; ==============================================================================

; --- English ---
LangString FINISH_TITLE ${LANG_ENGLISH} \
  "Installation Completed"
LangString FINISH_TEXT ${LANG_ENGLISH} \
  "The 64-bit ASIO, WinMM MIDI, and WinUSB drivers for Behringer BCD2000 have been installed successfully.$\r$\n$\r$\n• If your BCD2000 was plugged in during installation, disconnect and reconnect the USB cable once before opening your DAW.$\r$\n• If you use Mixxx with the built-in factory preset (instead of our 'Behringer BCD2000 (Driver Safe)'), leave physical EQ knobs (Low/Mid/High) centered before enabling the controller."

; --- Spanish ---
LangString FINISH_TITLE ${LANG_SPANISH} \
  "Instalación completada"
LangString FINISH_TEXT ${LANG_SPANISH} \
  "Los drivers ASIO 64-bit, WinMM MIDI y WinUSB para Behringer BCD2000 quedaron instalados.$\r$\n$\r$\n• Si tu BCD2000 estaba conectada, desconectá y volvé a conectar el cable USB antes de abrir tu DAW.$\r$\n• Si usás Mixxx con el preset original de fábrica (en vez de nuestro 'Behringer BCD2000 (Driver Safe)'), dejá las perillas de EQ (Low/Mid/High) en el centro antes de activar el controlador MIDI."

; --- Portuguese (Brazil) ---
LangString FINISH_TITLE ${LANG_PORTUGUESEBR} \
  "Instalação concluída"
LangString FINISH_TEXT ${LANG_PORTUGUESEBR} \
  "Os drivers ASIO 64-bit, WinMM MIDI e WinUSB para Behringer BCD2000 foram instalados com sucesso.$\r$\n$\r$\n• Se o seu BCD2000 já estava conectado, desconecte e reconecte o cabo USB antes de abrir sua DAW.$\r$\n• Se você usa o Mixxx com o preset padrão de fábrica (em vez do nosso 'Behringer BCD2000 (Driver Safe)'), deixe os botões de EQ (Low/Mid/High) centralizados antes de ativar o controlador MIDI."

; --- German ---
LangString FINISH_TITLE ${LANG_GERMAN} \
  "Installation abgeschlossen"
LangString FINISH_TEXT ${LANG_GERMAN} \
  "Die 64-Bit-Treiber für ASIO, WinMM-MIDI und WinUSB für Behringer BCD2000 wurden erfolgreich installiert.$\r$\n$\r$\n• Wenn Ihr BCD2000 während der Installation angeschlossen war, trennen Sie das USB-Kabel und schließen Sie es erneut an, bevor Sie Ihre DAW öffnen.$\r$\n• Falls Sie Mixxx mit dem Werks-Preset (statt unserem 'Behringer BCD2000 (Driver Safe)') verwenden, stellen Sie die EQ-Regler (Low/Mid/High) mittig ein, bevor Sie den Controller aktivieren."

; --- French ---
LangString FINISH_TITLE ${LANG_FRENCH} \
  "Installation terminée"
LangString FINISH_TEXT ${LANG_FRENCH} \
  "Les pilotes ASIO 64 bits, WinMM MIDI et WinUSB pour Behringer BCD2000 ont été installés avec succès.$\r$\n$\r$\n• Si votre BCD2000 était connectée pendant l'installation, débranchez et rebranchez le câble USB avant d'ouvrir votre DAW.$\r$\n• Si vous utilisez Mixxx avec le préréglage d'usine (au lieu de notre 'Behringer BCD2000 (Driver Safe)'), laissez les potentiomètres d'EQ (Low/Mid/High) au centre avant d'activer le contrôleur MIDI."

; --- Chinese (Simplified) ---
LangString FINISH_TITLE ${LANG_SIMPCHINESE} \
  "安装完成"
LangString FINISH_TEXT ${LANG_SIMPCHINESE} \
  "Behringer BCD2000 64 位 ASIO、WinMM MIDI 及 WinUSB 驱动程序已成功安装。$\r$\n$\r$\n• 如果在安装时已连接 BCD2000，请在启动 DAW 之前重新插拔一次 USB 数据线。$\r$\n• 如果您在 Mixxx 中使用出厂默认映射预设（而非随本程序安装的 'Behringer BCD2000 (Driver Safe)'），请在启用控制器前将硬件 EQ 旋钮（低音/中音/高音）调至中心位置。"

; --- Japanese ---
LangString FINISH_TITLE ${LANG_JAPANESE} \
  "インストールが完了しました"
LangString FINISH_TEXT ${LANG_JAPANESE} \
  "Behringer BCD2000 用 64 ビット ASIO、WinMM MIDI および WinUSB ドライバが正常にインストールされました。$\r$\n$\r$\n• インストール時に BCD2000 が接続されていた場合は、DAW を起動する前に USB ケーブルを抜き差ししてください。$\r$\n• Mixxx で（同梱の「Behringer BCD2000 (Driver Safe)」ではなく）内蔵プリセットを使用する場合は、コントローラーを有効にする前に EQ ノブ（Low/Mid/High）を中央位置にしてください。"

; ==============================================================================
; 3. Architecture Requirement Message (.onInit)
; ==============================================================================

LangString MSG_REQUIRES_X64 ${LANG_ENGLISH}     "This installer requires 64-bit Windows (x64)."
LangString MSG_REQUIRES_X64 ${LANG_SPANISH}     "Este instalador requiere Windows de 64 bits (x64)."
LangString MSG_REQUIRES_X64 ${LANG_PORTUGUESEBR} "Este instalador requer o Windows de 64 bits (x64)."
LangString MSG_REQUIRES_X64 ${LANG_GERMAN}      "Dieser Installer erfordert 64-Bit-Windows (x64)."
LangString MSG_REQUIRES_X64 ${LANG_FRENCH}      "Cet installateur nécessite Windows 64 bits (x64)."
LangString MSG_REQUIRES_X64 ${LANG_SIMPCHINESE} "此安装程序需要 64 位 Windows (x64)。"
LangString MSG_REQUIRES_X64 ${LANG_JAPANESE}    "このインストーラーには 64 ビット Windows (x64) が必要です。"

; ==============================================================================
; 4. Components Page Section Names and Descriptions
; ==============================================================================

; --- Section Drivers ---
LangString NAME_SecDrivers ${LANG_ENGLISH}     "BCD2000 Drivers (WinUSB + ASIO + MIDI x64)"
LangString NAME_SecDrivers ${LANG_SPANISH}     "Drivers BCD2000 (WinUSB + ASIO + MIDI x64)"
LangString NAME_SecDrivers ${LANG_PORTUGUESEBR} "Drivers BCD2000 (WinUSB + ASIO + MIDI x64)"
LangString NAME_SecDrivers ${LANG_GERMAN}      "BCD2000 Treiber (WinUSB + ASIO + MIDI x64)"
LangString NAME_SecDrivers ${LANG_FRENCH}      "Pilotes BCD2000 (WinUSB + ASIO + MIDI x64)"
LangString NAME_SecDrivers ${LANG_SIMPCHINESE} "BCD2000 驱动程序 (WinUSB + ASIO + MIDI x64)"
LangString NAME_SecDrivers ${LANG_JAPANESE}    "BCD2000 ドライバ (WinUSB + ASIO + MIDI x64)"

LangString DESC_SecDrivers ${LANG_ENGLISH} \
  "Installs WinUSB driver (via local signed catalog), registers bcd2000_asio.dll (64-bit COM ASIO), installs bcd2000_midi.dll into System32 (Drivers32), and includes diagnostic tools."
LangString DESC_SecDrivers ${LANG_SPANISH} \
  "Instala el driver WinUSB (vía catálogo firmado local), registra bcd2000_asio.dll (COM ASIO 64-bit), instala bcd2000_midi.dll en System32 (Drivers32) e incluye herramientas de diagnóstico."
LangString DESC_SecDrivers ${LANG_PORTUGUESEBR} \
  "Instala o driver WinUSB (via catálogo assinado local), registra bcd2000_asio.dll (COM ASIO 64-bit), instala bcd2000_midi.dll no System32 (Drivers32) e inclui ferramentas de diagnóstico."
LangString DESC_SecDrivers ${LANG_GERMAN} \
  "Installiert den WinUSB-Treiber (über lokal signierten Katalog), registriert bcd2000_asio.dll (64-Bit COM ASIO), installiert bcd2000_midi.dll in System32 (Drivers32) und enthält Diagnoseprogramme."
LangString DESC_SecDrivers ${LANG_FRENCH} \
  "Installe le pilote WinUSB (via un catalogue signé localement), enregistre bcd2000_asio.dll (COM ASIO 64 bits), installe bcd2000_midi.dll dans System32 (Drivers32) et inclut les outils de diagnostic."
LangString DESC_SecDrivers ${LANG_SIMPCHINESE} \
  "安装 WinUSB 驱动程序（通过本地签名编录），注册 bcd2000_asio.dll（64 位 COM ASIO），将 bcd2000_midi.dll 安装至 System32（Drivers32），并附带诊断测试工具。"
LangString DESC_SecDrivers ${LANG_JAPANESE} \
  "WinUSB ドライバを構成（ローカル署名カタログ経由）し、bcd2000_asio.dll（64 ビット COM ASIO）を登録、System32 (Drivers32) に bcd2000_midi.dll を配置し、診断ツールを導入します。"

; --- Section Mixxx Safe Preset ---
LangString NAME_SecMixxxPreset ${LANG_ENGLISH}     "Mixxx 2.x Safe MIDI Preset (prevents zero audio mute)"
LangString NAME_SecMixxxPreset ${LANG_SPANISH}     "Preset MIDI seguro para Mixxx 2.x (evita audio en 0)"
LangString NAME_SecMixxxPreset ${LANG_PORTUGUESEBR} "Preset MIDI seguro para Mixxx 2.x (evita áudio mudo em 0)"
LangString NAME_SecMixxxPreset ${LANG_GERMAN}      "Sicheres Mixxx 2.x MIDI-Preset (verhindert Nullpegel-Stummschaltung)"
LangString NAME_SecMixxxPreset ${LANG_FRENCH}      "Préréglage MIDI sécurisé pour Mixxx 2.x (évite le son muet à 0)"
LangString NAME_SecMixxxPreset ${LANG_SIMPCHINESE} "Mixxx 2.x 安全 MIDI 预设（防止开机零电平静音）"
LangString NAME_SecMixxxPreset ${LANG_JAPANESE}    "Mixxx 2.x 安全 MIDI プリセット（起動時ゼロミュート防止）"

LangString DESC_SecMixxxPreset ${LANG_ENGLISH} \
  "Installs 'Behringer BCD2000 (Driver Safe)' into %LOCALAPPDATA%\Mixxx\controllers\. Prevents physical EQ/Master knobs left at 0 from muting audio when starting Mixxx and enables soft-takeover."
LangString DESC_SecMixxxPreset ${LANG_SPANISH} \
  "Instala 'Behringer BCD2000 (Driver Safe)' en %LOCALAPPDATA%\Mixxx\controllers\. Evita que las perillas físicas de EQ o Master dejadas en 0 corten el audio al iniciar Mixxx y agrega soft-takeover."
LangString DESC_SecMixxxPreset ${LANG_PORTUGUESEBR} \
  "Instala 'Behringer BCD2000 (Driver Safe)' em %LOCALAPPDATA%\Mixxx\controllers\. Impede que botões físicos de EQ/Master deixados em 0 cortem o áudio ao iniciar o Mixxx e adiciona soft-takeover."
LangString DESC_SecMixxxPreset ${LANG_GERMAN} \
  "Installiert 'Behringer BCD2000 (Driver Safe)' in %LOCALAPPDATA%\Mixxx\controllers\. Verhindert, dass physische EQ-/Master-Regler auf 0 beim Starten den Ton stummschalten, und aktiviert Soft-Takeover."
LangString DESC_SecMixxxPreset ${LANG_FRENCH} \
  "Installe 'Behringer BCD2000 (Driver Safe)' dans %LOCALAPPDATA%\Mixxx\controllers\. Empêche les potentiomètres physiques d'EQ/Master laissés à 0 de couper le son au démarrage de Mixxx et active le soft-takeover."
LangString DESC_SecMixxxPreset ${LANG_SIMPCHINESE} \
  "将 'Behringer BCD2000 (Driver Safe)' 安装至 %LOCALAPPDATA%\Mixxx\controllers\。防止硬件 EQ/Master 旋钮在置零状态下导致 Mixxx 启动静音，并启用平滑接管 (soft-takeover)。"
LangString DESC_SecMixxxPreset ${LANG_JAPANESE} \
  "「Behringer BCD2000 (Driver Safe)」を %LOCALAPPDATA%\Mixxx\controllers\ にインストールします。Mixxx 起動時に物理 EQ/Master ノブが 0 の位置にあることによるミュートを防ぎ、ソフトテイクオーバーを有効にします。"

; ==============================================================================
; 5. DetailPrint Progress Messages (Installation)
; ==============================================================================

LangString DETAIL_WINUSB_SETUP ${LANG_ENGLISH}     "Configuring WinUSB driver for Behringer BCD2000 (VID_1397 / PID_00BD)..."
LangString DETAIL_WINUSB_SETUP ${LANG_SPANISH}     "Configurando driver WinUSB para Behringer BCD2000 (VID_1397 / PID_00BD)..."
LangString DETAIL_WINUSB_SETUP ${LANG_PORTUGUESEBR} "Configurando driver WinUSB para Behringer BCD2000 (VID_1397 / PID_00BD)..."
LangString DETAIL_WINUSB_SETUP ${LANG_GERMAN}      "Konfiguriere WinUSB-Treiber für Behringer BCD2000 (VID_1397 / PID_00BD)..."
LangString DETAIL_WINUSB_SETUP ${LANG_FRENCH}      "Configuration du pilote WinUSB pour Behringer BCD2000 (VID_1397 / PID_00BD)..."
LangString DETAIL_WINUSB_SETUP ${LANG_SIMPCHINESE} "正在为 Behringer BCD2000 配置 WinUSB 驱动程序 (VID_1397 / PID_00BD)..."
LangString DETAIL_WINUSB_SETUP ${LANG_JAPANESE}    "Behringer BCD2000 (VID_1397 / PID_00BD) 用 WinUSB ドライバを構成中..."

LangString DETAIL_WINUSB_WARN ${LANG_ENGLISH}     "Warning: bcd2000_winusb_setup returned code"
LangString DETAIL_WINUSB_WARN ${LANG_SPANISH}     "Advertencia: bcd2000_winusb_setup devolvió código"
LangString DETAIL_WINUSB_WARN ${LANG_PORTUGUESEBR} "Aviso: bcd2000_winusb_setup retornou o código"
LangString DETAIL_WINUSB_WARN ${LANG_GERMAN}      "Warnung: bcd2000_winusb_setup gab Code zurück:"
LangString DETAIL_WINUSB_WARN ${LANG_FRENCH}      "Avertissement : bcd2000_winusb_setup a renvoyé le code"
LangString DETAIL_WINUSB_WARN ${LANG_SIMPCHINESE} "警告：bcd2000_winusb_setup 返回代码"
LangString DETAIL_WINUSB_WARN ${LANG_JAPANESE}    "警告: bcd2000_winusb_setup が終了コードを返しました:"

LangString DETAIL_ASIO_REGISTER ${LANG_ENGLISH}     "Registering 64-bit ASIO COM Server..."
LangString DETAIL_ASIO_REGISTER ${LANG_SPANISH}     "Registrando servidor COM ASIO 64-bit..."
LangString DETAIL_ASIO_REGISTER ${LANG_PORTUGUESEBR} "Registrando servidor COM ASIO 64-bit..."
LangString DETAIL_ASIO_REGISTER ${LANG_GERMAN}      "Registriere 64-Bit ASIO COM-Server..."
LangString DETAIL_ASIO_REGISTER ${LANG_FRENCH}      "Enregistrement du serveur COM ASIO 64 bits..."
LangString DETAIL_ASIO_REGISTER ${LANG_SIMPCHINESE} "正在注册 64 位 ASIO COM 服务器..."
LangString DETAIL_ASIO_REGISTER ${LANG_JAPANESE}    "64 ビット ASIO COM サーバーを登録中..."

LangString DETAIL_ASIO_WARN ${LANG_ENGLISH}     "Warning: regsvr32 ASIO returned code"
LangString DETAIL_ASIO_WARN ${LANG_SPANISH}     "Advertencia: regsvr32 ASIO devolvió código"
LangString DETAIL_ASIO_WARN ${LANG_PORTUGUESEBR} "Aviso: regsvr32 ASIO retornou o código"
LangString DETAIL_ASIO_WARN ${LANG_GERMAN}      "Warnung: regsvr32 ASIO gab Code zurück:"
LangString DETAIL_ASIO_WARN ${LANG_FRENCH}      "Avertissement : regsvr32 ASIO a renvoyé le code"
LangString DETAIL_ASIO_WARN ${LANG_SIMPCHINESE} "警告：regsvr32 ASIO 返回代码"
LangString DETAIL_ASIO_WARN ${LANG_JAPANESE}    "警告: regsvr32 ASIO が終了コードを返しました:"

LangString DETAIL_MIDI_REGISTER ${LANG_ENGLISH}     "Installing and registering 64-bit WinMM MIDI driver in System32..."
LangString DETAIL_MIDI_REGISTER ${LANG_SPANISH}     "Instalando y registrando driver WinMM MIDI 64-bit en System32..."
LangString DETAIL_MIDI_REGISTER ${LANG_PORTUGUESEBR} "Instalando e registrando driver WinMM MIDI 64-bit no System32..."
LangString DETAIL_MIDI_REGISTER ${LANG_GERMAN}      "Installiere und registriere 64-Bit WinMM MIDI-Treiber in System32..."
LangString DETAIL_MIDI_REGISTER ${LANG_FRENCH}      "Installation et enregistrement du pilote WinMM MIDI 64 bits dans System32..."
LangString DETAIL_MIDI_REGISTER ${LANG_SIMPCHINESE} "正在 System32 中安装并注册 64 位 WinMM MIDI 驱动程序..."
LangString DETAIL_MIDI_REGISTER ${LANG_JAPANESE}    "64 ビット WinMM MIDI ドライバを System32 にインストールおよび登録中..."

LangString DETAIL_MIDI_WARN ${LANG_ENGLISH}     "Warning: regsvr32 MIDI returned code"
LangString DETAIL_MIDI_WARN ${LANG_SPANISH}     "Advertencia: regsvr32 MIDI devolvió código"
LangString DETAIL_MIDI_WARN ${LANG_PORTUGUESEBR} "Aviso: regsvr32 MIDI retornou o código"
LangString DETAIL_MIDI_WARN ${LANG_GERMAN}      "Warnung: regsvr32 MIDI gab Code zurück:"
LangString DETAIL_MIDI_WARN ${LANG_FRENCH}      "Avertissement : regsvr32 MIDI a renvoyé le code"
LangString DETAIL_MIDI_WARN ${LANG_SIMPCHINESE} "警告：regsvr32 MIDI 返回代码"
LangString DETAIL_MIDI_WARN ${LANG_JAPANESE}    "警告: regsvr32 MIDI が終了コードを返しました:"

LangString DETAIL_MIXXX_PRESET ${LANG_ENGLISH}     "Installing preset 'Behringer BCD2000 (Driver Safe)' into $LOCALAPPDATA\Mixxx\controllers..."
LangString DETAIL_MIXXX_PRESET ${LANG_SPANISH}     "Instalando preset 'Behringer BCD2000 (Driver Safe)' en $LOCALAPPDATA\Mixxx\controllers..."
LangString DETAIL_MIXXX_PRESET ${LANG_PORTUGUESEBR} "Instalando preset 'Behringer BCD2000 (Driver Safe)' em $LOCALAPPDATA\Mixxx\controllers..."
LangString DETAIL_MIXXX_PRESET ${LANG_GERMAN}      "Installiere Preset 'Behringer BCD2000 (Driver Safe)' in $LOCALAPPDATA\Mixxx\controllers..."
LangString DETAIL_MIXXX_PRESET ${LANG_FRENCH}      "Installation du préréglage 'Behringer BCD2000 (Driver Safe)' dans $LOCALAPPDATA\Mixxx\controllers..."
LangString DETAIL_MIXXX_PRESET ${LANG_SIMPCHINESE} "正在将 'Behringer BCD2000 (Driver Safe)' 预设安装至 $LOCALAPPDATA\Mixxx\controllers..."
LangString DETAIL_MIXXX_PRESET ${LANG_JAPANESE}    "「Behringer BCD2000 (Driver Safe)」プリセットを $LOCALAPPDATA\Mixxx\controllers にインストール中..."

; ==============================================================================
; 6. DetailPrint Progress Messages (Uninstallation)
; ==============================================================================

LangString UN_DETAIL_ASIO ${LANG_ENGLISH}     "Unregistering 64-bit ASIO COM Server..."
LangString UN_DETAIL_ASIO ${LANG_SPANISH}     "Desregistrando servidor COM ASIO 64-bit..."
LangString UN_DETAIL_ASIO ${LANG_PORTUGUESEBR} "Desregistrando servidor COM ASIO 64-bit..."
LangString UN_DETAIL_ASIO ${LANG_GERMAN}      "Deregestriere 64-Bit ASIO COM-Server..."
LangString UN_DETAIL_ASIO ${LANG_FRENCH}      "Désenregistrement du serveur COM ASIO 64 bits..."
LangString UN_DETAIL_ASIO ${LANG_SIMPCHINESE} "正在注销 64 位 ASIO COM 服务器..."
LangString UN_DETAIL_ASIO ${LANG_JAPANESE}    "64 ビット ASIO COM サーバーの登録を解除中..."

LangString UN_DETAIL_MIDI ${LANG_ENGLISH}     "Unregistering 64-bit WinMM MIDI driver..."
LangString UN_DETAIL_MIDI ${LANG_SPANISH}     "Desregistrando driver WinMM MIDI 64-bit..."
LangString UN_DETAIL_MIDI ${LANG_PORTUGUESEBR} "Desregistrando driver WinMM MIDI 64-bit..."
LangString UN_DETAIL_MIDI ${LANG_GERMAN}      "Deregestriere 64-Bit WinMM MIDI-Treiber..."
LangString UN_DETAIL_MIDI ${LANG_FRENCH}      "Désenregistrement du pilote WinMM MIDI 64 bits..."
LangString UN_DETAIL_MIDI ${LANG_SIMPCHINESE} "正在注销 64 位 WinMM MIDI 驱动程序..."
LangString UN_DETAIL_MIDI ${LANG_JAPANESE}    "64 ビット WinMM MIDI ドライバの登録を解除中..."

LangString UN_DETAIL_WINUSB ${LANG_ENGLISH}     "Cleaning up WinUSB package..."
LangString UN_DETAIL_WINUSB ${LANG_SPANISH}     "Limpiando paquete WinUSB..."
LangString UN_DETAIL_WINUSB ${LANG_PORTUGUESEBR} "Removendo pacote WinUSB..."
LangString UN_DETAIL_WINUSB ${LANG_GERMAN}      "Bereinige WinUSB-Paket..."
LangString UN_DETAIL_WINUSB ${LANG_FRENCH}      "Nettoyage du paquet WinUSB..."
LangString UN_DETAIL_WINUSB ${LANG_SIMPCHINESE} "正在清理 WinUSB 驱动程序包..."
LangString UN_DETAIL_WINUSB ${LANG_JAPANESE}    "WinUSB パッケージをクリーンアップ中..."

