[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) Español](README.es.md) | [![PT](flags/br.svg) Português](README.pt.md) | [![DE](flags/de.svg) Deutsch](README.de.md) | [![FR](flags/fr.svg) **Français**](README.fr.md) | [![ZH](flags/cn.svg) 中文](README.zh.md) | [![JA](flags/jp.svg) 日本語](README.ja.md)

# Behringer BCD2000 — Pilote Natif ASIO & MIDI 64 bits (Windows 10/11)

**Une toute nouvelle suite de pilotes open-source 64 bits qui redonne vie au contrôleur DJ classique Behringer BCD2000 sur les DAW et logiciels DJ modernes.**

[Documentation Technique & Architecture (TECHNICS.fr.md)](TECHNICS.fr.md)


---

## Bon retour, BCD2000 !

Si vous avez encore une **Behringer B-Control Deejay BCD2000** sur votre bureau ou rangée dans un flight-case, vous connaissez sûrement ce sentiment doux-amer : le matériel est solide comme un roc — faders, potentiomètres d'égalisation, jog wheels, préampli casque et sorties RCA fonctionnent toujours — mais les PC modernes la considéraient comme un presse-papier.

**Aujourd'hui, cela change.** Ce projet offre à la BCD2000 ce que Behringer n'a jamais publié : un **véritable pilote audio ASIO 64 bits et un pilote MIDI 64 bits natifs**, conçus de zéro pour **Windows 10 et Windows 11 (x64)**.

Branchez-la, lancez un seul installateur et réutilisez votre BCD2000 dans des logiciels 64 bits modernes comme **Mixxx**, **Ableton Live**, **REAPER**, **FL Studio**, **Traktor**, et bien d'autres.

---

## D'où nous venons et pourquoi cette initiative existe

Behringer a lancé la BCD2000 au milieu des années 2000 sous Windows XP / Vista et **a arrêté le support officiel aux pilotes Windows 32 bits**. Contrairement aux périphériques USB Audio Class ou USB-MIDI standard, la BCD2000 utilise un protocole propriétaire sur son microcontrôleur 8051 (`VID 0x1397`, `PID 0x00BD`). Sans pilote dédié, Windows 10 et Windows 11 ne peuvent tout simplement pas communiquer avec elle.

Pendant des années, les propriétaires n'avaient que des solutions bancales : garder un vieux PC 32 bits sous Windows XP/7, bricoler des machines virtuelles instables ou abandonner un contrôleur parfaitement fonctionnel.

Cette initiative est née d'une conviction simple : **du bon matériel ne doit pas finir en déchet électronique simplement parce que le fabricant l'a abandonné.** Grâce aux bases de rétro-ingénierie posées par le pilote libre Linux (`snd-bcd2000`) et aux API USB modernes en mode utilisateur de Windows, nous avons développé de zéro une pile logicielle 64 bits propre et fiable pour la communauté BCD2000.

---

## Pourquoi ce n'est PAS « d'anciens pilotes en mode de compatibilité Windows 7 »

On nous demande souvent : *« Pourquoi ne pas simplement lancer l'ancien installateur Behringer en mode de compatibilité Windows 7 ? »*

Voici la réalité technique :

1. **Le mode de compatibilité ne fonctionne pas sur les pilotes noyau.** Le mode de compatibilité Windows ne modifie que le comportement d'anciennes applications (`.exe`). Un noyau Windows 64 bits **ne peut en aucun cas charger des pilotes noyau `.sys` 32 bits** — aucune option ne permet de contourner cette limite.
2. **Les DAW 64 bits ne peuvent pas charger des DLL ASIO 32 bits.** Les logiciels de MAO et de DJ modernes sont des processus natifs 64 bits et exigent une bibliothèque COM ASIO 64 bits (`x64`) ainsi que des ports MIDI 64 bits natifs.
3. **Aucun ancien binaire Behringer n'est réutilisé ici.** Ce projet **ne** patche ni n'encapsule le logiciel abandonné de 2006. C'est du **code C++ 64 bits 100 % original**, exécuté en mode utilisateur au-dessus de **`WinUSB.sys`** (le pilote USB officiel intégré à Windows 10/11). Aucun hack du noyau ni désactivation de la signature des pilotes requis.

---

## Ce que vous obtenez (l'essentiel)

* **Audio ASIO Natif 64 bits (`Behringer BCD2000 ASIO 64-bit`) :**
  * 4 canaux de sortie : **Master Gauche/Droite (RCA arrière)** + **Casque Gauche/Droite (Cue façade TRS 6,35 mm)** en PCM 44,1 kHz / 16 bits.
  * 4 canaux d'entrée exposés via ASIO (`In 1–4`).
  * Streaming temps réel à faible latence avec suivi d'horloge matériel pour un son propre et sans craquements.
* **MIDI IN & OUT Simultané 64 bits (`Behringer BCD2000 MIDI`) :**
  * Prise en charge complète des faders, potentiomètres, jog wheels, boutons et **retour LED bidirectionnel**, **en même temps** que l'audio ASIO sans conflit d'accès exclusif.
