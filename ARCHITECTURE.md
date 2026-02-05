# Architecture

This project is an ESP32 clock/audio device with a 4x7-segment display (74HC595), Bluetooth A2DP sink, local SD player, Wi-Fi config/time sync, and alarms. Firmware is organized into modules under `main/`.

## High-level flow
- `app_main.c` boots NVS/config, initializes subsystems, starts tasks, and sets the initial UI mode.
- UI mode switching is centralized in `app_set_ui_mode()` and executed via `ui_cmd_task` to avoid ISR/input context.
- Rendering is done by the display task: time by default, overlays for mode/event feedback.
- Runtime logs are trimmed: mode changes and heap snapshots stay at `INFO`, most details are `DEBUG`.
- Time validity: before NTP/manual time set, the display shows `--:--` (dashes with colon).
- Wi-Fi provisioning is manual: menu enables AP-only setup, then STA connect after credentials are saved.

## Core modules
- `app_main.c`
  - Boot sequence and orchestration.
  - Owns global config (`app_config_t`) and UI mode state.
- `app_control.h/c`
  - Shared UI mode enum and helpers (`app_get_ui_mode`, `app_set_ui_mode`, `app_request_ui_mode`).
- `config_store.*`
  - Loads/saves persistent configuration (volume, EQ, brightness, alarm, timezone, etc.).
- `config_owner.*`
  - Single-writer owner task for `app_config_t` updates.
  - All runtime writes go through `config_owner_request_update`.
- `clock_time.*`
  - RTC/timezone handling and time retrieval.

## Display system
- `display_74hc595.*`
  - Low-level 7-seg driver (bit-bang or SPI).
  - Optional hardware brightness via OE PWM (LEDC) or software PWM.
- `display_ui.*`
  - Holds base time state and overlay state.
  - `display_ui_render()` resolves overlays vs. normal time.
- `display_bt_anim.*`
  - Bluetooth display animation controller.
  - Uses `audio_spectrum` levels.
- `ui_display_task.*`
  - Dedicated FreeRTOS task that updates time/colon and renders overlays.

## Audio and Bluetooth
- `audio_pcm5102.*`
  - I2S output (PCM5102), tone/alarm playback, volume control.
- `audio_eq.*`
  - 2-band shelving EQ (low/high) applied in `audio_i2s_write`.
  - Low shelf @ 150 Hz, high shelf @ 5 kHz, range +/-6 dB (steps 0..30, center=15).
- `alarm_playback.*`
  - Alarm scheduler playback loop, repeat timers, and stop logic.
  - Chooses alarm source by mode:
    - Clock/Player: MP3 from SD via `alarm_sound` if files exist.
    - Bluetooth: built-in tone via `alarm_tone` (no SD or MP3).
- `alarm_sound.*`
  - MP3 alarm playback from `/sdcard/alarm` (Helix).
  - Lazy-initialized task (created only on first use).
- `alarm_tone.*`
  - Lightweight alarm tone task used in Bluetooth mode.
- `bluetooth_sink.*`
  - Bluetooth init, GAP, A2DP sink setup, and discoverability.
- `bt_app_av.*`
  - A2DP callbacks and stream configuration, forwards audio data to ring buffer.
- `bt_app_core.*`
  - Ring buffer + I2S writer task for BT audio.
  - Prefetch is deterministic: playback starts only after the byte watermark.
  - `BtI2STask` runs at fixed priority `9`.
- `bt_avrc.*`
  - AVRCP control/metadata and absolute volume.
- `audio_spectrum.*`
  - Lightweight 4-band visualizer (display only).

## Storage and player
- `storage/storage_sd_spi.*`
  - SD card over SPI; mount/unmount.
- `audio_player.*`
  - Local file playback from SD (MP3/WAV).
  - Decode task pinned to core 1.
  - Stores only track count and playback order indices (no filename list).
  - Resolves filename on demand by scanning directory index.

## Connectivity and web UI
- `wifi_ntp.*`
  - Wi-Fi lifecycle + time sync (NTP).
  - Two runtime modes:
    - Provisioning mode: AP + web only (manual start from menu).
    - Normal mode: STA for NTP/runtime connectivity.
  - AP SSID/password are `ClockSetup` / `12345678`.
- `web_config.*`
  - Minimal Wi-Fi config UI (SSID/pass/reset only).

## Input and UI logic
- `ui_input.*`
  - Thin wrapper to init encoder/ADC key drivers.
- `ui_input_handlers.*`
  - Mode switching, volume control, playback controls, menu/time-setting entry.
- `ui_menu.*`
  - Menu rendering and interactions.
  - Alarm tone selection is blocked in Bluetooth mode and shows `frbd`.
- `ui_time_setting.*`
  - Time set mode, rendering and adjustments.
- `alarm_timer.*`
  - Alarm scheduler and callbacks.
- `power_manager.*`
  - Power state and soft-power logic.

## Tasks and timers
- Tasks:
  - `ui_cmd_task`, `ui_input`, `cfg_owner`, `display_task`
  - `alarm_timer`, `alarm_playback`, `alarm_sound`, `alarm_tone`
  - `audio_task`, `audio_player`
  - `BtAppTask`, `BtI2STask`, `audio_spectrum`
  - `encoder_task`, `adc_keys`, `led_indicator`
  - `wifi_shutdown`, `web_cfg_stop`
- Timers:
  - Display refresh software PWM timer (if no OE PWM).
  - Alarm repeat/stop timers.

## Data flow summary
- Inputs (encoder/ADC) -> `ui_input_handlers` -> `app_request_ui_mode` / volume / menu / time set.
- BT A2DP -> `bt_app_av` -> ring buffer -> `bt_app_core` -> `audio_i2s_write` (EQ) -> I2S out.
- Display task -> `display_ui` -> `display_74hc595`.
- BT audio -> `audio_spectrum` -> `display_bt_anim`.

## Build entry
- `main/CMakeLists.txt` registers all modules.
