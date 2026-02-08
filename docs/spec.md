# Kurodenwa 仕様整理（暫定）

本ドキュメントは、プロジェクト配下の `README.md` / `TODO.md` および現状実装（`main/`）から読み取れる仕様を、実装有無を区別しながら整理したものです。

## 1. 目的 / スコープ

### 目的（優先順）
1. Bluetooth HFP 通話（ESP32 を HFP の **Hands Free Unit** として動作）
2. I2S 音声（受話器スピーカーへの再生）
3. リング制御（KS0835F でベル鳴動）
4. ダイヤル/フック検出（パルスダイヤル入力・オンフック/オフフック）

### スコープ（現状実装の前提）
- 対象ボード: ESP32-DevKitC WROOM-32
- Classic Bluetooth (Bluedroid) を使用
- HFP の SCO 音声データパスは `menuconfig` で **PCM** を選択する前提

### 非スコープ（現状未実装/未提供）
- mSBC デコード（mSBC の場合はミュート）
 - 高音質化（EQ/AGC などの高度な音質調整）

## 2. ハードウェア構成

### 使用モジュール
- 黒電話 601A
- KS0835F（SLIC / 電話回線エミュレータ）
- PCM5102A（I2S DAC: スピーカー）
- PCM1808（I2S ADC: マイク）

### GPIO 割り当て（現状実装）

| 用途 | デバイス | 信号 | ESP32 GPIO | 実装箇所 |
| --- | --- | --- | --- | --- |
| I2S BCLK | PCM5102A | BCK | GPIO32 | `main/audio_i2s.c` |
| I2S LRCLK | PCM5102A | LCK/LRCK | GPIO25 | `main/audio_i2s.c` |
| I2S DOUT | PCM5102A | DIN | GPIO33 | `main/audio_i2s.c` |
| I2S DIN | PCM1808 | DOUT | GPIO34 | `main/audio_i2s.c` |
| I2S MCLK | （PCM1808等） | SCK/MCLK | GPIO0（`AUDIO_USE_MCLK=1`） | `main/audio_i2s.c` |
| ベル制御 | KS0835F | F/R | GPIO5 | `main/ring_control.c` |
| ベル制御 | KS0835F | RM | GPIO19 | `main/ring_control.c` |
| フック/ダイヤル | KS0835F | SHK | GPIO21 | `main/dial_hook.c` |
| 動作表示 | - | LED | GPIO13 | `main/main.c` |

補足:
- `GPIO21(SHK)` は入力 + プルアップ、割り込み（両エッジ）で監視する。
- `GPIO0(MCLK)` はブートストラップピンなので、外部回路の影響に注意する（プル状態や接続方法）。

### 配線メモ（README.md の要約）
- PCM5102A: `VIN=5V`, `BCK=GPIO32`, `LRCK=GPIO25`, `DIN=GPIO33`, `SCK/MCLK=GND`（PLL利用）
- PCM1808: `AVDD=5V`, `DVDD=3.3V`, `BCK=GPIO32`, `LRCK=GPIO25`, `DOUT=GPIO34`, `MCLK=GPIO0`（必要）
- KS0835F: `F/R=GPIO5`, `RM=GPIO19`, `SHK=GPIO21`, `+VDC=5V/3.3V`, `GND=GND`

## 3. ソフトウェア構成

### ディレクトリ/主要ファイル
```
main/
  main.c                  全体状態管理・コンソール・イベント結線
  hfp.c/hfp.h             HFP（HF client）・自動接続/自動応答・音声接続制御
  audio_i2s.c/audio_i2s.h  I2S 再生（HFP受信音声の簡易処理/アップサンプル）
  ring_control.c/.h        ベル鳴動（GPIO トグル + RM 制御）
  dial_hook.c/.h           フック/パルスダイヤル検出
```

