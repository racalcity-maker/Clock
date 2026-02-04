#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef size_t (*mp3_write_cb_t)(const uint8_t *data, size_t len, void *user);

typedef void (*mp3_progress_cb_t)(size_t bytes_read, size_t total_bytes, uint32_t elapsed_ms, uint32_t est_total_ms, void *user);

// Decode MP3 to PCM 44.1k/16bit/stereo, write via callback.
bool helix_mp3_decode_file(const char *path,
                           int volume_percent,
                           mp3_write_cb_t writer,
                           void *user,
                           mp3_progress_cb_t progress_cb,
                           void *progress_user,
                           float start_ratio);
