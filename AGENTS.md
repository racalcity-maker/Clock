# AGENTS.md

Project notes for automated changes.

## Layout
- `main/` contains firmware modules.
- `build/` contains generated artifacts (do not edit by hand).

## Build / run
- Build: `idf.py build`
- Flash/monitor: `idf.py -p COMx flash monitor`

## Config persistence
- `app_config_t` is stored as a single NVS blob.
- When adding fields: update defaults, validation, and keep backward compatibility by pre-filling defaults before load.
- Runtime updates must go through `config_owner_request_update` (owner task) to avoid races.

## Display
- 4x7-seg (74HC595). Use only supported characters; add new glyphs in `display_encode_char`.
- Brightness uses LEDC or software PWM; avoid heavy work in display refresh.

## Audio
- Output path goes through `audio_i2s_write`.
- EQ is applied in `audio_i2s_write` and must stay allocation-free and fast.
- If changing sample rate, call `audio_eq_set_sample_rate`.

## Memory / performance
- Avoid heap allocations in realtime paths (audio, display, BT I2S).
- Prefer fixed buffers and small chunk sizes.
