[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) Español](README.es.md) | [![PT](flags/br.svg) Português](README.pt.md) | [![DE](flags/de.svg) Deutsch](README.de.md) | [![FR](flags/fr.svg) Français](README.fr.md) | [![ZH](flags/cn.svg) 中文](README.zh.md) | [![JA](flags/jp.svg) **日本語**](README.ja.md)

# Behringer BCD2000 — ネイティブ 64-bit ASIO & MIDI ドライバー (Windows 10/11)

**名機 Behringer BCD2000 DJ コントローラーを、最新の 64-bit DAW や DJ ソフトウェアで再び蘇らせる、完全新規のオープンソース 64-bit ドライバー・スイート。**

[技術ドキュメント & アーキテクチャ (TECHNICS.ja.md)](TECHNICS.ja.md)


---

## おかえりなさい、BCD2000！

デスクの隅やフライトケースの中に、今でも **Behringer B-Control Deejay BCD2000** が眠っていませんか？ フェーダー、EQ ノブ、ジョグホイール、ヘッドホンアンプ、RCA 出力までハードウェアは戦車のように頑丈で完璧に動くのに、現代の PC ではただの文鎮扱いされていた——そんな歯がゆい思いを経験した方は多いはずです。

**今日、その状況が変わります。** 本プロジェクトは、Behringer 公式がついにリリースしなかったもの——**Windows 10 および Windows 11 (x64) 向けにゼロから設計された、真のネイティブ 64-bit ASIO オーディオドライバーと 64-bit MIDI ドライバー**を BCD2000 に提供します。

USB をつないでインストーラーを一度実行するだけで、**Mixxx**、**Ableton Live**、**REAPER**、**FL Studio**、**Traktor** などの最新 64-bit アプリケーションで再び BCD2000 を操れます。

---

## プロジェクトの背景と目的

Behringer は 2000 年代半ば（Windows XP / Vista 時代）に BCD2000 を発売しましたが、**公式ドライバーの提供は 32-bit 版 Windows で終了しました**。一般的な USB Audio Class や USB-MIDI 準拠機器とは異なり、BCD2000 は内蔵 8051 マイコン上で独自プロトコル（`VID 0x1397`、`PID 0x00BD`）を使用しているため、専用ドライバーなしでは Windows 10 / 11 と通信できません。

長年にわたり、ユーザーは古い 32-bit Windows XP/7 マシンを残したり、不安定な仮想マシンを組んだり、あるいは動く機材を諦めるしかありませんでした。

この取り組みは、シンプルな信念から生まれました——**「メーカーがサポートを終えたからといって、優れたハードウェアを電子ゴミにしてはならない」。** オープンソースの Linux ALSA ドライバー（`snd-bcd2000`）によるリバースエンジニアリングの知見と、Windows のモダンなユーザーモード USB API を組み合わせ、私たちは 64-bit Windows 向けドライバースタックを一から構築しました。

---

## なぜ「Windows 7 互換モードで旧ドライバーを動かす」のとは全く別物なのか

よくある質問に *「古い Behringer 公式インストーラーを Windows 7 互換モードで動かせば済むのでは？」* というものがあります。

技術的な答えは明確です：

1. **互換モードはカーネルドライバーには効きません。** Windows の互換モードは古いユーザーアプリ（`.exe`）の挙動を調整するだけの機能です。64-bit 版 Windows カーネルは **32-bit の `.sys` カーネルドライバーを一切読み込めません**。
2. **64-bit DAW は 32-bit ASIO DLL を読み込めません。** 現代の DAW や DJ ソフトはネイティブ 64-bit プロセスであり、ネイティブ 64-bit COM ASIO ライブラリ（`x64`）と 64-bit MIDI ポートが必須です。
3. **古い Behringer 製バイナリは一切使っていません。** 本プロジェクトは 2006 年の旧ソフトをパッチしたりラップしたりするものではありません。Microsoft 標準搭載の **`WinUSB.sys`** 上で安全に動作する、**100% 新規書き下ろしの 64-bit C++ コード（ユーザーモード）**です。カーネル改造やドライバー署名強制の無効化、テストモードは不要です。

---

## 主な機能（基本概要）

* **ネイティブ 64-bit ASIO オーディオ（`Behringer BCD2000 ASIO 64-bit`）：**
  * 4 チャンネル出力：**Master L/R（背面 RCA）** + **Phones L/R（前面 Cue 1/4" TRS）**（44.1 kHz / 16-bit PCM）。
  * ASIO 経由で 4 チャンネル入力（`In 1–4`）を公開。
  * 低レイテンシーのリアルタイム・ストリーミングとハードウェア・クロック追従により、クリックノイズのない安定した再生を実現。
