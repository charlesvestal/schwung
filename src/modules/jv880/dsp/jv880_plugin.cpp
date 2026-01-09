/*
 * JV-880 Plugin for Move Anything
 * Based on mini-jv880 emulator by giulioz (based on Nuked-SC55 by nukeykt)
 * Single-threaded approach - simpler and more reliable
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include "mcu.h"

extern "C" {
#include "../../../../src/host/plugin_api_v1.h"
}

/* The emulator instance */
static MCU *g_mcu = nullptr;

/* Plugin state */
static char g_module_dir[512];
static int g_initialized = 0;
static int g_rom_loaded = 0;

/* Background emulation thread */
static pthread_t g_emu_thread;
static volatile int g_thread_running = 0;

/* Audio ring buffer (44.1kHz stereo output) */
#define AUDIO_RING_SIZE 2048
static int16_t g_audio_ring[AUDIO_RING_SIZE * 2];
static volatile int g_ring_write = 0;
static volatile int g_ring_read = 0;
static pthread_mutex_t g_ring_mutex = PTHREAD_MUTEX_INITIALIZER;

/* MIDI queue */
#define MIDI_QUEUE_SIZE 256
#define MIDI_MSG_MAX_LEN 32
static uint8_t g_midi_queue[MIDI_QUEUE_SIZE][MIDI_MSG_MAX_LEN];
static int g_midi_queue_len[MIDI_QUEUE_SIZE];
static volatile int g_midi_write = 0;
static volatile int g_midi_read = 0;

/* Sample rates */
#define JV880_SAMPLE_RATE 66207
#define MOVE_SAMPLE_RATE 44100

/* Ring buffer helpers */
static int ring_available(void) {
    int avail = g_ring_write - g_ring_read;
    if (avail < 0) avail += AUDIO_RING_SIZE;
    return avail;
}

static int ring_free(void) {
    return AUDIO_RING_SIZE - 1 - ring_available();
}

/* Load ROM file */
static int load_rom(const char *filename, uint8_t *dest, size_t size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/roms/%s", g_module_dir, filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "JV880: Cannot open: %s\n", path);
        return 0;
    }

    size_t got = fread(dest, 1, size, f);
    fclose(f);

    if (got != size) {
        fprintf(stderr, "JV880: Size mismatch: %s (%zu vs %zu)\n", filename, got, size);
        return 0;
    }

    fprintf(stderr, "JV880: Loaded %s\n", filename);
    return 1;
}

/*
 * Emulation thread - runs MCU+PCM together as designed
 */
static void *emu_thread_func(void *arg) {
    (void)arg;
    fprintf(stderr, "JV880: Emulation thread started\n");

    float resample_acc = 0.0f;
    const float resample_ratio = (float)JV880_SAMPLE_RATE / (float)MOVE_SAMPLE_RATE;

    while (g_thread_running) {
        /* Process MIDI queue */
        while (g_midi_read != g_midi_write) {
            int idx = g_midi_read;
            g_mcu->postMidiSC55(g_midi_queue[idx], g_midi_queue_len[idx]);
            g_midi_read = (g_midi_read + 1) % MIDI_QUEUE_SIZE;
        }

        /* Check if we need more audio */
        int free_space = ring_free();
        if (free_space < 64) {
            /* Ring buffer nearly full, wait a bit */
            usleep(50);
            continue;
        }

        /* Generate some samples using the integrated update function */
        /* updateSC55 generates samples into sample_buffer */
        g_mcu->updateSC55(64);  /* Generate ~64 samples at JV880 rate */

        /* Resample and copy to ring buffer */
        int avail = g_mcu->sample_write_ptr;
        for (int i = 0; i < avail && ring_free() > 0; i += 2) {
            resample_acc += 1.0f;
            if (resample_acc >= resample_ratio) {
                resample_acc -= resample_ratio;

                pthread_mutex_lock(&g_ring_mutex);
                int wr = g_ring_write;
                g_audio_ring[wr * 2 + 0] = g_mcu->sample_buffer[i];
                g_audio_ring[wr * 2 + 1] = g_mcu->sample_buffer[i + 1];
                g_ring_write = (wr + 1) % AUDIO_RING_SIZE;
                pthread_mutex_unlock(&g_ring_mutex);
            }
        }
    }

    fprintf(stderr, "JV880: Emulation thread stopped\n");
    return NULL;
}

/* Plugin callbacks */

