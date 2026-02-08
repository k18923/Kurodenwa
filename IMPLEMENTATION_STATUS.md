# Kurodenwa Implementation Status & TODO

## Project Overview
**Platform**: ESP32-DevKitC (WROOM-32)
**Goal**: Bluetooth HFP Handset using Retro 601A Black Phone

## Functional Implementation Status

### 1. Bluetooth HFP (Hands-Free Profile)
- **Status**: ✅ Implemented / 🚧 Testing
- **Files**: `hfp.c`, `hfp.h`
- **Implemented**:
  - [x] Initialization & SLC (Service Level Connection)
  - [x] Call Answer (`hfp_answer_call`)
  - [x] Call Hangup (`hfp_hangup_call`)
  - [x] Dial Number (`hfp_dial_number`)
  - [x] Audio Gateway / Handsfree Unit Role configuration
- **Pending / In Progress**:
  - [ ] **mSBC (Wideband Speech)**: Verify codec negotiation (CVSD vs mSBC).
  - [ ] **Connection Stability**: Handling "Connection Accept Timeout" and auto-reconnect logic.
  - [ ] **Volume Control**: Syncing HFP volume events with I2S gain.

### 2. Audio System (I2S)
- **Status**: ✅ Implemented
- **Files**: `audio_i2s.c`, `audio_i2s.h`
- **Hardware**: PCM5102A (DAC), PCM1808 (ADC)
- **Implemented**:
  - [x] I2S Driver Installation (Standard & Full Duplex if needed)
  - [x] Test Tone Generation (440Hz) for debugging
- **Pending**:
  - [ ] **Noise Management**: Mute output when HFP is not active (prevent buzzing).
  - [ ] **Sample Rate Sync**: Ensure 8kHz/16kHz matches HFP negotiation.
  - [ ] **Latency Tuning**: Adjust DMA buffer sizes for minimal delay.

### 3. Telephony Interface (Ring/Hook/Dial)
- **Status**: 🚧 Partial Integration
- **Files**: `dial_hook.c`, `ring_control.c`, `main.c`
- **Implemented**:
  - [x] **Hook Detection**: Detects On-hook/Off-hook transitions.
  - [x] **Rotary Dial**: Pulse counting (10pps) and Digit determination.
  - [x] **Ringer**: PWM/GPIO control for KS0835F bell generation (20Hz).
- **Pending**:
  - [ ] **State Synchronization (P0)**:
    - *Critical*: Fix race conditions where hanging up doesn't end the call immediately.
    - *Critical*: Ensure "Off-hook" answers an incoming call reliably.
  - [ ] **Ring Patterns**: Modify `ring_control.c` to support standard cadences (e.g., 1s Ring / 2s Silent) instead of continuous ringing.

### 4. System & Integration
- **Status**: ✅ Functional
- **Files**: `main.c`, `Makefile`
- **Implemented**:
  - [x] CLI Console (UART) for debugging (`t`, `r`, `s` commands).
  - [x] Finite State Machine (IDLE -> DIALING -> TALKING).
  - [x] **Cleanup**: Legacy `wm8960.c` and `wm8960.h` removed (unused, PCM5102A/PCM1808 used instead).
- **Pending**:
  - [ ] **Power Saving**: Implement KS0835F Power Down (PD) control.

## Usage Guide (CLI)
commands available in UART console:
- `t`: Toggle 440Hz test sine wave.
- `r`: Trigger ringer (manual test).
- `s`: Show current system state.
- `a`: Toggle Auto-Answer mode.