* **64-bit MIDI IN/OUT 同時利用（`Behringer BCD2000 MIDI`）：**
  * フェーダー、ノブ、ジョグホイール、ボタン、そして**双方向 LED フィードバック**を、排他ロック競合なしで ASIO オーディオと**完全に同時動作**。
* **ワンクリック・インストーラー（`Behringer_BCD2000_Setup_x64.exe`）：**
  * Windows 標準 `WinUSB` ドライバーを BCD2000 に自動バインド（手動で Zadig を導入する必要は**ありません**）。
  * 64-bit ASIO と 64-bit MIDI の両方をクリーンに登録し、標準アンインストーラーも付属。
* **Mixxx セーフ・プリセット同梱（`Behringer BCD2000 (Driver Safe)`）：**
  * Mixxx 2.x 向けのオプション・マッピング。ソフトテイクオーバーを備え、物理 EQ や Master ノブが 0 の位置のまま起動しても音が出なくなる問題を防止します。

---

## クイックスタートガイド

### 動作要件
* **Windows 10 または Windows 11（64-bit / x64）** 搭載 PC。
* **Behringer BCD2000**（バスパワー不足を避けるため、PC 本体の USB ポートへの直接接続を推奨）。

### インストール手順
1. 起動中の DAW や DJ ソフト（Mixxx、Ableton Live、REAPER など）をすべて**終了**します。
2. **`Behringer_BCD2000_Setup_x64.exe`** を管理者として実行し、ウィザードに従ってインストールします。
   * *(任意)* Mixxx をお使いの場合は Mixxx セーフ・プリセットのチェックを入れてください。
3. インストール中に BCD2000 を接続していた場合は、新しいバインドを確実に適用するため **USB ケーブルを一度抜いて差し直してください**。
4. DAW または DJ ソフトを開き、以下を選択します：
   * **オーディオデバイス (ASIO)：** `Behringer BCD2000 ASIO 64-bit`
   * **MIDI コントローラー (IN & OUT)：** `Behringer BCD2000 MIDI`
5. **Mixxx ユーザーへのヒント：** インストーラーが配置する **`Behringer BCD2000 (Driver Safe)`** マッピングを選択してください。Mixxx 標準の旧プリセットを使う場合は、コントローラーを有効化する前に物理 EQ ノブ（Low / Mid / High）を中央にしておいてください。

---

## 開発者・コントリビューターの皆さまへ

内部アーキテクチャ図、USB 1.1 水晶クロックドリフトや WinUSB オーディオ＋MIDI 共有の解決手法、ソースからのビルド方法、開発参加ルールについては以下をご覧ください。

👉 詳細な技術仕様ドキュメント：**[TECHNICS.ja.md](TECHNICS.ja.md)**
🤖 AI アシスタントおよび自動コーディングエージェントは **[AGENTS.md](../../AGENTS.md)** を必ず遵守してください。

---

## ライセンスと法的情報

本プロジェクトは、**[GNU General Public License v3 or later (GPL-3.0-or-later)](../../LICENSE)** に基づき、**ASIO およびホストアプリケーションリンク例外条項**（GPLv3 第 7 条）を追加した自由ソフトウェアとして公開されています。

* **DAW および DJ ソフトウェアとの互換性：** ホスト側のライセンス形態に関わらず、任意の DAW や DJ ソフトウェア（Mixxx、Ableton Live、FL Studio、REAPER、Traktor など）から `bcd2000_asio.dll` および `bcd2000_midi.dll` を動的リンク・ロードすることが明示的に許可されています。
* **クリーンルーム ASIO ヘッダー：** 本リポジトリに含まれる ASIO インターフェース定義は、公開 COM ABI に基づき独自に作成されたクリーンルーム仕様であり、Steinberg 社の非公開 SDK コードは一切含まれていません。
* **商標について：** ASIO は Steinberg Media Technologies GmbH の登録商標およびソフトウェアです。
* **作者と謝辞：** 著作権保持者およびリバースエンジニアリングに関する謝辞（Mario Kicherer 氏による Linux ALSA ドライバ研究や Mixxx コミュニティ）は [AUTHORS](../../AUTHORS) をご覧ください。

---

[![EN](flags/gb.svg) English](../../README.md) | [![ES](flags/es.svg) Español](README.es.md) | [![PT](flags/br.svg) Português](README.pt.md) | [![DE](flags/de.svg) Deutsch](README.de.md) | [![FR](flags/fr.svg) Français](README.fr.md) | [![ZH](flags/cn.svg) 中文](README.zh.md) | [![JA](flags/jp.svg) **日本語**](README.ja.md)