### 役割分担（概略）
- `hfp.*`: ペアリング/接続、通話状態イベントの通知、音声接続（SCO）要求/切断の補助。
- `main.c`: 黒電話としての状態（待受/着信/発信/通話）を管理し、フック/ダイヤルイベントと HFP イベントを統合して振る舞いを決める。
- `audio_i2s.*`: 受信した HFP 音声（PCM 想定）を I2S 48kHz/32bit slot/stereo で出力する（16bit 相当を上位ビットに配置）。
- `ring_control.*`: KS0835F を使ってベルを鳴らす。
- `dial_hook.*`: `SHK` のレベル変化からオンフック/オフフックとパルス数を推定する。

## 4. Bluetooth / HFP 仕様（現状実装）

### デバイス名・発見性
- デバイス名: `Kurodenwa`
- `ESP_BT_CONNECTABLE` + `ESP_BT_GENERAL_DISCOVERABLE`

### 自動接続（暫定・ハードコード）
- 起動後 `HFP_AUTO_CONNECT_DELAY_MS=5000ms` 経過後に接続を試行する。
- 接続先は `main/hfp.c` の `HFP_AUTO_CONNECT_BDA`（固定 MAC）を優先する。
  - 固定 MAC が無効（パース失敗）の場合、bonded device の先頭を使う。

### 自動応答
- 既定: OFF（`HFP_AUTO_ANSWER=0`）
- コンソールコマンド `a` で切替。

### SCO 音声接続の扱い
- `HFP_AUTO_AUDIO=0` のため、アプリ側の明示的な制御で音声接続を要求する。
- 自動切断は抑止（`HFP_AUDIO_DISCONNECT_ENABLED=0`）。

### コーデックの扱い（制限）
- mSBC のデコードは未実装。
  - `ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC` の場合は `audio_i2s_set_hfp_enabled(false)` でミュートし警告ログを出す。
- CVSD/PCM の場合: 受信データを I2S 再生へ投入し、送話はマイク入力を送出する。

## 5. 音声（I2S）仕様（現状実装）

### 出力フォーマット
- I2S: 48kHz / 32bit slot / stereo / master
- ピン: BCLK=GPIO32, LRCLK=GPIO25, DOUT=GPIO33
- `AUDIO_USE_MCLK=1` の場合: MCLK=GPIO0 を出力する

### HFP 受信音声の前提（暫定）
- HFP 受信 PCM を 8kHz・16bit（リトルエンディアン）として扱う前提で処理する。
- 48kHz への変換は単純アップサンプル（整数倍のみ）+ 線形補間 + DC ブロック + 固定ゲイン。
- 片チャンネルとして処理し、左右に同一サンプルを出力する。

### マイク送話
- PCM1808 から取り込んだデータをダウンサンプルし、DC ブロックと簡易ノイズゲートを適用して HFP 送話に使用する。

### テストトーン
- コンソールコマンド `t` で 440Hz 正弦波トーンを ON/OFF。
- トーン ON 中は HFP 音声を無効化し、ストリームバッファをクリアする（無音時ノイズ対策の一部）。

## 6. ベル（リング）仕様（現状実装）

- `ring_control_start()`:
  - `RM=HIGH`、`F/R` を一定周期でトグル。
  - 鳴動パターン: 約1秒鳴る → 約2秒止まる を繰り返す。
- トグル周期: `RING_TOGGLE_PERIOD_US=20000us`
- `ring_control_stop()`:
  - タイマ停止、`F/R=LOW`、`RM=LOW`。

## 7. フック / パルスダイヤル仕様（現状実装）

### オンフック / オフフック判定
- `HOOK_GPIO=GPIO21` を入力（プルアップ）として監視する。
- レベル `HIGH`: オフフック（受話器が上がっている想定）
- レベル `LOW` が `PULSE_THRESHOLD_MS=200ms` 以上継続: オンフック確定（`DIAL_HOOK_ON`）

