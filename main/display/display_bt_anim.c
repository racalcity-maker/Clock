#include "display_bt_anim.h"

#include "display_74hc595.h"
#include "display_ui.h"
#include "audio_spectrum.h"
#include "esp_timer.h"

#define BT_ANIM_PERIOD_MS 60000U
#define BT_MODE_DURATION_MS 2000U
#define BT_JUMP_DURATION_MS 8000U
#define BT_BARS_DURATION_MS 40000U
#define BT_TIME_DURATION_MS 10000U
#define BT_BARS_FRAME_MS 30U
#define BT_WAVE_PERIOD_MS 1600U
#define BT_WAVE_HOLD_MS 80U

static int64_t s_bt_anim_epoch_us = 0;
static uint8_t s_bt_anim_frame_idx = 0xFF;
static int s_bt_anim_phase = -1;

void display_bt_anim_reset(int64_t now_us)
{
    s_bt_anim_epoch_us = now_us;
    s_bt_anim_frame_idx = 0xFF;
    s_bt_anim_phase = -1;
}

void display_bt_anim_update(int64_t now_us)
{
    if (s_bt_anim_epoch_us == 0) {
        display_bt_anim_reset(now_us);
    }

    uint32_t elapsed_ms = (uint32_t)((now_us - s_bt_anim_epoch_us) / 1000);
    uint32_t phase_ms = elapsed_ms % BT_ANIM_PERIOD_MS;
    int phase = 0;
    uint32_t phase_elapsed_ms = 0;
    if (phase_ms < BT_MODE_DURATION_MS) {
        phase = 0; // mode text
    } else if (phase_ms < (BT_MODE_DURATION_MS + BT_JUMP_DURATION_MS)) {
        phase = 1; // jumping blocks
        phase_elapsed_ms = phase_ms - BT_MODE_DURATION_MS;
    } else if (phase_ms < (BT_MODE_DURATION_MS + BT_JUMP_DURATION_MS + BT_BARS_DURATION_MS)) {
        phase = 2; // spectrum bars
        phase_elapsed_ms = phase_ms - BT_MODE_DURATION_MS - BT_JUMP_DURATION_MS;
    } else {
        phase = 3; // normal time
    }

    if (phase != s_bt_anim_phase) {
        s_bt_anim_phase = phase;
        s_bt_anim_frame_idx = 0xFF;
        if (phase == 0) {
            display_ui_show_text("BLUE", BT_MODE_DURATION_MS);
        }
    }

    if (s_bt_anim_phase == 0 || s_bt_anim_phase == 3) {
        return;
    }

    if (display_ui_overlay_active() && !display_ui_overlay_is_segments()) {
        return;
    }

    uint8_t segs[4] = {0, 0, 0, 0};
    uint32_t hold_ms = 200;
    if (s_bt_anim_phase == 1) {
        (void)phase_elapsed_ms;
        uint32_t t_ms = (uint32_t)(now_us / 1000);
        uint32_t phase_base = t_ms % BT_WAVE_PERIOD_MS;
        uint32_t phase_step = BT_WAVE_PERIOD_MS / 4U;
        for (int i = 0; i < 4; ++i) {
            uint32_t phase = (phase_base + (uint32_t)i * phase_step) % BT_WAVE_PERIOD_MS;
            bool top = phase < (BT_WAVE_PERIOD_MS / 2U);
            segs[i] = top ? (SEG_A | SEG_B | SEG_F | SEG_G)
                          : (SEG_D | SEG_C | SEG_E | SEG_G);
        }
        hold_ms = BT_WAVE_HOLD_MS;
    } else {
        uint32_t frame = phase_elapsed_ms / BT_BARS_FRAME_MS;
        if (frame != s_bt_anim_frame_idx) {
            s_bt_anim_frame_idx = (uint8_t)frame;
        }
        hold_ms = BT_BARS_FRAME_MS;
        uint8_t levels[4] = {0};
        audio_spectrum_get_levels(levels);
        uint8_t max_level = 0;
        for (int i = 0; i < 4; ++i) {
            if (levels[i] > max_level) {
                max_level = levels[i];
            }
        }
        if (max_level == 0) {
            for (int i = 0; i < 4; ++i) {
                segs[i] = SEG_D;
            }
            display_ui_show_segments(segs, false, hold_ms);
            return;
        }
        for (int i = 0; i < 4; ++i) {
            if (levels[i] >= 1) {
                segs[i] |= SEG_D;
            }
            if (levels[i] >= 2) {
                segs[i] |= SEG_G;
            }
            if (levels[i] >= 3) {
                segs[i] |= SEG_A;
            }
        }
    }

    display_ui_show_segments(segs, false, hold_ms);
}
