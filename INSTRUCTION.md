# Quick Instruction

This file is a short user guide for daily device usage.

## 1) Basic Control

- Rotate encoder: navigate items / adjust values.
- Short encoder press: confirm/select.
- Long encoder press: open menu or go one level back.
- ADC keys (if wired): mode and quick actions based on current UI state.

## 2) Modes

- `CLCK` - clock display mode.
- `PLYR` - SD card player mode.
- `BLUE` - Bluetooth audio sink mode.

Mode switching is handled by the UI command task; transitions are asynchronous.

## 3) Audio

- Player reads tracks from `/sdcard/music`.
- Bluetooth mode uses A2DP sink input.
- EQ settings are shared and applied in `audio_i2s_write`.
- Alarm playback uses a dedicated path and takes audio ownership when active.

## 4) Wi-Fi / Web Setup

- Web interface is started manually from menu (`InOn`).
- Provisioning starts in AP mode (setup page).
- After saving credentials, system attempts STA connect.
- On successful STA connect, setup AP is stopped and interface flag is cleared.

## 5) Build / Flash

```bash
idf.py build
idf.py -p COMx flash monitor
```

## 6) Troubleshooting

- If SD playback fails, check card mount logs and `/sdcard/music` content.
- If Bluetooth cannot start after many mode changes, check `PROBLEMS.md`.
- For memory tracking, use heap logs around mode transitions.
