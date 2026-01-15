/*
 * Shadow Instrument Proof of Concept
 *
 * A minimal standalone process that demonstrates the shadow instrument
 * architecture:
 * - Connects to shared memory created by the shim
 * - Loads a synth module (SF2)
 * - Receives MIDI from the shim
 * - Renders audio and sends it back to be mixed
 * - Renders a simple display when in shadow mode
 *
 * Usage: ./shadow_poc [soundfont_path]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* Include plugin API for synth module interface */
#include "../host/plugin_api_v1.h"

/* ============================================================================
 * Shared Memory Configuration (must match shim)
 * ============================================================================ */

#define SHM_SHADOW_AUDIO   "/move-shadow-audio"
#define SHM_SHADOW_MIDI    "/move-shadow-midi"
#define SHM_SHADOW_DISPLAY "/move-shadow-display"
#define SHM_SHADOW_CONTROL "/move-shadow-control"

#define AUDIO_BUFFER_SIZE 512      /* 128 frames * 2 channels * 2 bytes */
#define MIDI_BUFFER_SIZE 256
#define DISPLAY_BUFFER_SIZE 1024   /* 128x64 @ 1bpp */
#define CONTROL_BUFFER_SIZE 64
#define FRAMES_PER_BLOCK 128

/* Shadow control structure - must match shim definition */
typedef struct {
    volatile uint8_t display_mode;    /* 0=stock Move, 1=shadow display */
    volatile uint8_t shadow_ready;    /* 1 when shadow process is running */
    volatile uint8_t should_exit;     /* 1 to signal shadow to exit */
    volatile uint8_t midi_ready;      /* increments when new MIDI available */
    volatile uint8_t reserved[60];
} shadow_control_t;

/* ============================================================================
 * Global State
 * ============================================================================ */

static int16_t *shadow_audio_shm = NULL;
static uint8_t *shadow_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;
static shadow_control_t *shadow_control = NULL;

static void *synth_handle = NULL;
static plugin_api_v1_t *synth_plugin = NULL;

static volatile int running = 1;
static uint8_t last_midi_ready = 0;

/* Host API for synth - minimal implementation */
static host_api_v1_t host_api;

/* ============================================================================
 * Logging
 * ============================================================================ */

static void shadow_log(const char *msg) {
    printf("shadow_poc: %s\n", msg);
}

/* ============================================================================
 * Simple Display Rendering (1-bit packed format)
 * ============================================================================ */

/* Simple 5x7 font for basic text */
static const uint8_t font_5x7[96][5] = {
    /* Space to ~ (ASCII 32-126) - simplified subset */
    {0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x00,0x08,0x14,0x22,0x41}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x41,0x22,0x14,0x08,0x00}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x01,0x01}, /* F */
    {0x3E,0x41,0x41,0x51,0x32}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x04,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x03,0x04,0x78,0x04,0x03}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
};

/* Set a pixel in the packed display buffer */
static void set_pixel(uint8_t *buf, int x, int y, int on) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    int byte_idx = (y / 8) * 128 + x;
    int bit_idx = y % 8;
    if (on) {
        buf[byte_idx] |= (1 << bit_idx);
    } else {
        buf[byte_idx] &= ~(1 << bit_idx);
    }
}

/* Draw a character at position */
static void draw_char(uint8_t *buf, int x, int y, char c) {
    if (c < 32 || c > 90) c = ' ';
    int idx = c - 32;
    for (int col = 0; col < 5; col++) {
        uint8_t line = font_5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                set_pixel(buf, x + col, y + row, 1);
            }
        }
    }
}

/* Draw a string */
static void draw_string(uint8_t *buf, int x, int y, const char *str) {
    while (*str) {
        draw_char(buf, x, y, *str);
        x += 6;  /* 5 pixel width + 1 space */
        str++;
    }
}

/* Clear display buffer */
static void clear_display(uint8_t *buf) {
    memset(buf, 0, DISPLAY_BUFFER_SIZE);
}

/* Render the shadow display */
static void render_shadow_display(void) {
    if (!shadow_display_shm) return;

    clear_display(shadow_display_shm);

    /* Title */
    draw_string(shadow_display_shm, 20, 4, "SHADOW MODE");

    /* Horizontal line */
    for (int x = 0; x < 128; x++) {
        set_pixel(shadow_display_shm, x, 14, 1);
    }

    /* Status */
    draw_string(shadow_display_shm, 8, 20, "SF2 SYNTH LOADED");
    draw_string(shadow_display_shm, 8, 32, "RECEIVING MIDI");

    /* Instructions */
    draw_string(shadow_display_shm, 4, 50, "SHIFT+VOL+KNOB1:");
    draw_string(shadow_display_shm, 4, 58, "RETURN TO MOVE");
}