static int jv880_on_load(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    strncpy(g_module_dir, module_dir, sizeof(g_module_dir) - 1);
    fprintf(stderr, "JV880: Loading from %s\n", module_dir);

    /* Create emulator instance */
    g_mcu = new MCU();

    /* Load ROMs */
    uint8_t *rom1 = (uint8_t *)malloc(ROM1_SIZE);
    uint8_t *rom2 = (uint8_t *)malloc(ROM2_SIZE);
    uint8_t *waverom1 = (uint8_t *)malloc(0x200000);
    uint8_t *waverom2 = (uint8_t *)malloc(0x200000);
    uint8_t *nvram = (uint8_t *)malloc(NVRAM_SIZE);

    if (!rom1 || !rom2 || !waverom1 || !waverom2 || !nvram) {
        fprintf(stderr, "JV880: Memory allocation failed\n");
        return -1;
    }

    memset(nvram, 0xFF, NVRAM_SIZE);

    int ok = 1;
    ok = ok && load_rom("jv880_rom1.bin", rom1, ROM1_SIZE);
    ok = ok && load_rom("jv880_rom2.bin", rom2, ROM2_SIZE);
    ok = ok && load_rom("jv880_waverom1.bin", waverom1, 0x200000);
    ok = ok && load_rom("jv880_waverom2.bin", waverom2, 0x200000);

    /* NVRAM is optional */
    char nvram_path[1024];
    snprintf(nvram_path, sizeof(nvram_path), "%s/roms/jv880_nvram.bin", module_dir);
    FILE *nf = fopen(nvram_path, "rb");
    if (nf) {
        fread(nvram, 1, NVRAM_SIZE, nf);
        fclose(nf);
        fprintf(stderr, "JV880: Loaded NVRAM\n");
    }

    if (!ok) {
        fprintf(stderr, "JV880: ROM loading failed\n");
        free(rom1); free(rom2); free(waverom1); free(waverom2); free(nvram);
        delete g_mcu;
        g_mcu = nullptr;
        return -1;
    }

    /* Initialize emulator */
    g_mcu->startSC55(rom1, rom2, waverom1, waverom2, nvram);

    free(rom1); free(rom2); free(waverom1); free(waverom2); free(nvram);

    g_rom_loaded = 1;

    /* Warmup - run emulator for a bit to initialize */
    fprintf(stderr, "JV880: Running warmup...\n");
    for (int i = 0; i < 100000; i++) {
        g_mcu->updateSC55(1);
    }
    fprintf(stderr, "JV880: Warmup done\n");

    /* Pre-fill audio buffer */
    g_ring_write = 0;
    g_ring_read = 0;

    float resample_acc = 0.0f;
    const float ratio = (float)JV880_SAMPLE_RATE / (float)MOVE_SAMPLE_RATE;

    fprintf(stderr, "JV880: Pre-filling buffer...\n");
    for (int i = 0; i < 256; i++) {  /* Minimal pre-fill for low latency */
        g_mcu->updateSC55(8);
        int avail = g_mcu->sample_write_ptr;
        for (int j = 0; j < avail; j += 2) {
            resample_acc += 1.0f;
            if (resample_acc >= ratio) {
                resample_acc -= ratio;
                g_audio_ring[g_ring_write * 2 + 0] = g_mcu->sample_buffer[j];
                g_audio_ring[g_ring_write * 2 + 1] = g_mcu->sample_buffer[j + 1];
                g_ring_write = (g_ring_write + 1) % AUDIO_RING_SIZE;
            }
        }
    }
    fprintf(stderr, "JV880: Buffer pre-filled: %d samples\n", g_ring_write);

    /* Debug: check first few samples */
    fprintf(stderr, "JV880: First samples: %d %d %d %d\n",
            g_audio_ring[0], g_audio_ring[1], g_audio_ring[2], g_audio_ring[3]);

    /* Start emulation thread */
    g_thread_running = 1;
    pthread_create(&g_emu_thread, NULL, emu_thread_func, NULL);

    g_initialized = 1;
    fprintf(stderr, "JV880: Ready!\n");
    return 0;
}

static void jv880_on_unload(void) {
    if (g_thread_running) {
        g_thread_running = 0;
        pthread_join(g_emu_thread, NULL);
    }

    if (g_mcu) {
        delete g_mcu;
        g_mcu = nullptr;
    }

    g_initialized = 0;
    g_rom_loaded = 0;
}

static void jv880_on_midi(const uint8_t *msg, int len, int source) {
    (void)source;

    if (!g_initialized || !g_thread_running) return;

    /* Debug: show MIDI received */
    if (len >= 1) {
        fprintf(stderr, "JV880: MIDI recv [%02X", msg[0]);
        for (int i = 1; i < len && i < 8; i++) {
            fprintf(stderr, " %02X", msg[i]);
        }
        fprintf(stderr, "] len=%d\n", len);
    }

    int next = (g_midi_write + 1) % MIDI_QUEUE_SIZE;
    if (next != g_midi_read) {
        int n = (len > MIDI_MSG_MAX_LEN) ? MIDI_MSG_MAX_LEN : len;
        memcpy(g_midi_queue[g_midi_write], msg, n);
        g_midi_queue_len[g_midi_write] = n;
        g_midi_write = next;
    }
}

static void jv880_set_param(const char *key, const char *val) {
    (void)key; (void)val;
}

static int jv880_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "buffer_fill") == 0) {
        snprintf(buf, buf_len, "%d", ring_available());
        return 1;
    }
    return 0;
}

static int g_render_debug_count = 0;

static void jv880_render_block(int16_t *out, int frames) {
    if (!g_initialized || !g_thread_running) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    pthread_mutex_lock(&g_ring_mutex);
    int avail = ring_available();
    int to_read = (avail < frames) ? avail : frames;

    for (int i = 0; i < to_read; i++) {
        out[i * 2 + 0] = g_audio_ring[g_ring_read * 2 + 0];
        out[i * 2 + 1] = g_audio_ring[g_ring_read * 2 + 1];
        g_ring_read = (g_ring_read + 1) % AUDIO_RING_SIZE;
    }
    pthread_mutex_unlock(&g_ring_mutex);

    /* Debug output every ~1 second */
    g_render_debug_count++;
    if (g_render_debug_count % 344 == 1) {
        fprintf(stderr, "JV880: render avail=%d to_read=%d out[0]=%d out[1]=%d\n",
                avail, to_read, out[0], out[1]);
    }

    /* Pad with silence if underrun */
    for (int i = to_read; i < frames; i++) {
        out[i * 2 + 0] = 0;
        out[i * 2 + 1] = 0;
    }
}

static plugin_api_v1_t jv880_api = {
    1,
    jv880_on_load,
    jv880_on_unload,
    jv880_on_midi,
    jv880_set_param,
    jv880_get_param,
    jv880_render_block
};

extern "C" plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host) {
    (void)host;
    return &jv880_api;
}
