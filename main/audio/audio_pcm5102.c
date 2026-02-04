#include "audio_pcm5102.h"

#include "board_pins.h"
#include "audio_owner.h"
#include "audio_eq.h"
#include "audio_tones.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_AMPLITUDE 16000
#define AUDIO_QUEUE_DEPTH 8
#define AUDIO_CHUNK_FRAMES 256
#define AUDIO_I2S_TIMEOUT_MS 5000
#define AUDIO_EQ_CHUNK_FRAMES 256
#define AUDIO_KS_MAX_DELAY 512

static const char *TAG = "audio_pcm5102";

typedef enum {
    AUDIO_WAVE_SILENCE = 0,
    AUDIO_WAVE_SQUARE,
    AUDIO_WAVE_KARPLUS
} audio_wave_t;

typedef struct {
    uint16_t freq_hz;
    uint32_t duration_ms;
    uint8_t volume;
    audio_wave_t wave;
    uint16_t damping_q15;
} audio_cmd_t;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_audio_task = NULL;
static volatile bool s_audio_ready = false;
static volatile bool s_stop_requested = false;
static uint8_t s_volume = 200;
static i2s_chan_handle_t s_tx_chan = NULL;
static volatile bool s_i2s_enabled = false;
static SemaphoreHandle_t s_i2s_mutex = NULL;
static int16_t s_eq_buf[AUDIO_EQ_CHUNK_FRAMES * 2];
static int16_t s_ks_buf[AUDIO_KS_MAX_DELAY];

static void audio_write_silence(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        return;
    }

    int16_t frame[AUDIO_CHUNK_FRAMES * 2] = {0};
    size_t bytes_written = 0;
    uint32_t total_frames = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;

    for (uint32_t offset = 0; offset < total_frames; ) {
        if (s_stop_requested) {
            break;
        }
        uint32_t frames = total_frames - offset;
        if (frames > AUDIO_CHUNK_FRAMES) {
            frames = AUDIO_CHUNK_FRAMES;
        }
        audio_i2s_write(frame, frames * sizeof(int16_t) * 2, &bytes_written, AUDIO_I2S_TIMEOUT_MS);
        offset += frames;
    }
}

static void audio_write_tone(uint16_t freq_hz, uint32_t duration_ms, uint8_t volume)
{
    if (freq_hz == 0 || duration_ms == 0) {
        audio_write_silence(duration_ms);
        return;
    }

    uint32_t samples_per_cycle = AUDIO_SAMPLE_RATE / freq_hz;
    if (samples_per_cycle < 2) {
        return;
    }

    int16_t amplitude = (int16_t)((AUDIO_AMPLITUDE * volume) / 255U);
    uint32_t half_cycle = samples_per_cycle / 2;
    uint32_t total_frames = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    int16_t frame[AUDIO_CHUNK_FRAMES * 2];
    size_t bytes_written = 0;

    for (uint32_t offset = 0; offset < total_frames; ) {
        if (s_stop_requested) {
            break;
        }
        uint32_t frames = total_frames - offset;
        if (frames > AUDIO_CHUNK_FRAMES) {
            frames = AUDIO_CHUNK_FRAMES;
        }

        for (uint32_t i = 0; i < frames; ++i) {
            uint32_t pos = (offset + i) % samples_per_cycle;
            int16_t sample = (pos < half_cycle) ? amplitude : (int16_t)-amplitude;
            frame[i * 2] = sample;
            frame[i * 2 + 1] = sample;
        }

        audio_i2s_write(frame, frames * sizeof(int16_t) * 2, &bytes_written, AUDIO_I2S_TIMEOUT_MS);
        offset += frames;
    }
}

