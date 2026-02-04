#include "audio_spectrum.h"

#include <math.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FHT_SIZE 512
#define FHT_HALF (FHT_SIZE / 2)
#define FHT_MIN_INTERVAL_US 20000
#define FHT_BUF_COUNT 2

#define SPECTRUM_LOG_K 2.0e-6f
#define SPECTRUM_AGC_DECAY 0.990f
#define SPECTRUM_AGC_HEADROOM 1.45f
#define SPECTRUM_AGC_MIN 1.0e-6f

#define LEVEL_T0 0.20f
#define LEVEL_T1 0.45f
#define LEVEL_T2 0.70f

static const float kPi = 3.1415926535f;

static uint32_t s_sample_rate = 44100;
static int s_band_start[4] = {1, 1, 1, 1};
static int s_band_end[4] = {1, 1, 1, 1};
static float s_band_weight[4][FHT_HALF + 1] = {{0}};
static float s_band_weight_sum[4] = {0};

static float s_window[FHT_SIZE];
static float s_twiddle_re[FHT_HALF];
static float s_twiddle_im[FHT_HALF];
static uint16_t s_bitrev[FHT_SIZE];
static bool s_tables_ready = false;
static bool s_enabled = false;

static int16_t s_pcm_buf[FHT_BUF_COUNT][FHT_SIZE];
static volatile uint8_t s_buf_ready[FHT_BUF_COUNT] = {0, 0};
static uint8_t s_active_buf = 0;
static size_t s_pcm_idx = 0;

static float s_work_re[FHT_SIZE];
static float s_work_im[FHT_SIZE];
static float s_fht[FHT_SIZE];

static float s_band_max[4] = {SPECTRUM_AGC_MIN, SPECTRUM_AGC_MIN, SPECTRUM_AGC_MIN, SPECTRUM_AGC_MIN};
static float s_band_level[4] = {0};
static const float s_band_gain[4] = {3.6f, 1.3f, 1.1f, 1.5f};
static const float s_band_attack[4] = {0.75f, 0.55f, 0.50f, 0.65f};
static const float s_band_release[4] = {0.05f, 0.08f, 0.10f, 0.15f};

static uint32_t s_levels_packed = 0;
static int64_t s_last_update_us = 0;
static int64_t s_last_fht_us = 0;

static TaskHandle_t s_fht_task = NULL;

static inline uint32_t pack_levels(uint8_t l0, uint8_t l1, uint8_t l2, uint8_t l3)
{
    return (uint32_t)l0 | ((uint32_t)l1 << 8) | ((uint32_t)l2 << 16) | ((uint32_t)l3 << 24);
}

static void bands_recalc(void)
{
    float fs = (s_sample_rate == 0) ? 44100.0f : (float)s_sample_rate;
    float nyq = fs * 0.5f;
    const float band_start_hz[4] = {50.0f, 300.0f, 1200.0f, 3500.0f};
    const float band_end_hz[4] = {150.0f, 1000.0f, 3000.0f, 12000.0f};
    float bin_hz = fs / (float)FHT_SIZE;

    memset(s_band_weight, 0, sizeof(s_band_weight));
    int prev_end = 0;
    for (int i = 0; i < 4; ++i) {
        float f0 = band_start_hz[i];
        float f1 = band_end_hz[i];
        if (f0 > nyq) {
            f0 = nyq;
        }
        if (f1 > nyq) {
            f1 = nyq;
        }
        int b0 = (int)ceilf(f0 * (float)FHT_SIZE / fs);
        int b1 = (int)floorf(f1 * (float)FHT_SIZE / fs);
        if (b0 < 1) {
            b0 = 1;
        }
        if (b1 > FHT_HALF) {
            b1 = FHT_HALF;
        }
        if (b0 <= prev_end) {
            b0 = prev_end + 1;
        }
        if (b1 < b0) {
            b1 = b0;
        }
        s_band_start[i] = b0;
        s_band_end[i] = b1;
        prev_end = b1;

        float f0_bin = (float)b0 * bin_hz;
        float f1_bin = (float)b1 * bin_hz;
        float center = 0.5f * (f0_bin + f1_bin);
        float half_bw = 0.5f * (f1_bin - f0_bin);
        float sum_w = 0.0f;
        int count = b1 - b0 + 1;

        if (count <= 3 || half_bw <= 0.0f) {
            for (int k = b0; k <= b1; ++k) {
                s_band_weight[i][k] = 1.0f;
                sum_w += 1.0f;
            }
        } else {
            for (int k = b0; k <= b1; ++k) {
                float fk = (float)k * bin_hz;
                float w = 1.0f - (fabsf(fk - center) / half_bw);
                if (w < 0.0f) {
                    w = 0.0f;
                }
                s_band_weight[i][k] = w;
                sum_w += w;
            }
        }
        s_band_weight_sum[i] = sum_w;
    }
}

