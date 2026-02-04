#include "audio_eq.h"

#include <math.h>

#define AUDIO_EQ_LOW_HZ 150.0f
#define AUDIO_EQ_HIGH_HZ 5000.0f
#define AUDIO_EQ_MAX_STEP 30
#define AUDIO_EQ_CENTER_STEP 15
#define AUDIO_EQ_RANGE_DB 12.0f

typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1[2];
    float z2[2];
} biquad_t;

static uint32_t s_sample_rate = 44100;
static uint8_t s_low_step = AUDIO_EQ_CENTER_STEP;
static uint8_t s_high_step = AUDIO_EQ_CENTER_STEP;
static bool s_ready = false;
static bool s_flat = true;
static biquad_t s_low;
static biquad_t s_high;

static float step_to_db(uint8_t step)
{
    if (step > AUDIO_EQ_MAX_STEP) {
        step = AUDIO_EQ_MAX_STEP;
    }
    float delta = (float)((int)step - AUDIO_EQ_CENTER_STEP);
    return delta * (AUDIO_EQ_RANGE_DB / (float)AUDIO_EQ_MAX_STEP);
}

static void biquad_reset(biquad_t *bq)
{
    bq->z1[0] = 0.0f;
    bq->z1[1] = 0.0f;
    bq->z2[0] = 0.0f;
    bq->z2[1] = 0.0f;
}

static void biquad_set_identity(biquad_t *bq)
{
    bq->b0 = 1.0f;
    bq->b1 = 0.0f;
    bq->b2 = 0.0f;
    bq->a1 = 0.0f;
    bq->a2 = 0.0f;
    biquad_reset(bq);
}

static void biquad_design_low_shelf(biquad_t *bq, float fs, float freq, float gain_db)
{
    if (gain_db == 0.0f) {
        biquad_set_identity(bq);
        return;
    }

    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * 3.1415926535f * freq / fs;
    if (w0 > 3.1415926535f * 0.99f) {
        w0 = 3.1415926535f * 0.99f;
    }
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float sqrtA = sqrtf(A);
    float alpha = sinw0 / 2.0f * sqrtf(2.0f);

    float b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha);
    float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
    float b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha);
    float a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
    float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
    float a2 = (A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha;

    bq->b0 = b0 / a0;
    bq->b1 = b1 / a0;
    bq->b2 = b2 / a0;
    bq->a1 = a1 / a0;
    bq->a2 = a2 / a0;
    biquad_reset(bq);
}

static void biquad_design_high_shelf(biquad_t *bq, float fs, float freq, float gain_db)
{
    if (gain_db == 0.0f) {
        biquad_set_identity(bq);
        return;
    }

    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * 3.1415926535f * freq / fs;
    if (w0 > 3.1415926535f * 0.99f) {
        w0 = 3.1415926535f * 0.99f;
    }
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float sqrtA = sqrtf(A);
    float alpha = sinw0 / 2.0f * sqrtf(2.0f);

    float b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha);
    float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
    float b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha);
    float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
    float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
    float a2 = (A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha;

    bq->b0 = b0 / a0;
    bq->b1 = b1 / a0;
    bq->b2 = b2 / a0;
    bq->a1 = a1 / a0;
    bq->a2 = a2 / a0;
    biquad_reset(bq);
}

static void audio_eq_update(void)
{
    if (s_sample_rate == 0) {
        return;
    }
    float low_db = step_to_db(s_low_step);
    float high_db = step_to_db(s_high_step);
    s_flat = (low_db == 0.0f && high_db == 0.0f);

    float low_hz = AUDIO_EQ_LOW_HZ;
    float high_hz = AUDIO_EQ_HIGH_HZ;
    float nyq = 0.5f * (float)s_sample_rate;
    if (low_hz > nyq * 0.45f) {
        low_hz = nyq * 0.45f;
    }
    if (high_hz > nyq * 0.9f) {
        high_hz = nyq * 0.9f;
    }

    biquad_design_low_shelf(&s_low, (float)s_sample_rate, low_hz, low_db);
    biquad_design_high_shelf(&s_high, (float)s_sample_rate, high_hz, high_db);
}

void audio_eq_init(uint32_t sample_rate)
{
    if (sample_rate > 0) {
        s_sample_rate = sample_rate;
    }
    s_ready = true;
    audio_eq_update();
}

void audio_eq_set_sample_rate(uint32_t sample_rate)
{
    if (sample_rate == 0) {
        return;
    }
    s_sample_rate = sample_rate;
    if (s_ready) {
        audio_eq_update();
    }
}

void audio_eq_set_steps(uint8_t low_step, uint8_t high_step)
{
    if (low_step > AUDIO_EQ_MAX_STEP) {
        low_step = AUDIO_EQ_MAX_STEP;
    }
    if (high_step > AUDIO_EQ_MAX_STEP) {
        high_step = AUDIO_EQ_MAX_STEP;
    }
    s_low_step = low_step;
    s_high_step = high_step;
    if (s_ready) {
        audio_eq_update();
    }
}

bool audio_eq_is_flat(void)
{
    return s_flat;
}

static float biquad_process(biquad_t *bq, float x, int ch)
{
    float y = bq->b0 * x + bq->z1[ch];
    bq->z1[ch] = bq->b1 * x - bq->a1 * y + bq->z2[ch];
    bq->z2[ch] = bq->b2 * x - bq->a2 * y;
    return y;
}

void audio_eq_process(int16_t *samples, size_t frames, int channels)
{
    if (!samples || frames == 0 || channels <= 0) {
        return;
    }
    if (s_flat) {
        return;
    }
    if (channels < 2) {
        for (size_t i = 0; i < frames; ++i) {
            float x = (float)samples[i] * (1.0f / 32768.0f);
            x = biquad_process(&s_low, x, 0);
            x = biquad_process(&s_high, x, 0);
            int32_t y = (int32_t)lrintf(x * 32768.0f);
            if (y > 32767) {
                y = 32767;
            } else if (y < -32768) {
                y = -32768;
            }
            samples[i] = (int16_t)y;
        }
        return;
    }

    for (size_t i = 0; i < frames; ++i) {
        size_t base = i * (size_t)channels;
        for (int ch = 0; ch < 2; ++ch) {
            float x = (float)samples[base + (size_t)ch] * (1.0f / 32768.0f);
            x = biquad_process(&s_low, x, ch);
            x = biquad_process(&s_high, x, ch);
            int32_t y = (int32_t)lrintf(x * 32768.0f);
            if (y > 32767) {
                y = 32767;
            } else if (y < -32768) {
                y = -32768;
            }
            samples[base + (size_t)ch] = (int16_t)y;
        }
    }
}