* **Installateur en un clic (`Behringer_BCD2000_Setup_x64.exe`) :**
  * Associe automatiquement le pilote natif `WinUSB` à votre BCD2000 (vous n'avez **pas** besoin de télécharger ou configurer Zadig manuellement).
  * Enregistre proprement les pilotes ASIO et MIDI 64 bits avec un désinstallateur standard.
* **Preset Mixxx Sécurisé Inclus (`Behringer BCD2000 (Driver Safe)`) :**
  * Preset optionnel pour Mixxx 2.x ajoutant le *soft-takeover* et empêchant que des potards d'EQ ou Master laissés à zéro ne coupent le son au démarrage de Mixxx.

---

## Guide de Démarrage Rapide

### Téléchargements & Paquets de Version
Téléchargez la dernière version validée sur la page officielle des **[GitHub Releases](https://github.com/martinrevert/bcd2000_ASIO_64bit/releases)** :
* **`Behringer_BCD2000_Setup_x64.exe`** : Installateur automatique recommandé (inclut pilote WinUSB, DLL ASIO/MIDI et preset Mixxx).
* **`bcd2000_v2.x.x_binaries_x64.zip`** : Archive portable autonome pour installations manuelles.
* **`SHA256SUMS.txt`** : Empreintes de hachage SHA-256 pour vérifier l'intégrité des fichiers.

### Prérequis
* PC sous **Windows 10 ou Windows 11 (64 bits / x64)**.
* **Behringer BCD2000** branchée de préférence sur un port USB direct (évitez les hubs USB non alimentés).

### Étapes d'Installation
1. **Fermez** toute application DAW ou DJ ouverte (Mixxx, Ableton Live, REAPER, etc.).
2. Exécutez **`Behringer_BCD2000_Setup_x64.exe`** en tant qu'Administrateur et suivez l'assistant.
   * *(Optionnel)* Cochez l'installation du preset sécurisé si vous utilisez Mixxx.
3. Si votre BCD2000 était déjà branchée pendant l'installation, **débranchez puis rebranchez le câble USB** une fois pour appliquer la liaison proprement.
4. Ouvrez votre DAW ou logiciel DJ et sélectionnez :
   * **Périphérique Audio (ASIO) :** `Behringer BCD2000 ASIO 64-bit`
   * **Contrôleur MIDI (IN & OUT) :** `Behringer BCD2000 MIDI`
5. **Astuce pour Mixxx :** Sélectionnez le mapping **`Behringer BCD2000 (Driver Safe)`** fourni par l'installateur. Si vous utilisez le preset d'origine de Mixxx, placez vos potards d'EQ physiques (Low / Mid / High) au centre avant d'activer le contrôleur.

---

## Pour les Développeurs et Contributeurs

Vous souhaitez consulter le diagramme d'architecture, découvrir comment nous avons résolu la dérive d'horloge USB 1.1 et le partage WinUSB audio+MIDI, compiler depuis les sources ou contribuer ?

👉 Consultez la documentation technique complète dans **[TECHNICS.fr.md](TECHNICS.fr.md)**.
🤖 Les assistants IA et agents de code doivent respecter **[AGENTS.md](../../AGENTS.md)**.

---

## Licence et Mentions Légales

Ce projet est un logiciel libre distribué selon les termes de la **[GNU General Public License v3 ou ultérieure (GPL-3.0-or-later)](../../LICENSE)** avec l'**Exception de Liaison pour DAWs et Hôtes** (Section 7 de la GPLv3).

* **Compatibilité avec les DAWs et Logiciels DJ :** Il est expressément permis de lier et charger dynamiquement `bcd2000_asio.dll` et `bcd2000_midi.dll` depuis n'importe quel logiciel hôte audio (Mixxx, Ableton Live, FL Studio, REAPER, Traktor, etc.), quelles que soient les conditions de licence de l'hôte.
* **En-têtes ASIO Clean-Room :** Les définitions d'interface ASIO incluses sont des déclarations libres et indépendantes de l'ABI COM publique, conçues pour l'interopérabilité binaire sans aucun code propriétaire du SDK de Steinberg.
* **Mention de Marque :** ASIO est une marque déposée et un logiciel de Steinberg Media Technologies GmbH.
* **Auteurs et Remerciements :** Consultez [AUTHORS](../../AUTHORS) pour les auteurs et crédits d'ingénierie inverse (incluant les recherches pionnières de Mario Kicherer sur Linux ALSA et l'équipe Mixxx).

---

[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) Español](README.es.md) | [![PT](flags/br.svg) Português](README.pt.md) | [![DE](flags/de.svg) Deutsch](README.de.md) | [![FR](flags/fr.svg) **Français**](README.fr.md) | [![ZH](flags/cn.svg) 中文](README.zh.md) | [![JA](flags/jp.svg) 日本語](README.ja.md)
