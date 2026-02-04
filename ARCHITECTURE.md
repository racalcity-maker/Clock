# Architecture

This project is an ESP32 clock/audio device with 4x7-segment display (74HC595), Bluetooth A2DP sink, local SD player, Wi-Fi config/clock sync, and alarms. The firmware is organized into small modules under `main/`.

## High-level flow
- `app_main.c` boots NVS/config, initializes subsystems, starts tasks, and sets the initial UI mode (clock).
- UI mode switching is centralized in `app_set_ui_mode()` and executed via `ui_cmd_task` to avoid doing it in ISR/input context.
- Rendering is done by the display task, which sets time by default and overlays animation/text based on mode and events.
- Runtime logs are intentionally trimmed: mode changes and heap snapshots stay at `INFO`, transition details are mostly `DEBUG`.
- Runtime resources are mode-scoped:
  - Clock/Player modes stop BT streaming task, deinit BT sink, and release BT ring buffer memory.
  - Bluetooth mode stops player + flushes I2S, unmounts SD, pauses display/power tasks, and reserves BT ring buffer.
- Wi-Fi provisioning is user-driven:
  - Enabling web interface (`InOn`) starts AP setup mode only (no STA connect attempt).
  - Saving credentials in web UI switches AP -> STA attempt.
  - On successful STA connect, interface flag is auto-cleared and AP/web setup is stopped.

## Core modules
- `app_main.c`
  - Boot sequence and orchestration.
  - Owns global config (`app_config_t`) and UI mode state.
  - Starts/stops Wi-Fi, BT discoverability, SD, and web config depending on mode.
- `app_control.h/c`
  - Shared UI mode enum and helpers (`app_get_ui_mode`, `app_set_ui_mode`, `app_request_ui_mode`).
- `config_store.*`
  - Loads/saves persistent configuration (volume, EQ, brightness, alarm, timezone, etc.).
- `config_owner.*`
  - Single-writer owner task for `app_config_t` updates.
  - All runtime config writes go through `config_owner_request_update` to avoid races.
- `clock_time.*`
  - RTC/timezone handling and time retrieval for display.

## Display system
- `display_74hc595.*`
  - Low-level display driver (bit-bang or SPI).
  - Optional hardware brightness via OE PWM (LEDC) or software PWM via esp_timer.
- `display_ui.*`
  - Holds base time state and overlay state.
  - `display_ui_render()` resolves overlays vs. normal time.
- `display_bt_anim.*`
  - Bluetooth display animation controller.
  - Uses `audio_spectrum` levels and schedules phases (cubes/spectrum/text).
- `ui_display_task.*`
  - Dedicated FreeRTOS task that:
    - Updates time/colon.
    - Shows overlays (track info, volume, BT animation).
    - Coordinates with menu/time-setting modes.

## Audio and Bluetooth
- `audio_pcm5102.*`
  - I2S output (PCM5102), tone/alarm playback, volume control.
- `audio_eq.*`
  - 2-band shelving EQ (low/high) applied in `audio_i2s_write`.
  - Low shelf @ 150 Hz, high shelf @ 5 kHz, range +/-6 dB (steps 0..30, center=15).
- `helix_mp3_wrapper.*`
  - Helix MP3 decode wrapper with 4 KB input buffer and direct PCM write callback to I2S path.
- `bluetooth_sink.*`
  - Bluetooth init, GAP, A2DP sink setup, and discoverability.
- `bt_app_av.*`
  - A2DP callbacks and stream configuration, forwards audio data to ring buffer.
- `bt_app_core.*`
  - Ring buffer + I2S writer task for BT audio.
  - Prefetch start is deterministic: BT playback starts only after ringbuffer reaches byte watermark (`s_prefetch_start_bytes`), without packet-count shortcut.
  - `BtI2STask` runs with fixed priority `9` to avoid starving other app tasks.
  - Feeds `audio_spectrum` for visualization.
- `bt_avrc.*`
  - AVRCP control/metadata and absolute volume.
- `audio_spectrum.*`
  - Lightweight 4-band visualizer (not audio processing).

## Storage and player
- `storage/storage_sd_spi.*`
  - SD card over SPI; mount/unmount.
- `audio_player.*`
  - Local file playback from SD.
  - Decode task is pinned to core 1.
  - Keeps only track count and playback order indices (no persistent filename list).
  - Resolves current filename on demand by scanning directory index.

## Connectivity and web UI
- `wifi_ntp.*`
  - Wi-Fi lifecycle + time sync (NTP).
  - Supports two explicit runtime modes:
    - Provisioning mode: AP + web only, started manually from menu.
    - Normal mode: STA for NTP/runtime connectivity.
  - Provisioning fallback: if STA connect fails during AP->STA handoff, returns to AP setup.
- `web_config.*`
  - Minimal Wi-Fi config UI (SSID/pass/reset only).

