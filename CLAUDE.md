# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Kurodenwa is an ESP32-based project that transforms a vintage Japanese rotary telephone (601A model) into a Bluetooth HFP (Hands-Free Profile) handset. Users can make/receive calls on modern devices using the retro telephony interface.

- **Platform**: ESP32-DevKitC WROOM-32
- **Framework**: ESP-IDF v5.5.2
- **Language**: C

## Build Commands

```bash
# Build
make build

# Flash to device (PORT auto-detected)
make flash

# Monitor serial output
make monitor

# Flash and monitor with logging
make start

# ESP-IDF configuration menu
make menuconfig

# Clean build
make clean
```

Manual port specification: `make flash PORT=/dev/cu.usbserial-XXX`

## Architecture

### Component Responsibilities

| File | Purpose |
|------|---------|
| `main.c` | State machine, event coordination, UART console |
| `hfp.c` | Bluetooth HFP client, auto-connect, call control |
| `audio_i2s.c` | I2S audio: HFP→speaker (48kHz), mic→HFP (8kHz) |
| `ring_control.c` | Bell control via KS0835F (GPIO5/19) |
| `dial_hook.c` | Hook/rotary dial detection (GPIO21) |

### State Machine (main.c)

```
IDLE ←→ RINGING
  ↓
OFFHOOK_IDLE → DIALING → OUTBOUND_RING → TALKING
```

- **IDLE**: On-hook, waiting
- **RINGING**: Incoming call, bell ringing
- **OFFHOOK_IDLE**: Receiver lifted, awaiting dial input
- **DIALING**: Rotary dial pulse detection
- **OUTBOUND_RING**: Outbound call, waiting for answer
- **TALKING**: Active call

### Audio Pipeline

- HFP receives 8kHz/16bit PCM → upsampled to 48kHz/32bit → I2S DAC (PCM5102A)
- Mic input from I2S ADC (PCM1808) → downsampled to 8kHz → HFP transmit
- mSBC codec not supported (falls back to mute)

### GPIO Assignments

| GPIO | Function |
|------|----------|
| 32 | I2S BCLK |
| 25 | I2S LRCLK |
| 33 | I2S DOUT (to DAC) |
| 34 | I2S DIN (from ADC) |
| 0 | MCLK (optional, bootstrap pin - handle carefully) |
| 5 | KS0835F F/R (bell toggle) |
| 19 | KS0835F RM (ring mode) |
| 21 | KS0835F SHK (hook detection, input+pullup) |
| 13 | Status LED |

## Console Commands (UART)

| Key | Function |
|-----|----------|
| `t` | Toggle 440Hz test tone |
| `r` | Toggle bell manually |
| `a` | Toggle auto-answer |
| `b` | Toggle ring-on-incoming |
| `d <number>` | Dial a number via HFP (e.g. `d 09012345678`) |
| `h` | Hang up the current call |
| `s` | Show status |
| `?` | Help |

## Key Configuration (hfp.c)

- `HFP_AUTO_CONNECT_BDA`: Hardcoded target device MAC address
- `HFP_AUTO_ANSWER`: 0 (manual answer by default)
- `HFP_AUTO_CONNECT_DELAY_MS`: 5000ms delay before auto-connect attempt

## Bluetooth menuconfig Requirements

- `Classic Bluetooth`: ON
- `Hands Free Unit`: ON
- `Audio Gateway`: OFF
- `audio(SCO) data path`: PCM

## Specifications

Detailed specifications are in `docs/spec.md`. Hardware datasheets are in `docs/`.
