# Kurodenwa (ESP32)

ESP32-DevKitC WROOM-32 を使って黒電話を Bluetooth HFP 子機化するプロジェクトの新規版です。

## 使用モジュール

- 黒電話 601A
- KS0835F: SLIC(Subscriber Line Interface Circuuit)
- PCM5102A (I2S DAC, スピーカー)
- PCM1808 (I2S ADC, マイク)

## 目標 (優先順)

1. HFP 通話
2. I2S 音声
3. リング制御 (KS0835F)
4. ダイヤル/フック検出

## ビルド

ESP-IDF をインストールした状態で:

```bash
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p /dev/tty.usbserial-XXXX flash monitor
```

Makefile を使う場合:

```bash
make build
make menuconfig
make flash PORT=/dev/tty.usbserial-XXXX
make monitor PORT=/dev/tty.usbserial-XXXX
```

ログを保存する場合:

```bash
make log PORT=/dev/tty.usbserial-XXXX
```

- ログは `log/monitor.log` に保存される
- 停止: `Ctrl+T` → `L`
- 終了: `Ctrl+]`

## Bluetooth 設定 (menuconfig)

ESP-IDF の menuconfig で以下を設定してください。

- `Component config -> Bluetooth -> Bluedroid Options -> Classic Bluetooth`: ON
- `Component config -> Bluetooth -> Bluedroid Options -> Classic Bluetooth -> Hands Free/Handset Profile`
  - `Hands Free Unit`: ON
  - `Audio Gateway`: OFF
  - `audio(SCO) data path`: PCM
- `Component config -> Bluetooth -> Bluedroid Options -> Enable Bluedroid`: ON (項目がある場合)
- `Component config -> Bluetooth -> Bluetooth Low Energy`: 任意（メモリ節約したい場合はOFF）

## 配線メモ (仮)

### PCM5102A (I2S DAC)

- VIN: 5V
- GND: GND
- BCK: GPIO32
- LCK/LRCK: GPIO25
- DIN (ESP32→PCM5102A): GPIO33
- SCK/MCLK: GNDに接続（内部PLL用。未接続でも動く個体あり）
- ジャンパ設定（PCM5102Aモジュール）:
  - XSMT: H（出力ON）
  - FMT: L（I2S）
  - DEMP: L（デエンファシス無効）
  - FLT: L（シャープロールオフ）

#### 今回の修正内容

- 矩形波→正弦波（クリアな音）
- I2Sスロット設定を16ビットで明示しPCM5102Aと互換
- SCKピンをGNDへ接続（内部PLL用）
- 無音時のノイズ対策: HFP無効時はゼロ出力

### PCM1808 (I2S ADC)

- AVDD: 5V（アナログ電源）
- DVDD: 3.3V（デジタル電源）
- GND: GND
- BCK: GPIO32
- LRCK: GPIO25
- DOUT (PCM1808→ESP32): GPIO34
- SCK/MCLK: GPIO0（ESP32のI2S MCLK出力）
  - 注意: MCLKが必要なADCなので、I2S設定でMCLKを有効化する（無効化する場合は `AUDIO_USE_MCLK` を 0 にする）

### KS0835F（電話回線エミュレータ）

最新接続表（GPIO 5/19/21 使用案）:

| KS0835F ピン | ピン名 | ESP32 GPIO | 役割と動作のポイント |
| --- | --- | --- | --- |
| 3 | F/R | GPIO5 | 20Hz〜25Hz でトグルしてベル信号生成 |
| 4 | RM | GPIO19 | リンギング時は HIGH、待機時は LOW |
| 5 | SHK | GPIO21 | フック検出。オフフックで HIGH、10ms 程度のデバウンス推奨 |
| 9 | GND | GND | 共通グランド |
| 10 | +VDC | 5V / 3.3V | 電源入力。ベル鳴動時は電流が増えるので注意 |
| 11 | PD | NC / GPIO | パワーダウン。LOW で停止。HIGH 直結は避ける |

## 構成

```
main/
  main.c
  hfp.c/hfp.h
  audio_i2s.c/audio_i2s.h
  ring_control.c/ring_control.h
  dial_hook.c/dial_hook.h
```

## 状態遷移仕様（黒電話）

### 状態（State）
| ID | 説明 |
| --- | --- |
| IDLE | 待受（オンフック） |
| RINGING | 着信中（ベル鳴動） |
| OFFHOOK_IDLE | オフフック直後（まだダイヤルなし） |
| DIALING | ダイヤルパルス送出中 |
| OUTBOUND_RING | 相手呼び出し中 |
| TALKING | 通話中 |

### イベント（Event）
#### ユーザ操作
| ID | 説明 |
| --- | --- |
| HOOK_OFF | 受話器を上げる |
| HOOK_ON | 受話器を置く |
| DIAL_START | ダイヤル回し始め |
| DIAL_PULSE | パルス1回 |
| DIAL_DIGIT_END | 1桁完了 |

#### 内部イベント
| ID | 説明 |
| --- | --- |
| NUMBER_COMPLETE | 番号入力完了（タイムアウト） |

#### 局／外部イベント
| ID | 説明 |
| --- | --- |
| RING_START | 着信開始 |
| RING_STOP | 着信停止 |
| PEER_ANSWER | 相手応答 |
| PEER_HANGUP | 相手切断 |

### 主な状態遷移
| 現在状態 | イベント | 次状態 | 備考 |
| --- | --- | --- | --- |
| IDLE | RING_START | RINGING |  |
| IDLE | HOOK_OFF | OFFHOOK_IDLE |  |
| RINGING | HOOK_OFF | TALKING | 応答 |
| RINGING | RING_STOP | IDLE |  |
| OFFHOOK_IDLE | DIAL_START | DIALING |  |
| OFFHOOK_IDLE | HOOK_ON | IDLE |  |
| DIALING | DIAL_DIGIT_END | OFFHOOK_IDLE | 次桁待ち |
| DIALING | NUMBER_COMPLETE | OUTBOUND_RING | 発信開始 |
| DIALING | HOOK_ON | IDLE |  |
| OUTBOUND_RING | PEER_ANSWER | TALKING |  |
| OUTBOUND_RING | PEER_HANGUP | OFFHOOK_IDLE | 話中音 |
| OUTBOUND_RING | HOOK_ON | IDLE |  |
| TALKING | HOOK_ON | IDLE |  |
| TALKING | PEER_HANGUP | OFFHOOK_IDLE |  |
