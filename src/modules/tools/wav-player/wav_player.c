/*
 * WAV Player Tool DSP Plugin
 *
 * Lightweight headless WAV file player for audio preview.
 * Controlled via set_param/get_param from shadow UI tools.
 * Uses mmap for zero-copy file access.
 * Supports PCM int16 and IEEE float32 WAV files.
 *
 * V2 API - Instance-based
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "host/plugin_api_v1.h"

/* WAV audio format codes */
#define WAV_FORMAT_PCM   1
#define WAV_FORMAT_FLOAT 3

/* ------------------------------------------------------------------ */
/*  Instance state                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;                     /* open file descriptor (-1 if none) */
    void *map;                  /* mmap'd file data */
    size_t map_size;            /* mmap size */
    void *data;                 /* pointer to audio data start (after header) */
    uint32_t total_frames;      /* total frames in file */
    uint32_t play_pos;          /* current playback position (frames) */
    int num_channels;           /* 1 or 2 */
    int audio_format;           /* WAV_FORMAT_PCM or WAV_FORMAT_FLOAT */
    int bits_per_sample;        /* 16 or 32 */
    int playing;                /* 1 = playing, 0 = stopped */
    int loop;                   /* 1 = loop, 0 = one-shot */
    float gain;                 /* output gain (default 0.5) */
} wav_player_t;

static const host_api_v1_t *g_host = NULL;

static void wp_log(const char *msg) {
    if (g_host && g_host->log) g_host->log(msg);
}

/* ------------------------------------------------------------------ */
/*  WAV file handling                                                  */
/* ------------------------------------------------------------------ */

static void close_file(wav_player_t *wp) {
    if (wp->map && wp->map != MAP_FAILED) {
        munmap(wp->map, wp->map_size);
    }
    if (wp->fd >= 0) {
        close(wp->fd);
    }
    wp->fd = -1;
    wp->map = NULL;
    wp->map_size = 0;
    wp->data = NULL;
    wp->total_frames = 0;
    wp->play_pos = 0;
    wp->num_channels = 0;
    wp->audio_format = 0;
    wp->bits_per_sample = 0;
    wp->playing = 0;
}

static int open_wav(wav_player_t *wp, const char *path) {
    close_file(wp);

    wp->fd = open(path, O_RDONLY);
    if (wp->fd < 0) {
        wp_log("wav_player: failed to open file");
        return -1;
    }

    struct stat st;
    if (fstat(wp->fd, &st) < 0 || st.st_size < 44) {
        wp_log("wav_player: file too small for WAV header");
        close_file(wp);
        return -1;
    }

    wp->map_size = (size_t)st.st_size;
    wp->map = mmap(NULL, wp->map_size, PROT_READ, MAP_PRIVATE, wp->fd, 0);
    if (wp->map == MAP_FAILED) {
        wp_log("wav_player: mmap failed");
        wp->map = NULL;
        close_file(wp);
        return -1;
    }

    /* Parse WAV header - validate RIFF/WAVE */
    const uint8_t *raw = (const uint8_t *)wp->map;

    if (memcmp(raw, "RIFF", 4) != 0 || memcmp(raw + 8, "WAVE", 4) != 0) {
        wp_log("wav_player: not a RIFF/WAVE file");
        close_file(wp);
        return -1;
    }

    /* Walk chunks to find fmt and data */
    uint32_t offset = 12;
    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    int found_fmt = 0;
    int found_data = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;

    while (offset + 8 <= wp->map_size) {
        const uint8_t *chunk = raw + offset;
        uint32_t chunk_size = chunk[4] | (chunk[5] << 8) | (chunk[6] << 16) | (chunk[7] << 24);

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            audio_format    = chunk[8]  | (chunk[9]  << 8);
            num_channels    = chunk[10] | (chunk[11] << 8);
            sample_rate     = chunk[12] | (chunk[13] << 8) | (chunk[14] << 16) | (chunk[15] << 24);
            bits_per_sample = chunk[22] | (chunk[23] << 8);
            found_fmt = 1;
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_offset = offset + 8;
            data_size = chunk_size;
            found_data = 1;
            break; /* data chunk found, stop scanning */
        }

        offset += 8 + chunk_size;
        /* Chunks are word-aligned */
        if (chunk_size & 1) offset++;
    }

    if (!found_fmt || !found_data) {
        wp_log("wav_player: missing fmt or data chunk");
        close_file(wp);
        return -1;
    }

    /* Validate format: PCM int16 or IEEE float32 */
    int bytes_per_sample = 0;
    if (audio_format == WAV_FORMAT_PCM && bits_per_sample == 16) {
        bytes_per_sample = 2;
    } else if (audio_format == WAV_FORMAT_FLOAT && bits_per_sample == 32) {
        bytes_per_sample = 4;
    } else {
        char logbuf[128];
        snprintf(logbuf, sizeof(logbuf),
                 "wav_player: unsupported format %u / %u-bit",
                 audio_format, bits_per_sample);
        wp_log(logbuf);
        close_file(wp);
        return -1;
    }

    if (num_channels < 1 || num_channels > 2) {
        wp_log("wav_player: unsupported channel count");
        close_file(wp);
        return -1;
    }

    if (data_offset + data_size > wp->map_size) {
        /* Clamp data_size to what's actually available */
        data_size = (uint32_t)(wp->map_size - data_offset);
    }

    wp->audio_format = audio_format;
    wp->bits_per_sample = bits_per_sample;
    wp->num_channels = num_channels;
    wp->data = (void *)(raw + data_offset);
    wp->total_frames = data_size / (num_channels * bytes_per_sample);
    wp->play_pos = 0;

    char logbuf[128];
    snprintf(logbuf, sizeof(logbuf),
             "wav_player: loaded %u frames, %d ch, %u Hz, fmt=%u/%u-bit",
             wp->total_frames, num_channels, sample_rate,
             audio_format, bits_per_sample);
    wp_log(logbuf);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  V2 API implementation                                              */