static void fht_build_tables(void)
{
    if (s_tables_ready) {
        return;
    }

    for (int i = 0; i < FHT_SIZE; ++i) {
        float phase = (2.0f * kPi * (float)i) / (float)(FHT_SIZE - 1);
        s_window[i] = 0.5f * (1.0f - cosf(phase));
    }

    for (int i = 0; i < FHT_HALF; ++i) {
        float phase = -2.0f * kPi * (float)i / (float)FHT_SIZE;
        s_twiddle_re[i] = cosf(phase);
        s_twiddle_im[i] = sinf(phase);
    }

    int bits = 0;
    for (int n = FHT_SIZE; n > 1; n >>= 1) {
        ++bits;
    }
    for (int i = 0; i < FHT_SIZE; ++i) {
        uint16_t x = (uint16_t)i;
        uint16_t r = 0;
        for (int b = 0; b < bits; ++b) {
            r = (uint16_t)((r << 1) | (x & 1U));
            x >>= 1;
        }
        s_bitrev[i] = r;
    }

    s_tables_ready = true;
}

static void fft_radix2(float *re, float *im)
{
    for (int i = 0; i < FHT_SIZE; ++i) {
        uint16_t j = s_bitrev[i];
        if (j > i) {
            float tr = re[i];
            float ti = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = tr;
            im[j] = ti;
        }
    }

    for (int len = 2; len <= FHT_SIZE; len <<= 1) {
        int half = len >> 1;
        int step = FHT_SIZE / len;
        for (int i = 0; i < FHT_SIZE; i += len) {
            for (int j = 0; j < half; ++j) {
                int tw = j * step;
                float wr = s_twiddle_re[tw];
                float wi = s_twiddle_im[tw];
                int idx1 = i + j;
                int idx2 = idx1 + half;
                float xr = re[idx2];
                float xi = im[idx2];
                float tr = wr * xr - wi * xi;
                float ti = wr * xi + wi * xr;
                re[idx2] = re[idx1] - tr;
                im[idx2] = im[idx1] - ti;
                re[idx1] += tr;
                im[idx1] += ti;
            }
        }
    }
}

static void fht_exec(void)
{
    fft_radix2(s_work_re, s_work_im);

    s_fht[0] = s_work_re[0];
    s_fht[FHT_HALF] = s_work_re[FHT_HALF];
    for (int k = 1; k < FHT_HALF; ++k) {
        float re = s_work_re[k];
        float im = s_work_im[k];
        s_fht[k] = re - im;
        s_fht[FHT_SIZE - k] = re + im;
    }
}

static float fht_bin_power(int k)
{
    if (k <= 0 || k >= FHT_HALF) {
        float h = s_fht[k];
        return h * h;
    }
    float h1 = s_fht[k];
    float h2 = s_fht[FHT_SIZE - k];
    return 0.5f * (h1 * h1 + h2 * h2);
}

