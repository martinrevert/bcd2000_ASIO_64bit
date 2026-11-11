[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) Español](README.es.md) | [![PT](flags/br.svg) **Português**](README.pt.md) | [![DE](flags/de.svg) Deutsch](README.de.md) | [![FR](flags/fr.svg) Français](README.fr.md) | [![ZH](flags/cn.svg) 中文](README.zh.md) | [![JA](flags/jp.svg) 日本語](README.ja.md)

# Behringer BCD2000 — Driver Nativo ASIO e MIDI de 64 bits (Windows 10/11)

**Uma nova suíte de drivers open-source de 64 bits que traz a clássica controladora DJ Behringer BCD2000 de volta à vida em DAWs e softwares de DJ modernos.**

[Documentação Técnica e Arquitetura (TECHNICS.pt.md)](TECHNICS.pt.md)


---

## Bem-vinda de volta, BCD2000!

Se você ainda tem uma **Behringer B-Control Deejay BCD2000** na sua mesa ou guardada em um case, provavelmente conhece esse sentimento agridoce: o hardware continua firme como um tanque — faders, potenciômetros de EQ, jog wheels, pré-amplificador de fones e saídas RCA intactos —, mas os computadores modernos a tratavam como um peso de papel.

**Hoje isso muda.** Este projeto entrega à BCD2000 algo que a Behringer nunca lançou: um **verdadeiro driver de áudio ASIO de 64 bits e driver MIDI de 64 bits**, construídos do zero para **Windows 10 e Windows 11 (x64)**.

Conecte o cabo, rode um único instalador e volte a usar sua BCD2000 em softwares modernos de 64 bits como **Mixxx**, **Ableton Live**, **REAPER**, **FL Studio**, **Traktor** e muitos outros.

---

## De Onde Viemos e Por Que Esta Iniciativa Existe

A Behringer lançou a BCD2000 em meados dos anos 2000, na era do Windows XP / Vista, e **encerrou o suporte oficial nos drivers de 32 bits**. Diferente de dispositivos USB Audio Class ou USB-MIDI padrão, a BCD2000 usa um protocolo proprietário em seu microcontrolador 8051 (`VID 0x1397`, `PID 0x00BD`). Sem um driver dedicado, o Windows 10 e o Windows 11 simplesmente não conseguem conversar com ela.

Durante anos, quem mantinha o equipamento ficava preso a gambiarras desconfortáveis: manter um PC antigo de 32 bits com Windows XP/7, configurar máquinas virtuais instáveis ou abandonar uma controladora totalmente funcional.

Esta iniciativa nasceu de uma convicção simples: **um bom hardware não deve virar lixo eletrônico só porque o fabricante o abandonou.** Com base na engenharia reversa documentada pelo driver livre do Linux (`snd-bcd2000`) e nas APIs USB modernas em modo usuário do Windows, construímos do zero uma pilha de drivers nativa de 64 bits para que a comunidade BCD2000 possa continuar mixando e produzindo em máquinas atuais.

---

## Por Que Isso NÃO é "Usar Drivers Antigos no Modo de Compatibilidade do Windows 7"

Uma pergunta muito comum é: *"Por que não apenas instalar o driver antigo da Behringer em Modo de Compatibilidade do Windows 7?"*

A resposta direta é:

1. **O Modo de Compatibilidade não funciona com drivers de kernel.** O modo de compatibilidade do Windows ajusta apenas aplicativos antigos em modo usuário (`.exe`). Um kernel do Windows de 64 bits **jamais consegue carregar drivers `.sys` de 32 bits** — não existe nenhuma opção que faça isso funcionar.
2. **DAWs de 64 bits não carregam DLLs ASIO de 32 bits.** Softwares modernos de áudio e DJ são processos nativos de 64 bits e exigem uma biblioteca COM ASIO nativa de 64 bits (`x64`) e portas MIDI de 64 bits.
3. **Nenhum binário antigo da Behringer é usado aqui.** Este projeto **não** modifica nem encapsula o software abandonado de 2006. É **código C++ de 64 bits 100% original**, rodando em modo usuário sobre o **`WinUSB.sys`** (o driver USB oficial integrado ao Windows 10/11). Sem gambiarras no kernel e sem precisar desativar a imposição de assinatura de drivers.

---

## O Que Você Recebe (O Básico)