## Input and UI logic
- `ui_input.*`
  - Thin wrapper to init encoder/ADC key drivers.
- `ui_input_handlers.*`
  - Mode switching, volume control, playback controls, menu/time-setting entry.
- `ui_menu.*`
  - Menu rendering and interactions.
  - Root menu now includes EQ (`EqUA`) entry for low/high adjustments.
- `ui_time_setting.*`
  - Time set mode, rendering and adjustments.
- `alarm_timer.*`
  - Alarm scheduler and callbacks.
- `power_manager.*`
  - Power state and soft-power logic.

## APIs
### Firmware (C) APIs
- `app_control.h`: `app_get_ui_mode`, `app_request_ui_mode`, `app_set_ui_mode`.
- `display_74hc595.h`: `display_init`, `display_set_time`, `display_set_text`,
  `display_set_segments`, `display_set_brightness`, `display_get_brightness`.
- `display_ui.h`: `display_ui_init`, `display_ui_set_time`, `display_ui_show_text`,
  `display_ui_show_digits`, `display_ui_show_segments`, `display_ui_render`.
- `display_bt_anim.h`: `display_bt_anim_reset`, `display_bt_anim_update`.
- `audio_pcm5102.h`: `audio_init`, `audio_set_volume`, `audio_i2s_write`,
  `audio_i2s_set_sample_rate`, `audio_play_tone`, `audio_play_alarm`, `audio_stop`.
- `audio_eq.h`: `audio_eq_init`, `audio_eq_set_sample_rate`, `audio_eq_set_steps`,
  `audio_eq_is_flat`, `audio_eq_process`.
- `audio_player.h`: `audio_player_init`, `audio_player_play`, `audio_player_pause`,
  `audio_player_stop`, `audio_player_next`, `audio_player_prev`,
  `audio_player_set_volume`, `audio_player_get_state`, `audio_player_get_time_ms`.
- `bluetooth_sink.h`: `bt_sink_init`, `bt_sink_set_discoverable`, `bt_sink_disconnect`,
  `bt_sink_is_connected`, `bt_sink_is_streaming`, `bt_sink_set_name`,
  `bt_sink_clear_bonds`.
- `bt_avrc.h`: `bt_avrc_send_command`, `bt_avrc_register_volume_cb`,
  `bt_avrc_notify_volume`, `bt_avrc_is_connected`.
- `config_store.h`: `config_store_init`, `config_store_get`, `config_store_update`.
- `config_owner.h`: `config_owner_init`, `config_owner_start`, `config_owner_request_update`.
- `clock_time.h`: `clock_time_init`, `clock_time_get`, `clock_time_set_timezone`.
- `wifi_ntp.h`: `wifi_init`, `wifi_set_enabled`, `wifi_is_enabled`,
  `wifi_update_credentials`.
- `storage_sd_spi.h`: `storage_sd_init`, `storage_sd_unmount`, `storage_sd_is_mounted`.
- `alarm_timer.h`: `alarm_timer_init`, `alarm_set`.
- `power_manager.h`: `power_manager_init`, `power_manager_set_autonomous`,
  `power_manager_handle_boot`.

### Web HTTP API
- `GET /` - redirects to `/wifi`.
- `GET /wifi` - Wi-Fi page and status.
- `POST /wifi` - save Wi-Fi config: `ssid`, `pass`.
- `POST /wifi_reset` - clear Wi-Fi credentials.
- Fetch requests return JSON `{ "ok": true }`.

## Tasks and timers
- Tasks:
  - `ui_cmd_task` (mode changes)
  - `ui_input` (input debounce/rate-limit dispatch)
  - `cfg_owner` (serialized config updates)
  - `display_task` (UI render loop)
  - `power_monitor` (external power monitor)
  - `alarm_timer` (alarm schedule polling)
  - `alarm_playback` / `alarm_sound` (alarm playback pipeline)
  - `BtAppTask` (BT event dispatch)
  - `BtI2STask` (BT ringbuffer -> I2S)
  - `audio_player` (SD playback decode)
  - `audio_task` (tone/alarm playback)
  - `audio_spectrum` (visualizer worker)
  - `encoder_task`, `adc_keys`, `button_task` (input drivers)
  - `led_indicator` (LED service)
  - `wifi_shutdown` / `web_cfg_stop` (deferred connectivity shutdown)
- Timers:
  - Display refresh software PWM timer (if no OE PWM).

## Data flow summary
- Inputs (encoder/ADC) -> `ui_input_handlers` -> `app_request_ui_mode` / volume / menu / time set.
- BT A2DP data -> `bt_app_av` -> ring buffer -> `bt_app_core` -> `audio_i2s_write` (EQ) -> I2S out.
- Display task -> `display_ui` -> `display_74hc595`.
- BT audio samples -> `audio_spectrum` -> `display_bt_anim`.

## Build entry
- `main/CMakeLists.txt` registers all modules.
