# Clock (ESP32, ESP-IDF)

Firmware for an ESP32-based clock/audio device with a 4-digit 7-segment display, SD MP3 player, Bluetooth sink, and alarm features.

## Main Features

- Clock mode with timezone-aware time handling.
- Alarm scheduler with configurable tone/volume/repeat settings.
- SD card MP3 player (Helix decoder, PCM5102 I2S output).
- Bluetooth A2DP sink mode with AVRCP support.
- 2-band real-time EQ (applied in audio output path).
- Web Wi-Fi provisioning flow and NTP sync support.

## Hardware Stack

- MCU: ESP32 (WROOM class target).
- Display: 4x7-segment via 74HC595.
- Audio DAC: PCM5102 over I2S.
- Storage: SD card over SPI.
- Input: rotary encoder + ADC keys.

## Build and Flash

Prerequisites:

- ESP-IDF 5.3.x installed and exported.

Commands:

```bash
idf.py build
idf.py -p COMx flash monitor
```

## Project Layout

- `main/app` - app orchestration, UI mode manager, menu/input handlers.
- `main/audio` - player, decoder wrapper, EQ, tones, audio ownership.
- `main/connectivity` - Bluetooth, Wi-Fi/NTP, web config.
- `main/display` - display driver and UI rendering.
- `main/storage` - SD card mount/unmount.
- `main/config` - persistent config store and owner task.
- `build` - generated artifacts (do not edit manually).

## Documentation

- `ARCHITECTURE.md` - architecture overview and module map.
- `ARCHITECTURE.ru.md` - architecture notes in Russian.
- `MENU.md` - menu behavior reference.
- `PROBLEMS.md` - known issues and tracking notes.
- `INSTRUCTION.md` - quick usage guide.

## CI

GitHub Actions workflow builds firmware on pushes and pull requests:

- `.github/workflows/esp-idf-build.yml`
