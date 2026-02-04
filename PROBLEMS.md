# Problems / Known Issues

## P-001: Menu mode switch to Bluetooth may fail after alarm ack by volume knob

- Status: open (do not fix yet)
- Repro:
  1. Play music from SD (Player mode).
  2. Alarm triggers.
  3. Acknowledge alarm by rotating volume (encoder CW/CCW), not by button.
  4. Open menu with encoder, select `BLUE`, press short.
- Actual:
  - Mode change to Bluetooth may not happen, music continues.
  - After menu timeout (~10s), switching still works to Clock.
  - If first switch to Clock, then to Bluetooth, it works.
  - If alarm is acknowledged by encoder button, issue is usually not reproduced.
- Hypothesis:
  - `ui_cmd` queue can be flooded by encoder rotation events after alarm ack;
    `UI_CMD_SET_MODE` from menu may be delayed or dropped in that window.