/* ============================================================================
 * Shared Memory Setup
 * ============================================================================ */

static int open_shm(void) {
    int fd;

    /* Open audio shared memory */
    fd = shm_open(SHM_SHADOW_AUDIO, O_RDWR, 0666);
    if (fd < 0) {
        perror("Failed to open audio shm");
        return -1;
    }
    shadow_audio_shm = (int16_t *)mmap(NULL, AUDIO_BUFFER_SIZE,
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED, fd, 0);
    close(fd);
    if (shadow_audio_shm == MAP_FAILED) {
        perror("Failed to mmap audio shm");
        return -1;
    }

    /* Open MIDI shared memory */
    fd = shm_open(SHM_SHADOW_MIDI, O_RDONLY, 0666);
    if (fd < 0) {
        perror("Failed to open MIDI shm");
        return -1;
    }
    shadow_midi_shm = (uint8_t *)mmap(NULL, MIDI_BUFFER_SIZE,
                                       PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (shadow_midi_shm == MAP_FAILED) {
        perror("Failed to mmap MIDI shm");
        return -1;
    }

    /* Open display shared memory */
    fd = shm_open(SHM_SHADOW_DISPLAY, O_RDWR, 0666);
    if (fd < 0) {
        perror("Failed to open display shm");
        return -1;
    }
    shadow_display_shm = (uint8_t *)mmap(NULL, DISPLAY_BUFFER_SIZE,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd, 0);
    close(fd);
    if (shadow_display_shm == MAP_FAILED) {
        perror("Failed to mmap display shm");
        return -1;
    }

    /* Open control shared memory */
    fd = shm_open(SHM_SHADOW_CONTROL, O_RDWR, 0666);
    if (fd < 0) {
        perror("Failed to open control shm");
        return -1;
    }
    shadow_control = (shadow_control_t *)mmap(NULL, CONTROL_BUFFER_SIZE,
                                               PROT_READ | PROT_WRITE,
                                               MAP_SHARED, fd, 0);
    close(fd);
    if (shadow_control == MAP_FAILED) {
        perror("Failed to mmap control shm");
        return -1;
    }

    printf("Shared memory opened successfully\n");
    return 0;
}

/* ============================================================================
 * Synth Module Loading
 * ============================================================================ */

static int load_synth(const char *soundfont_path) {
    const char *module_path = "/data/UserData/move-anything/modules/sf2/dsp.so";
    const char *module_dir = "/data/UserData/move-anything/modules/sf2";

    printf("Loading synth from %s\n", module_path);

    /* Load the shared library */
    synth_handle = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    if (!synth_handle) {
        fprintf(stderr, "Failed to load synth: %s\n", dlerror());
        return -1;
    }

    /* Get the init function */
    move_plugin_init_v1_fn init_fn = (move_plugin_init_v1_fn)dlsym(synth_handle, MOVE_PLUGIN_INIT_SYMBOL);
    if (!init_fn) {
        fprintf(stderr, "Failed to find init symbol: %s\n", dlerror());
        dlclose(synth_handle);
        synth_handle = NULL;
        return -1;
    }

    /* Setup minimal host API */
    memset(&host_api, 0, sizeof(host_api));
    host_api.api_version = MOVE_PLUGIN_API_VERSION;
    host_api.sample_rate = MOVE_SAMPLE_RATE;
    host_api.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host_api.log = shadow_log;
    /* Note: No mapped_memory or MIDI send in POC */

    /* Initialize the plugin */
    synth_plugin = init_fn(&host_api);
    if (!synth_plugin) {
        fprintf(stderr, "Plugin init returned NULL\n");
        dlclose(synth_handle);
        synth_handle = NULL;
        return -1;
    }

    /* Call on_load */
    if (synth_plugin->on_load) {
        int result = synth_plugin->on_load(module_dir, NULL);
        if (result != 0) {
            fprintf(stderr, "Plugin on_load failed: %d\n", result);
            dlclose(synth_handle);
            synth_handle = NULL;
            synth_plugin = NULL;
            return -1;
        }
    }

    /* Set soundfont if provided */
    if (soundfont_path && synth_plugin->set_param) {
        printf("Setting soundfont: %s\n", soundfont_path);
        synth_plugin->set_param("soundfont_path", soundfont_path);
    }

    printf("Synth loaded successfully\n");
    return 0;
}

static void unload_synth(void) {
    if (synth_plugin && synth_plugin->on_unload) {
        synth_plugin->on_unload();
    }
    if (synth_handle) {
        dlclose(synth_handle);
    }
    synth_plugin = NULL;
    synth_handle = NULL;
}

/* ============================================================================
 * MIDI Processing
 * ============================================================================ */

static void process_midi(void) {
    if (!shadow_midi_shm || !synth_plugin || !synth_plugin->on_midi) return;

    /* Process all MIDI packets in the buffer */
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t *pkt = &shadow_midi_shm[i];

        /* Skip empty packets */
        if (pkt[0] == 0 && pkt[1] == 0 && pkt[2] == 0 && pkt[3] == 0) {
            continue;
        }

        /* Extract MIDI message (skip USB-MIDI header byte) */
        uint8_t cable = (pkt[0] >> 4) & 0x0F;
        uint8_t cin = pkt[0] & 0x0F;

        /* Only process internal MIDI (cable 0) for POC */
        if (cable != 0) continue;

        /* Skip system messages and invalid CINs */
        if (cin < 0x08 || cin > 0x0E) continue;

        /* Forward to synth (3-byte MIDI message) */
        synth_plugin->on_midi(&pkt[1], 3, MOVE_MIDI_SOURCE_INTERNAL);
    }
}

