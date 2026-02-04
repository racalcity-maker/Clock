#include "helix_mp3_wrapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mp3dec.h"

static const char *TAG = "helix_mp3";

static size_t convert_and_write(const short *pcm, int samples, int nChans, int sampleRate, int volume_percent, mp3_write_cb_t writer, void *user)
{
    const int target_rate = 44100;
    (void)volume_percent;
    if (sampleRate == target_rate && nChans == 2) {
        return writer((const uint8_t *)pcm, (size_t)samples * sizeof(short), user);
    }
    float ratio = (float)sampleRate / (float)target_rate;
    int out_frames = samples / nChans;
    if (ratio != 1.0f) {
        out_frames = (int)((float)out_frames / ratio);
    }
    static short outbuf[1152 * 2]; // max mp3 frame stereo
    if (out_frames > 1152) {
        out_frames = 1152;
    }
    if (sampleRate == target_rate && nChans == 1) {
        for (int o = 0; o < out_frames; ++o) {
            int16_t s = pcm[o];
            outbuf[o * 2] = s;
            outbuf[o * 2 + 1] = s;
        }
    } else {
        for (int o = 0; o < out_frames; ++o) {
            int src_idx = ratio == 1.0f ? o : (int)(o * ratio);
            int32_t l = (nChans == 1) ? pcm[src_idx] : pcm[src_idx * 2];
            int32_t r = (nChans == 1) ? pcm[src_idx] : pcm[src_idx * 2 + 1];
            if (l > 32767) {
                l = 32767;
            } else if (l < -32768) {
                l = -32768;
            }
            if (r > 32767) {
                r = 32767;
            } else if (r < -32768) {
                r = -32768;
            }
            outbuf[o * 2] = (short)l;
            outbuf[o * 2 + 1] = (short)r;
        }
    }
    size_t bytes = out_frames * 2 * sizeof(short);
    return writer((const uint8_t *)outbuf, bytes, user);
}

bool helix_mp3_decode_file(const char *path,
                           int volume_percent,
                           mp3_write_cb_t writer,
                           void *user,
                           mp3_progress_cb_t progress_cb,
                           void *progress_user,
                           float start_ratio)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return false;
    }

    long total_bytes = 0;
    fseek(f, 0, SEEK_END);
    total_bytes = ftell(f);
    fseek(f, 0, SEEK_SET);

    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        fclose(f);
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        return false;
    }

    const int INBUF_SIZE = 4096;
    unsigned char *inbuf = malloc(INBUF_SIZE + MAINBUF_SIZE);
    if (!inbuf) {
        MP3FreeDecoder(dec);
        fclose(f);
        return false;
    }

    short pcm[1152 * 2];
    int bytesLeft = 0;
    unsigned char *readPtr = inbuf;
    bool ok = true;

    // Skip ID3v2 tag if present.
    unsigned char id3[10] = {0};
    size_t id3read = fread(id3, 1, sizeof(id3), f);
    if (id3read == sizeof(id3) && id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3') {
        int tagSize = (id3[6] << 21) | (id3[7] << 14) | (id3[8] << 7) | id3[9];
        fseek(f, 10 + tagSize, SEEK_SET);
    } else {
        fseek(f, 0, SEEK_SET);
    }

    size_t bytes_read_total = (size_t)ftell(f);
    uint64_t samples_total = 0;
    int last_rate = 44100;
    uint32_t est_total_ms = 0;
    bool total_ms_set = false;
    float ratio = start_ratio;
    if (ratio < 0.0f) {
        ratio = 0.0f;
    }
    if (ratio > 1.0f) {
        ratio = 1.0f;
    }
    if (ratio > 0.0f) {
        long seek_pos = (long)((double)total_bytes * ratio);
        if (seek_pos < 0) {
            seek_pos = 0;
        }
        if (seek_pos > total_bytes) {
            seek_pos = total_bytes;
        }
        fseek(f, seek_pos, SEEK_SET);
        bytes_read_total = (size_t)seek_pos;
    }

    uint32_t seek_base_ms = 0;
    bool seek_base_set = false;
    bool use_seek = ratio > 0.0f;

    int err_logged = 0;
    while (1) {
        if (bytesLeft < MAINBUF_SIZE) {
            memmove(inbuf, readPtr, bytesLeft);
            int n = fread(inbuf + bytesLeft, 1, INBUF_SIZE, f);
            bytesLeft += n;
            readPtr = inbuf;
            if (bytesLeft == 0) {
                break;
            }
        }

        int offset = MP3FindSyncWord(readPtr, bytesLeft);
        if (offset < 0) {
            bytesLeft = 0;
            continue;
        }
        readPtr += offset;
        bytesLeft -= offset;

        int err = MP3Decode(dec, &readPtr, &bytesLeft, pcm, 0);
        if (err) {
            if (err == ERR_MP3_INDATA_UNDERFLOW) {
                bytesLeft = 0;
                continue;
            }
            if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
                continue;
            }
            if (err_logged < 5) {
                ESP_LOGW(TAG, "MP3Decode err=%d bytesLeft=%d", err, bytesLeft);
                err_logged++;
            }
            if (bytesLeft > 0) {
                readPtr++;
                bytesLeft--;
            }
            continue;
        }

        MP3FrameInfo info;
        MP3GetLastFrameInfo(dec, &info);
        int samples = info.outputSamps;
        int chans = info.nChans;
        int rate = info.samprate;
        if (samples <= 0 || chans <= 0) {
            continue;
        }

        last_rate = rate;
        uint64_t frame_samples = (uint64_t)(samples / chans);
        samples_total += frame_samples;
        if (!total_ms_set && total_bytes > 0 && info.bitrate > 0) {
            est_total_ms = (uint32_t)(((uint64_t)total_bytes * 8ULL * 1000ULL) / (uint32_t)info.bitrate);
            total_ms_set = true;
            if (use_seek) {
                seek_base_ms = (uint32_t)((float)est_total_ms * ratio);
                seek_base_set = true;
            }
        }
        if (convert_and_write(pcm, samples, chans, rate, volume_percent, writer, user) == 0) {
            ok = false;
            break;
        }
        if (progress_cb) {
            bytes_read_total = (size_t)ftell(f);
            float elapsed_sec = (float)samples_total / (float)last_rate;
            uint32_t elapsed_ms = (uint32_t)(elapsed_sec * 1000.0f);
            if (use_seek && seek_base_set) {
                elapsed_ms += seek_base_ms;
            }
            progress_cb(bytes_read_total, (size_t)total_bytes, elapsed_ms, est_total_ms, progress_user);
        }
        if (bytesLeft < MAINBUF_SIZE) {
            memmove(inbuf, readPtr, bytesLeft);
            int n = fread(inbuf + bytesLeft, 1, INBUF_SIZE, f);
            bytesLeft += n;
            readPtr = inbuf;
            bytes_read_total = (size_t)ftell(f);
            if (progress_cb && total_bytes > 0) {
                progress_cb(bytes_read_total, (size_t)total_bytes, 0, est_total_ms, progress_user);
            }
        }
    }

    free(inbuf);
    MP3FreeDecoder(dec);
    fclose(f);
    return ok;
}