static void fht_process_block(const int16_t *src)
{
    const float scale = 1.0f / 32768.0f;
    float mean = 0.0f;
    for (int i = 0; i < FHT_SIZE; ++i) {
        mean += (float)src[i] * scale;
    }
    mean /= (float)FHT_SIZE;

    for (int i = 0; i < FHT_SIZE; ++i) {
        float x = (float)src[i] * scale - mean;
        x *= s_window[i];
        s_work_re[i] = x;
        s_work_im[i] = 0.0f;
    }

    fht_exec();

    float band_power[4] = {0};
    for (int b = 0; b < 4; ++b) {
        int start = s_band_start[b];
        int end = s_band_end[b];
        float sum_w = s_band_weight_sum[b];
        if (sum_w <= 1.0e-12f) {
            band_power[b] = 0.0f;
            continue;
        }
        float sum_wp = 0.0f;
        for (int k = start; k <= end; ++k) {
            float w = s_band_weight[b][k];
            if (w <= 0.0f) {
                continue;
            }
            sum_wp += w * fht_bin_power(k);
        }
        band_power[b] = (sum_wp / sum_w) * s_band_gain[b];
    }

    uint8_t levels[4];
    for (int i = 0; i < 4; ++i) {
        float x = log10f(1.0f + (SPECTRUM_LOG_K * band_power[i]));
        float maxv = s_band_max[i];
        if (x > maxv) {
            maxv = x * SPECTRUM_AGC_HEADROOM;
        } else {
            maxv *= SPECTRUM_AGC_DECAY;
        }
        if (maxv < SPECTRUM_AGC_MIN) {
            maxv = SPECTRUM_AGC_MIN;
        }
        s_band_max[i] = maxv;

        float norm = x / maxv;
        if (norm < 0.0f) {
            norm = 0.0f;
        } else if (norm > 1.0f) {
            norm = 1.0f;
        }

        float y = s_band_level[i];
        float alpha = (norm > y) ? s_band_attack[i] : s_band_release[i];
        y += alpha * (norm - y);
        s_band_level[i] = y;

        uint8_t level = 0;
        if (y < LEVEL_T0) {
            level = 0;
        } else if (y < LEVEL_T1) {
            level = 1;
        } else if (y < LEVEL_T2) {
            level = 2;
        } else {
            level = 3;
        }
        levels[i] = level;
    }

    uint32_t packed = pack_levels(levels[0], levels[1], levels[2], levels[3]);
    __atomic_store_n(&s_levels_packed, packed, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_update_us, esp_timer_get_time(), __ATOMIC_RELEASE);
}

static void fht_task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        if (__atomic_load_n(&s_buf_ready[0], __ATOMIC_ACQUIRE) == 0 &&
            __atomic_load_n(&s_buf_ready[1], __ATOMIC_ACQUIRE) == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        int64_t now = esp_timer_get_time();
        int64_t since = now - s_last_fht_us;
        if (since < FHT_MIN_INTERVAL_US) {
            int64_t wait_us = FHT_MIN_INTERVAL_US - since;
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(wait_us / 1000)));
            continue;
        }

        int buf = (__atomic_load_n(&s_buf_ready[0], __ATOMIC_ACQUIRE) != 0) ? 0 : 1;
        if (__atomic_exchange_n(&s_buf_ready[buf], 0, __ATOMIC_ACQ_REL) == 0) {
            continue;
        }

        fht_process_block(s_pcm_buf[buf]);
        s_last_fht_us = esp_timer_get_time();
    }
}

static void fht_task_start(void)
{
    if (s_fht_task) {
        return;
    }
    if (xTaskCreate(fht_task_entry, "audio_spectrum", 4096, NULL, 2, &s_fht_task) != pdPASS) {
        s_fht_task = NULL;
    }
}

void audio_spectrum_set_sample_rate(uint32_t sample_rate)
{
    if (sample_rate == 0) {
        return;
    }
    s_sample_rate = sample_rate;
    bands_recalc();
}

static void spectrum_reset_state(void)
{
    fht_build_tables();
    bands_recalc();

    s_pcm_idx = 0;
    s_active_buf = 0;
    __atomic_store_n(&s_buf_ready[0], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_buf_ready[1], 0, __ATOMIC_RELEASE);

    memset(s_band_level, 0, sizeof(s_band_level));
    for (int i = 0; i < 4; ++i) {
        s_band_max[i] = SPECTRUM_AGC_MIN;
    }
    __atomic_store_n(&s_levels_packed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_update_us, 0, __ATOMIC_RELEASE);
    s_last_fht_us = 0;

    fht_task_start();
}