/* ============================================================================
 * Audio Rendering
 * ============================================================================ */

static void render_audio(void) {
    if (!shadow_audio_shm) return;

    int16_t render_buffer[FRAMES_PER_BLOCK * 2];
    memset(render_buffer, 0, sizeof(render_buffer));

    if (synth_plugin && synth_plugin->render_block) {
        synth_plugin->render_block(render_buffer, FRAMES_PER_BLOCK);
    }

    /* Copy to shared memory */
    memcpy(shadow_audio_shm, render_buffer, AUDIO_BUFFER_SIZE);
}

/* ============================================================================
 * Signal Handling
 * ============================================================================ */

static void signal_handler(int sig) {
    (void)sig;
    printf("\nReceived signal, shutting down...\n");
    running = 0;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    const char *soundfont_path = NULL;

    printf("=== Shadow Instrument POC ===\n");

    /* Parse arguments */
    if (argc > 1) {
        soundfont_path = argv[1];
        printf("Soundfont: %s\n", soundfont_path);
    } else {
        printf("No soundfont specified, using module default\n");
    }

    /* Setup signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Open shared memory */
    if (open_shm() < 0) {
        fprintf(stderr, "Failed to open shared memory. Is Move running with the shadow shim?\n");
        return 1;
    }

    /* Load synth */
    if (load_synth(soundfont_path) < 0) {
        fprintf(stderr, "Failed to load synth module.\n");
        fprintf(stderr, "Make sure the SF2 module is installed at:\n");
        fprintf(stderr, "  /data/UserData/move-anything/modules/sf2/dsp.so\n");
        return 1;
    }

    /* Render initial display */
    render_shadow_display();

    /* Signal ready */
    shadow_control->shadow_ready = 1;
    printf("Shadow ready, entering main loop...\n");

    /* Main loop */
    while (running && !shadow_control->should_exit) {
        /* Check for new MIDI */
        if (shadow_control->midi_ready != last_midi_ready) {
            last_midi_ready = shadow_control->midi_ready;
            process_midi();
        }

        /* Render audio */
        render_audio();

        /* Update display if in shadow mode */
        if (shadow_control->display_mode) {
            render_shadow_display();
        }

        /* Pace to roughly match audio block rate (~3ms) */
        usleep(2900);
    }

    /* Cleanup */
    printf("Shutting down...\n");
    shadow_control->shadow_ready = 0;
    unload_synth();

    /* Unmap shared memory */
    if (shadow_audio_shm) munmap(shadow_audio_shm, AUDIO_BUFFER_SIZE);
    if (shadow_midi_shm) munmap((void*)shadow_midi_shm, MIDI_BUFFER_SIZE);
    if (shadow_display_shm) munmap(shadow_display_shm, DISPLAY_BUFFER_SIZE);
    if (shadow_control) munmap(shadow_control, CONTROL_BUFFER_SIZE);

    printf("Goodbye!\n");
    return 0;
}
