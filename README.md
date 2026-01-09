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

### WM8960 (I2S/I2C)

- I2S BCLK: GPIO18
- I2S LRCLK: GPIO19
- I2S DOUT (ESP32→WM8960): GPIO21
- I2S DIN (WM8960→ESP32): GPIO20
- I2C SDA: GPIO2
- I2C SCL: GPIO3
- MCLK: 不要（WM8960モジュールの24MHz水晶を使用）

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