void audio_spectrum_enable(bool enable)
{
    if (enable) {
        if (s_enabled) {
            return;
        }
        s_enabled = true;
        spectrum_reset_state();
        return;
    }

    if (!s_enabled) {
        return;
    }
    s_enabled = false;
    if (s_fht_task) {
        vTaskDelete(s_fht_task);
        s_fht_task = NULL;
    }
    s_pcm_idx = 0;
    s_active_buf = 0;
    __atomic_store_n(&s_buf_ready[0], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_buf_ready[1], 0, __ATOMIC_RELEASE);
    memset(s_band_level, 0, sizeof(s_band_level));
    for (int i = 0; i < 4; ++i) {
        s_band_max[i] = SPECTRUM_AGC_MIN;
    }
    __atomic_store_n(&s_levels_packed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_update_us, 0, __ATOMIC_RELEASE);
    s_last_fht_us = 0;
}

void audio_spectrum_reset(void)
{
    if (!s_enabled) {
        memset(s_band_level, 0, sizeof(s_band_level));
        __atomic_store_n(&s_levels_packed, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_last_update_us, 0, __ATOMIC_RELEASE);
        s_last_fht_us = 0;
        return;
    }
    spectrum_reset_state();
}

void audio_spectrum_feed(const int16_t *samples, size_t sample_count, int channels)
{
    if (!samples || sample_count == 0) {
        return;
    }
    if (!s_enabled) {
        return;
    }
    if (channels <= 0) {
        channels = 2;
    }

    if (!s_tables_ready) {
        fht_build_tables();
        bands_recalc();
    }
    fht_task_start();

    size_t frames = sample_count / (size_t)channels;
    for (size_t i = 0; i < frames; ++i) {
        if (__atomic_load_n(&s_buf_ready[s_active_buf], __ATOMIC_ACQUIRE) != 0) {
            uint8_t other = (uint8_t)(s_active_buf ^ 1U);
            if (__atomic_load_n(&s_buf_ready[other], __ATOMIC_ACQUIRE) == 0) {
                s_active_buf = other;
                s_pcm_idx = 0;
            } else {
                return;
            }
        }

        int32_t mono = 0;
        if (channels == 1) {
            mono = samples[i];
        } else {
            size_t base = i * (size_t)channels;
            for (int c = 0; c < channels; ++c) {
                mono += samples[base + (size_t)c];
            }
            mono /= channels;
        }

        s_pcm_buf[s_active_buf][s_pcm_idx++] = (int16_t)mono;
        if (s_pcm_idx >= FHT_SIZE) {
            __atomic_store_n(&s_buf_ready[s_active_buf], 1, __ATOMIC_RELEASE);
            s_pcm_idx = 0;
            s_active_buf ^= 1U;
            if (s_fht_task) {
                xTaskNotifyGive(s_fht_task);
            }
        }
    }
}

void audio_spectrum_get_levels(uint8_t out_levels[4])
{
    if (!out_levels) {
        return;
    }
    if (!s_enabled) {
        memset(out_levels, 0, 4);
        return;
    }
    int64_t last = __atomic_load_n(&s_last_update_us, __ATOMIC_ACQUIRE);
    if (last == 0) {
        memset(out_levels, 0, 4);
        return;
    }
    int64_t now = esp_timer_get_time();
    if ((now - last) > 250000) {
        __atomic_store_n(&s_levels_packed, 0, __ATOMIC_RELEASE);
        memset(out_levels, 0, 4);
        return;
    }
    uint32_t packed = __atomic_load_n(&s_levels_packed, __ATOMIC_ACQUIRE);
    out_levels[0] = (uint8_t)(packed & 0xFF);
    out_levels[1] = (uint8_t)((packed >> 8) & 0xFF);
    out_levels[2] = (uint8_t)((packed >> 16) & 0xFF);
    out_levels[3] = (uint8_t)((packed >> 24) & 0xFF);
}