static void audio_write_karplus(uint16_t freq_hz, uint32_t duration_ms, uint8_t volume, uint16_t damping_q15)
{
    if (freq_hz == 0 || duration_ms == 0) {
        audio_write_silence(duration_ms);
        return;
    }

    uint32_t delay = AUDIO_SAMPLE_RATE / freq_hz;
    if (delay < 2 || delay > AUDIO_KS_MAX_DELAY) {
        audio_write_tone(freq_hz, duration_ms, volume);
        return;
    }

    if (damping_q15 < 30000U || damping_q15 > 32760U) {
        damping_q15 = 32560U;
    }

    int32_t amp = ((int32_t)AUDIO_AMPLITUDE * (int32_t)volume) / 255;
    if (amp > 12000) {
        amp = 12000;
    }
    if (amp < 600) {
        amp = 600;
    }

    for (uint32_t i = 0; i < delay; ++i) {
        int32_t rnd = (int32_t)(esp_random() & 0xFFFFU) - 32768;
        s_ks_buf[i] = (int16_t)((rnd * amp) / 32768);
    }

    uint32_t total_frames = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    if (total_frames == 0) {
        total_frames = 1;
    }
    uint32_t attack_frames = (AUDIO_SAMPLE_RATE * 2U) / 1000U;
    uint32_t release_frames = (AUDIO_SAMPLE_RATE * 18U) / 1000U;
    if (release_frames > (total_frames / 2U)) {
        release_frames = total_frames / 2U;
    }

    int16_t frame[AUDIO_CHUNK_FRAMES * 2];
    size_t bytes_written = 0;
    uint32_t idx = 0;

    for (uint32_t offset = 0; offset < total_frames; ) {
        if (s_stop_requested) {
            break;
        }
        uint32_t frames = total_frames - offset;
        if (frames > AUDIO_CHUNK_FRAMES) {
            frames = AUDIO_CHUNK_FRAMES;
        }

        for (uint32_t i = 0; i < frames; ++i) {
            uint32_t pos = offset + i;
            uint32_t next_idx = idx + 1U;
            if (next_idx >= delay) {
                next_idx = 0;
            }

            int16_t out = s_ks_buf[idx];
            int32_t next = ((int32_t)s_ks_buf[idx] + (int32_t)s_ks_buf[next_idx]) / 2;
            next = (next * (int32_t)damping_q15) >> 15;
            if (next > 32767) {
                next = 32767;
            } else if (next < -32768) {
                next = -32768;
            }
            s_ks_buf[idx] = (int16_t)next;
            idx = next_idx;

            int32_t sample = out;
            if (attack_frames > 0 && pos < attack_frames) {
                sample = (sample * (int32_t)pos) / (int32_t)attack_frames;
            } else if (release_frames > 0 && pos >= (total_frames - release_frames)) {
                uint32_t rel = total_frames - pos;
                sample = (sample * (int32_t)rel) / (int32_t)release_frames;
            }

            frame[i * 2] = (int16_t)sample;
            frame[i * 2 + 1] = (int16_t)sample;
        }

        audio_i2s_write(frame, frames * sizeof(int16_t) * 2, &bytes_written, AUDIO_I2S_TIMEOUT_MS);
        offset += frames;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    audio_cmd_t cmd;

    while (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY)) {
        s_stop_requested = false;
        if (!s_audio_ready) {
            continue;
        }
        audio_owner_t owner = audio_owner_get();
        if (owner != AUDIO_OWNER_TONE && owner != AUDIO_OWNER_ALARM) {
            continue;
        }

        if (cmd.wave == AUDIO_WAVE_SILENCE || cmd.volume == 0) {
            audio_write_silence(cmd.duration_ms);
        } else if (cmd.wave == AUDIO_WAVE_KARPLUS) {
            audio_write_karplus(cmd.freq_hz, cmd.duration_ms, cmd.volume, cmd.damping_q15);
        } else {
            audio_write_tone(cmd.freq_hz, cmd.duration_ms, cmd.volume);
        }

        if (uxQueueMessagesWaiting(s_cmd_queue) == 0) {
            audio_write_silence(30);
            audio_i2s_reset();
            if (owner == AUDIO_OWNER_ALARM) {
                audio_owner_release(AUDIO_OWNER_ALARM);
            } else {
                audio_owner_release(AUDIO_OWNER_TONE);
            }
        }
    }
}

esp_err_t audio_init(void)
{
    if (s_tx_chan) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 384;
    chan_cfg.auto_clear_after_cb = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s channel create failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init std failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return err;
    }

    if (!s_i2s_mutex) {
        s_i2s_mutex = xSemaphoreCreateMutex();
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return err;
    }
    s_i2s_enabled = true;

    audio_eq_init(AUDIO_SAMPLE_RATE);

    s_cmd_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(audio_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(audio_task, "audio_task", 4096, NULL, 8, &s_audio_task) != pdPASS) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        s_i2s_enabled = false;
        if (s_i2s_mutex) {
            vSemaphoreDelete(s_i2s_mutex);
            s_i2s_mutex = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    s_audio_ready = true;
    ESP_LOGI(TAG, "audio ready");
    return ESP_OK;
}