/* ------------------------------------------------------------------ */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    (void)json_defaults;

    wav_player_t *wp = calloc(1, sizeof(wav_player_t));
    if (!wp) return NULL;

    wp->fd = -1;
    wp->gain = 0.5f;
    wp->loop = 0;
    wp->playing = 0;

    wp_log("wav_player: instance created");
    return wp;
}

static void v2_destroy_instance(void *instance) {
    wav_player_t *wp = (wav_player_t *)instance;
    if (!wp) return;
    close_file(wp);
    free(wp);
    wp_log("wav_player: instance destroyed");
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)instance; (void)msg; (void)len; (void)source;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    wav_player_t *wp = (wav_player_t *)instance;
    if (!wp || !key || !val) return;

    if (strcmp(key, "file_path") == 0) {
        if (open_wav(wp, val) == 0) {
            wp->playing = 1; /* auto-start on load */
        }
    } else if (strcmp(key, "playing") == 0) {
        int v = atoi(val);
        wp->playing = v ? 1 : 0;
        if (!v) wp->play_pos = 0; /* stop resets position */
    } else if (strcmp(key, "loop") == 0) {
        wp->loop = atoi(val) ? 1 : 0;
    } else if (strcmp(key, "gain") == 0) {
        float g = (float)atof(val);
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
        wp->gain = g;
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    wav_player_t *wp = (wav_player_t *)instance;
    if (!wp || !key || !buf || buf_len < 2) return -1;

    if (strcmp(key, "playing") == 0) {
        return snprintf(buf, buf_len, "%d", wp->playing);
    } else if (strcmp(key, "play_pos") == 0) {
        return snprintf(buf, buf_len, "%u", wp->play_pos);
    } else if (strcmp(key, "total_frames") == 0) {
        return snprintf(buf, buf_len, "%u", wp->total_frames);
    } else if (strcmp(key, "is_loaded") == 0) {
        return snprintf(buf, buf_len, "%d", wp->data ? 1 : 0);
    }

    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    (void)instance; (void)buf; (void)buf_len;
    return 0;
}

static void v2_render_block(void *instance, int16_t *out_lr, int frames) {
    wav_player_t *wp = (wav_player_t *)instance;

    if (!wp || !wp->playing || !wp->data || wp->total_frames == 0) {
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    const float gain = wp->gain;
    const int nch = wp->num_channels;
    const int is_float = (wp->audio_format == WAV_FORMAT_FLOAT);

    for (int i = 0; i < frames; i++) {
        if (wp->play_pos >= wp->total_frames) {
            if (wp->loop) {
                wp->play_pos = 0;
            } else {
                wp->playing = 0;
                /* Fill remainder with silence */
                memset(&out_lr[i * 2], 0, (frames - i) * 2 * sizeof(int16_t));
                return;
            }
        }

        float fL, fR;
        if (is_float) {
            const float *fdata = (const float *)wp->data;
            if (nch == 1) {
                fL = fR = fdata[wp->play_pos];
            } else {
                fL = fdata[wp->play_pos * 2];
                fR = fdata[wp->play_pos * 2 + 1];
            }
        } else {
            /* PCM int16 */
            const int16_t *sdata = (const int16_t *)wp->data;
            if (nch == 1) {
                fL = fR = sdata[wp->play_pos] / 32768.0f;
            } else {
                fL = sdata[wp->play_pos * 2]     / 32768.0f;
                fR = sdata[wp->play_pos * 2 + 1] / 32768.0f;
            }
        }

        /* Apply gain and convert to int16 */
        int32_t L = (int32_t)(fL * gain * 32767.0f);
        int32_t R = (int32_t)(fR * gain * 32767.0f);

        /* Clamp to int16 range */
        if (L > 32767) L = 32767; else if (L < -32768) L = -32768;
        if (R > 32767) R = 32767; else if (R < -32768) R = -32768;

        out_lr[i * 2]     = (int16_t)L;
        out_lr[i * 2 + 1] = (int16_t)R;

        wp->play_pos++;
    }
}

/* ------------------------------------------------------------------ */
/*  Plugin API v2 table                                                */
/* ------------------------------------------------------------------ */

static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi = v2_on_midi,
    .set_param = v2_set_param,
    .get_param = v2_get_param,
    .get_error = v2_get_error,
    .render_block = v2_render_block
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    wp_log("wav_player: plugin initialized (v2)");
    return &g_plugin_api_v2;
}