### ダイヤルパルス検出（概略）
- 立下りエッジでパルス開始を検知し、`dial_start` を 1 回だけ通知する。
- 立上り後、`BRIDGE_TIME_MS=10ms` のブリッジ期間を置いてパルス幅を推定し、`MIN_PULSE_WIDTH_MS=10ms` 以上なら 1 パルスとしてカウントする。
- `INTER_DIGIT_TIMEOUT_MS=300ms` 以内に次パルスが来なければ 1 桁確定。
- 桁は `pulse_count % 10` で算出（10 パルスを `0` とする）。

## 8. 黒電話の状態機械（現状実装ベース）

### 状態（`main/main.c`）
- `IDLE`: 待受（オンフック）
- `RINGING`: 着信中（ベル鳴動）
- `OFFHOOK_IDLE`: オフフック直後（番号入力待ち）
- `DIALING`: ダイヤルパルス入力中
- `OUTBOUND_RING`: 発信中（相手呼び出し）
- `TALKING`: 通話中

### 内部データ
- ダイヤルバッファ: 最大 31 文字（`DIAL_BUFFER_SIZE=32`）
- 番号確定タイムアウト: `NUMBER_COMPLETE_TIMEOUT_MS=3000ms`
  - 直近の桁入力から 3 秒で番号確定し `hfp_dial_number()` を呼ぶ。

### 代表的な遷移（README.md の表 + 実装補足）
- 着信:
  - `INCOMING`（HFP Call Setup）→ `RINGING`（auto-answer OFF の場合）
  - `RINGING` 中にオフフック → `hfp_answer_call()` → `TALKING`
  - auto-answer ON の場合: `INCOMING` 受信時に `hfp_answer_call()` → `TALKING`（ベルは抑制）
- 発信:
  - `IDLE` でオフフック → `OFFHOOK_IDLE`
  - ダイヤル開始 → `DIALING`
  - 桁確定（複数桁可）→ `OFFHOOK_IDLE`（次桁待ち）
  - 3 秒無操作で番号確定 → `hfp_dial_number()` → `OUTBOUND_RING`
  - 通話確立（HFP Call Status `IN_PROGRESS`）→ `TALKING`
- 終話:
  - オンフック時: `hfp_hangup_call()` を要求し、必要ならリトライ（最大 3 回）
  - 相手切断時: `NO_CALLS` で `OFFHOOK_IDLE` または `IDLE` に戻す（受話器状態に追従）

## 9. コンソール（UART）

UART0 上で簡易コンソールを起動し、以下のコマンドを提供する（`main/main.c`）。

| コマンド | 機能 |
| --- | --- |
| `t` | 440Hz テストトーン ON/OFF |
| `r` | ベル手動トグル |
| `a` | 自動応答 ON/OFF |
| `b` | 鳴動 ON/OFF（着信時に鳴らすかどうか） |
| `s` | 現在状態表示 |
| `?` | ヘルプ表示 |

## 10. ビルド / 設定

### ビルド
- `README.md` の手順を参照（`idf.py` または `Makefile`）。
- `Makefile` は `IDF_PATH ?= $(HOME)/.espressif/v5.5.2/esp-idf` を既定にしている。

### menuconfig（要点）
- `Classic Bluetooth`: ON
- `Hands Free/Handset Profile`:
  - `Hands Free Unit`: ON
  - `Audio Gateway`: OFF
  - `audio(SCO) data path`: PCM

## 11. 既知の課題 / TODO（要約）

`TODO.md` より:
- P0: フック操作と通話状態の整合、ベル鳴動のパターン制御
- P1: mSBC 対応 or CVSD 固定の明確化、音量/ゲイン/ミュート、無音時ノイズ最適化
- P2: HFP再接続安定化、ダイヤル入力→発信コマンド整備、設定切替、KS0835F PD 制御/省電力

## 12. 参考資料（リポジトリ内）
- `docs/HFP_v1.9.pdf` / `docs/HFP_v1.9 ja.pdf`: HFP 仕様（参照用）
- `docs/esp32_devkitC_v4_pinlayout.png`: ピン配置