void audio_set_volume(uint8_t volume)
{
    s_volume = volume;
}

uint8_t audio_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_i2s_set_sample_rate(uint32_t sample_rate)
{
    if (!s_tx_chan || sample_rate == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    if (s_i2s_mutex) {
        xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    }
    esp_err_t err = i2s_channel_disable(s_tx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (s_i2s_mutex) {
            xSemaphoreGive(s_i2s_mutex);
        }
        return err;
    }
    s_i2s_enabled = false;
    err = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
    if (err != ESP_OK) {
        if (s_i2s_mutex) {
            xSemaphoreGive(s_i2s_mutex);
        }
        return err;
    }
    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (s_i2s_mutex) {
            xSemaphoreGive(s_i2s_mutex);
        }
        return err;
    }
    s_i2s_enabled = (err == ESP_OK);
    if (s_i2s_mutex) {
        xSemaphoreGive(s_i2s_mutex);
    }
    audio_eq_set_sample_rate(sample_rate);
    return ESP_OK;
}

esp_err_t audio_i2s_write(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (!s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_i2s_mutex) {
        xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    }
    if (!s_i2s_enabled) {
        esp_err_t enable_err = i2s_channel_enable(s_tx_chan);
        if (enable_err != ESP_OK && enable_err != ESP_ERR_INVALID_STATE) {
            if (s_i2s_mutex) {
                xSemaphoreGive(s_i2s_mutex);
            }
            return enable_err;
        }
        s_i2s_enabled = true;
    }
    if (!data || len == 0) {
        if (bytes_written) {
            *bytes_written = 0;
        }
        if (s_i2s_mutex) {
            xSemaphoreGive(s_i2s_mutex);
        }
        return ESP_OK;
    }

    if (audio_eq_is_flat()) {
        esp_err_t err = i2s_channel_write(s_tx_chan, data, len, bytes_written, timeout_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write err=%s", esp_err_to_name(err));
        }
        if (s_i2s_mutex) {
            xSemaphoreGive(s_i2s_mutex);
        }
        return err;
    }

    const uint8_t *src = (const uint8_t *)data;
    size_t total_written = 0;
    esp_err_t err = ESP_OK;

    while (len > 0) {
        size_t frames = len / (sizeof(int16_t) * 2);
        if (frames == 0) {
            size_t bw = 0;
        err = i2s_channel_write(s_tx_chan, src, len, &bw, timeout_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write err=%s", esp_err_to_name(err));
        }
        total_written += bw;
        break;
        }

        if (frames > AUDIO_EQ_CHUNK_FRAMES) {
            frames = AUDIO_EQ_CHUNK_FRAMES;
        }
        size_t chunk_bytes = frames * sizeof(int16_t) * 2;
        memcpy(s_eq_buf, src, chunk_bytes);
        audio_eq_process(s_eq_buf, frames, 2);

        size_t bw = 0;
        err = i2s_channel_write(s_tx_chan, s_eq_buf, chunk_bytes, &bw, timeout_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write err=%s", esp_err_to_name(err));
        }
        total_written += bw;
        if (err != ESP_OK || bw == 0) {
            break;
        }
        if (bw < chunk_bytes) {
            break;
        }
        src += chunk_bytes;
        len -= chunk_bytes;
    }

    if (bytes_written) {
        *bytes_written = total_written;
    }
    if (s_i2s_mutex) {
        xSemaphoreGive(s_i2s_mutex);
    }
    return err;
}

esp_err_t audio_i2s_reset(void)
{
    if (!s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_i2s_mutex) {
        xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    }
    esp_err_t err = i2s_channel_disable(s_tx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (s_i2s_mutex) {
            xSemaphoreGive(s_i2s_mutex);
        }
        return err;
    }
    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (s_i2s_mutex) {
            xSemaphoreGive(s_i2s_mutex);
        }
        return err;
    }
    s_i2s_enabled = (err == ESP_OK);
    if (s_i2s_mutex) {
        xSemaphoreGive(s_i2s_mutex);
    }
    return ESP_OK;
}

void audio_i2s_write_silence(uint32_t duration_ms)
{
    if (!s_tx_chan || duration_ms == 0) {
        return;
    }
    bool prev_stop = s_stop_requested;
    s_stop_requested = false;
    audio_write_silence(duration_ms);
    s_stop_requested = prev_stop;
}