* **Áudio ASIO Nativo de 64 bits (`Behringer BCD2000 ASIO 64-bit`):**
  * 4 canais de saída: **Master Esquerdo/Direito (RCA traseiro)** + **Fones Esquerdo/Direito (Cue frontal TRS 1/4")** em 44,1 kHz / 16-bit PCM.
  * 4 canais de entrada expostos via ASIO (`In 1–4`).
  * Streaming em tempo real de baixa latência com rastreamento de clock por hardware para áudio limpo e sem estalos.
* **MIDI IN e OUT Simultâneo de 64 bits (`Behringer BCD2000 MIDI`):**
  * Suporte completo para faders, knobs, jog wheels, botões e **retorno bidirecional de LEDs**, funcionando **ao mesmo tempo** que o áudio ASIO sem conflitos de acesso exclusivo.
* **Painel de Controle Interativo e Dashboard em Tempo Real (`bcd2000_control_panel.exe`):**
  * Acessível diretamente das configurações de áudio do seu DAW (*"Control Panel"*) ou pelo Menu Iniciar do Windows.
  * **Roteamento de Áudio em Tempo Real:** Troca Master RCA (Ch 1-2) com Fones (Ch 3-4), espelhamento de Master nos Fones (Cue segue o Master) e Downmix para Mono.
  * **Seleção Instantânea de Buffer e Latência:** Escolha entre 64 e 4096 amostras com Hot-Reset automático transparente (`kAsioResetRequest`).
  * **Atenuação Digital / Headroom Pad:** Proteção contra clipping de DAC (0 dB, -3 dB, -6 dB, -12 dB) e mutes rápidos.
  * **Telemetria de Sinal e VU Meters Animados:** Medidores estéreo em tempo real para Master e Fones, medidor do buffer circular (%) e contador de falhas/dropouts.
  * **Ferramentas de Hardware:** Botão para testar todos os 26 LEDs físicos da controladora.
* **Instalador em Um Clique (`Behringer_BCD2000_Setup_x64.exe`):**
  * Vincula automaticamente o driver nativo `WinUSB` do Windows à sua BCD2000 (você **não** precisa baixar nem configurar o Zadig manualmente).
  * Registra os drivers ASIO e MIDI de 64 bits de forma limpa, com desinstalador padrão.
* **Preset Seguro para Mixxx Incluso (`Behringer BCD2000 (Driver Safe)`):**
  * Preset opcional para Mixxx 2.x que adiciona *soft-takeover* e evita que knobs físicos de EQ ou Master deixados no zero cortem o som ao abrir o Mixxx.

---

## Guia Rápido de Instalação e Uso

### Downloads e Arquivos de Versão
Baixe o instalador mais recente e verificado na página oficial do **[GitHub Releases](https://github.com/martinrevert/bcd2000_ASIO_64bit/releases)**:
* **`Behringer_BCD2000_Setup_x64.exe`**: Instalador único automatizado recomendado (inclui driver WinUSB, DLLs ASIO/MIDI e preset para Mixxx).
* **`bcd2000_v2.x.x_binaries_x64.zip`**: Pacote portátil independente para instalações manuais.
* **`SHA256SUMS.txt`**: Hashes criptográficos SHA-256 para verificação de integridade.

### Requisitos
* PC com **Windows 10 ou Windows 11 (64 bits / x64)**.
* **Behringer BCD2000** conectada de preferência a uma porta USB direta (evite hubs USB sem fonte).

### Passos de Instalação
1. **Feche** qualquer DAW ou software de DJ aberto (Mixxx, Ableton Live, REAPER, etc.).
2. Execute **`Behringer_BCD2000_Setup_x64.exe`** como Administrador e siga o assistente.
   * *(Opcional)* Marque o preset seguro se você usa o Mixxx.
3. Se a BCD2000 já estava conectada durante a instalação, **desconecte e reconecte o cabo USB** uma vez para aplicar o vínculo corretamente.
4. Abra sua DAW ou software de DJ e selecione:
   * **Dispositivo de Áudio (ASIO):** `Behringer BCD2000 ASIO 64-bit`
   * **Controlador MIDI (IN e OUT):** `Behringer BCD2000 MIDI`
5. **Dica para usuários do Mixxx:** Escolha o mapeamento **`Behringer BCD2000 (Driver Safe)`** instalado pelo assistente. Caso use o preset original do Mixxx, deixe os knobs físicos de EQ (Low / Mid / High) centralizados antes de ativar o controlador.

---

## Para Desenvolvedores e Colaboradores

Quer conferir o diagrama de arquitetura, entender como resolvemos o drift de clock USB 1.1 e o compartilhamento áudio+MIDI no WinUSB, compilar o código-fonte ou contribuir?

👉 Leia a documentação técnica completa em **[TECHNICS.pt.md](TECHNICS.pt.md)**.
🤖 Assistentes de IA e agentes de código devem seguir **[AGENTS.md](../../AGENTS.md)**.

---

## Licença e Aspectos Jurídicos

Este projeto é software livre distribuído sob os termos da **[GNU General Public License v3 ou posterior (GPL-3.0-or-later)](../../LICENSE)** com a **Exceção de Vinculação para DAWs e Hosts** (Seção 7 da GPLv3).

* **Compatibilidade com DAWs e Aplicativos DJ:** Concede-se permissão expressa para vincular e carregar `bcd2000_asio.dll` e `bcd2000_midi.dll` a partir de qualquer software host de áudio (Mixxx, Ableton Live, FL Studio, REAPER, Traktor, etc.), independentemente dos termos de licença do aplicativo anfitrião.
* **Cabeçalhos ASIO Clean-Room:** As definições de interface ASIO são declarações limpas e independentes da ABI pública COM, desenvolvidas para interoperabilidade binária sem qualquer código proprietário do SDK da Steinberg.
* **Aviso de Marca:** ASIO é marca registrada e software da Steinberg Media Technologies GmbH.
* **Autores e Créditos:** Consulte [AUTHORS](../../AUTHORS) para os direitos autorais e agradecimentos de engenharia reversa (incluindo o trabalho pioneiro de Mario Kicherer no Linux ALSA e a equipe do Mixxx).

---

[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) Español](README.es.md) | [![PT](flags/br.svg) **Português**](README.pt.md) | [![DE](flags/de.svg) Deutsch](README.de.md) | [![FR](flags/fr.svg) Français](README.fr.md) | [![ZH](flags/cn.svg) 中文](README.zh.md) | [![JA](flags/jp.svg) 日本語](README.ja.md)
