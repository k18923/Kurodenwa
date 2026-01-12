# Kurodenwa (ESP32)

ESP32-DevKitC WROOM-32 を使って黒電話を Bluetooth HFP 子機化するプロジェクトの新規版です。

## 目標 (優先順)

1. HFP 通話
2. I2S 音声 (WM8960)
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

- ログは `log/` に `log.*.txt` として保存される
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
- BCK: GPIO18
- LCK/LRCK: GPIO19
- DIN (ESP32→PCM5102A): GPIO23
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
- BCK: GPIO18
- LRCK: GPIO19
- DOUT (PCM1808→ESP32): GPIO20
- SCK/MCLK: GPIO25（ESP32のI2S MCLK出力）
  - 注意: MCLKが必要なADCなので、I2S設定でMCLKを有効化する

### KS0835F（電話回線エミュレータ）

- RM (Ringing Mode): GPIO16
- SHK (Switch Hook): GPIO17
- DP (Dial Pulse): GPIO22

## 構成

```
main/
  main.c
  hfp.c/hfp.h
  audio_i2s.c/audio_i2s.h
  ring_control.c/ring_control.h
  dial_hook.c/dial_hook.h
```