static void audio_play_tone_volume(uint16_t freq_hz, uint32_t duration_ms, uint8_t volume)
{
    if (!s_audio_ready || !s_cmd_queue) {
        return;
    }
    if (!audio_owner_acquire(AUDIO_OWNER_TONE, false)) {
        return;
    }
    audio_cmd_t cmd = {
        .freq_hz = freq_hz,
        .duration_ms = duration_ms,
        .volume = volume,
        .wave = AUDIO_WAVE_SQUARE,
        .damping_q15 = 0
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
}

static void audio_queue_tone(uint16_t freq_hz, uint32_t duration_ms, uint8_t volume)
{
    if (!s_audio_ready || !s_cmd_queue) {
        return;
    }
    audio_cmd_t cmd = {
        .freq_hz = freq_hz,
        .duration_ms = duration_ms,
        .volume = volume,
        .wave = AUDIO_WAVE_SQUARE,
        .damping_q15 = 0
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
}

static void audio_queue_pluck(uint16_t freq_hz, uint32_t duration_ms, uint8_t volume, uint16_t damping_q15)
{
    if (!s_audio_ready || !s_cmd_queue) {
        return;
    }
    audio_cmd_t cmd = {
        .freq_hz = freq_hz,
        .duration_ms = duration_ms,
        .volume = volume,
        .wave = AUDIO_WAVE_KARPLUS,
        .damping_q15 = damping_q15
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
}

static void audio_queue_silence(uint32_t duration_ms)
{
    if (!s_audio_ready || !s_cmd_queue) {
        return;
    }
    audio_cmd_t cmd = {
        .freq_hz = 0,
        .duration_ms = duration_ms,
        .volume = 0,
        .wave = AUDIO_WAVE_SILENCE,
        .damping_q15 = 0
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void audio_play_tone(uint16_t freq_hz, uint32_t duration_ms)
{
    audio_play_tone_volume(freq_hz, duration_ms, s_volume);
}

void audio_play_alarm(void)
{
    audio_play_alarm_tone(1);
}

void audio_play_tone_sequence(const audio_tone_step_t *seq, size_t count, uint8_t volume)
{
    if (!seq || count == 0) {
        return;
    }
    s_stop_requested = false;
    for (size_t i = 0; i < count; ++i) {
        audio_queue_tone(seq[i].freq_hz, seq[i].duration_ms, volume);
    }
    audio_queue_silence(30);
}

void audio_play_pluck_sequence(const audio_pluck_step_t *seq, size_t count, uint8_t volume)
{
    if (!seq || count == 0) {
        return;
    }
    s_stop_requested = false;
    for (size_t i = 0; i < count; ++i) {
        audio_queue_pluck(seq[i].freq_hz, seq[i].duration_ms, volume, seq[i].damping_q15);
    }
    audio_queue_silence(30);
}

void audio_play_tone_sequence_blocking(const audio_tone_step_t *seq, size_t count, uint8_t volume)
{
    if (!seq || count == 0 || !s_audio_ready) {
        return;
    }
    s_stop_requested = false;
    for (size_t i = 0; i < count; ++i) {
        if (s_stop_requested) {
            break;
        }
        if (seq[i].freq_hz == 0) {
            audio_write_silence(seq[i].duration_ms);
        } else {
            audio_write_tone(seq[i].freq_hz, seq[i].duration_ms, volume);
        }
    }
}

void audio_play_alarm_tone_volume(uint8_t tone, uint8_t volume);

void audio_play_alarm_tone(uint8_t tone)
{
    audio_play_alarm_tone_volume(tone, s_volume);
}

void audio_play_alarm_tone_volume(uint8_t tone, uint8_t volume)
{
    if (!s_audio_ready || !s_cmd_queue) {
        return;
    }
    if (!audio_owner_acquire(AUDIO_OWNER_ALARM, true)) {
        return;
    }
    s_stop_requested = true;
    xQueueReset(s_cmd_queue);
    (void)tone;
    audio_tones_play_alarm(volume);
}

void audio_play_system_tone(uint8_t tone)
{
    if (!s_audio_ready || !s_cmd_queue) {
        return;
    }
    if (!audio_owner_acquire(AUDIO_OWNER_TONE, true)) {
        return;
    }
    audio_tones_play_system(tone, s_volume);
}

void audio_stop(void)
{
    if (!s_audio_ready) {
        return;
    }
    s_stop_requested = true;
    xQueueReset(s_cmd_queue);
    audio_queue_silence(30);
    audio_owner_release(AUDIO_OWNER_TONE);
}

