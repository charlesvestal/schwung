#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <math.h>
#include <linux/spi/spidev.h>
#include <pthread.h>
#include <dbus/dbus.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
#include "host/shadow_constants.h"

/* Debug flags - set to 1 to enable various debug logging */
#define SHADOW_DEBUG 0           /* Master debug flag for mailbox/MIDI debug */
#define SHADOW_TRACE_DEBUG 0     /* SPI/MIDI trace logging */
#define SHADOW_TIMING_LOG 0      /* ioctl/DSP timing logs to /tmp */

unsigned char *global_mmap_addr = NULL;  /* Points to shadow_mailbox (what Move sees) */
unsigned char *hardware_mmap_addr = NULL; /* Points to real hardware mailbox */
static unsigned char shadow_mailbox[4096] __attribute__((aligned(64))); /* Shadow buffer for Move */
FILE *output_file;
int frame_counter = 0;

/* ============================================================================
 * SHADOW INSTRUMENT SUPPORT
 * ============================================================================
 * The shadow instrument allows a separate DSP process to run alongside stock
 * Move, mixing its audio output with Move's audio and optionally taking over
 * the display when in shadow mode.
 * ============================================================================ */

/* Mailbox layout constants */
#define MAILBOX_SIZE 4096
#define MIDI_OUT_OFFSET 0
#define AUDIO_OUT_OFFSET 256
#define DISPLAY_OFFSET 768
#define MIDI_IN_OFFSET 2048
#define AUDIO_IN_OFFSET 2304

#define AUDIO_BUFFER_SIZE 512      /* 128 frames * 2 channels * 2 bytes */
/* Buffer sizes from shadow_constants.h: MIDI_BUFFER_SIZE, DISPLAY_BUFFER_SIZE,
   CONTROL_BUFFER_SIZE, SHADOW_UI_BUFFER_SIZE, SHADOW_PARAM_BUFFER_SIZE */
#define FRAMES_PER_BLOCK 128

/* Move host shortcut CCs (mirror move_anything.c) */
#define CC_SHIFT 49
#define CC_JOG_CLICK 3
#define CC_JOG_WHEEL 14
#define CC_BACK 51
#define CC_MASTER_KNOB 79
#define CC_UP 55
#define CC_DOWN 54
#define CC_MENU 50
#define CC_CAPTURE 52
#define CC_UNDO 56
#define CC_LOOP 58
#define CC_COPY 60
#define CC_LEFT 62
#define CC_RIGHT 63
#define CC_KNOB1 71
#define CC_KNOB2 72
#define CC_KNOB3 73
#define CC_KNOB4 74
#define CC_KNOB5 75
#define CC_KNOB6 76
#define CC_KNOB7 77
#define CC_KNOB8 78
#define CC_PLAY 85
#define CC_REC 86
#define CC_MUTE 88
#define CC_MIC_IN_DETECT 114
#define CC_LINE_OUT_DETECT 115
#define CC_RECORD 118
#define CC_DELETE 119
#define CC_STEP_UI_FIRST 16
#define CC_STEP_UI_LAST 31

/* Shadow structs from shadow_constants.h: shadow_control_t, shadow_ui_state_t, shadow_param_t */
static shadow_control_t *shadow_control = NULL;
static uint8_t shadow_display_mode = 0;

static shadow_ui_state_t *shadow_ui_state = NULL;

static shadow_param_t *shadow_param = NULL;

static void launch_shadow_ui(void);

static uint32_t shadow_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum * 33u) ^ buf[i];
    }
    return sum;
}

/* ============================================================================
 * MIDI DEVICE TRACE (DISCOVERY)
 * ============================================================================
 * Track open/read/write on MIDI-ish device nodes to discover where Move sends
 * external MIDI. Enabled by creating midi_fd_trace_on.
 * ============================================================================ */

#define MAX_TRACKED_FDS 32
typedef struct {
    int fd;
    char path[128];
} tracked_fd_t;

static tracked_fd_t tracked_fds[MAX_TRACKED_FDS];
static FILE *midi_fd_trace_log = NULL;
static FILE *spi_io_log = NULL;

static int trace_midi_fd_enabled(void)
{
    static int enabled = -1;
    static int check_counter = 0;
    if (check_counter++ % 200 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/midi_fd_trace_on", F_OK) == 0);
    }
    return enabled;
}

static void midi_fd_trace_log_open(void)
{
    if (!midi_fd_trace_log) {
        midi_fd_trace_log = fopen("/data/UserData/move-anything/midi_fd_trace.log", "a");
    }
}

static int trace_spi_io_enabled(void)
{
    static int enabled = -1;
    static int check_counter = 0;
    if (check_counter++ % 200 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/spi_io_on", F_OK) == 0);
    }
    return enabled;
}

static void spi_io_log_open(void)
{
    if (!spi_io_log) {
        spi_io_log = fopen("/data/UserData/move-anything/spi_io.log", "a");
    }
}

static void str_to_lower(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (!dst_size) return;
    for (; i + 1 < dst_size && src[i]; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
    }
    dst[i] = '\0';
}

static int path_matches_midi(const char *path)
{
    if (!path || !*path) return 0;
    char lower[256];
    str_to_lower(lower, sizeof(lower), path);
    return strstr(lower, "midi") || strstr(lower, "snd") ||
           strstr(lower, "seq") || strstr(lower, "usb");
}

static int path_matches_spi(const char *path)
{
    if (!path || !*path) return 0;
    char lower[256];
    str_to_lower(lower, sizeof(lower), path);
    return strstr(lower, "ablspi") || strstr(lower, "spidev") ||
           strstr(lower, "/spi");
}

static void track_fd(int fd, const char *path)
{
    if (fd < 0 || !path) return;
    for (int i = 0; i < MAX_TRACKED_FDS; i++) {
        if (tracked_fds[i].fd == 0) {
            tracked_fds[i].fd = fd;
            strncpy(tracked_fds[i].path, path, sizeof(tracked_fds[i].path) - 1);
            tracked_fds[i].path[sizeof(tracked_fds[i].path) - 1] = '\0';
            return;
        }
    }
}

static void untrack_fd(int fd)
{
    for (int i = 0; i < MAX_TRACKED_FDS; i++) {
        if (tracked_fds[i].fd == fd) {
            tracked_fds[i].fd = 0;
            tracked_fds[i].path[0] = '\0';
            return;
        }
    }
}

static const char *tracked_path_for_fd(int fd)
{
    for (int i = 0; i < MAX_TRACKED_FDS; i++) {
        if (tracked_fds[i].fd == fd) {
            return tracked_fds[i].path;
        }
    }
    return NULL;
}

static void log_fd_bytes(const char *tag, int fd, const char *path,
                         const unsigned char *buf, size_t len)
{
    size_t max = len > 64 ? 64 : len;
    if (path_matches_midi(path)) {
        if (!trace_midi_fd_enabled()) return;
        midi_fd_trace_log_open();
        if (!midi_fd_trace_log) return;
        fprintf(midi_fd_trace_log, "%s fd=%d path=%s len=%zu bytes:", tag, fd, path, len);
        for (size_t i = 0; i < max; i++) {
            fprintf(midi_fd_trace_log, " %02x", buf[i]);
        }
        if (len > max) fprintf(midi_fd_trace_log, " ...");
        fprintf(midi_fd_trace_log, "\n");
        fflush(midi_fd_trace_log);
    }
    if (path_matches_spi(path)) {
        if (!trace_spi_io_enabled()) return;
        spi_io_log_open();
        if (!spi_io_log) return;
        fprintf(spi_io_log, "%s fd=%d path=%s len=%zu bytes:", tag, fd, path, len);
        for (size_t i = 0; i < max; i++) {
            fprintf(spi_io_log, " %02x", buf[i]);
        }
        if (len > max) fprintf(spi_io_log, " ...");
        fprintf(spi_io_log, "\n");
        fflush(spi_io_log);
    }
}

/* ============================================================================
 * MAILBOX DIFF PROBE (XMOS PATH DISCOVERY)
 * ============================================================================
 * Compare mailbox snapshots to find where MIDI-like bytes appear when playing.
 * Enabled by creating mailbox_diff_on; optional mailbox_snapshot_on dumps once.
 * ============================================================================ */

static FILE *mailbox_diff_log = NULL;
static void mailbox_diff_log_open(void)
{
    if (!mailbox_diff_log) {
        mailbox_diff_log = fopen("/data/UserData/move-anything/mailbox_diff.log", "a");
    }
}

static int mailbox_diff_enabled(void)
{
    static int enabled = -1;
    static int check_counter = 0;
    if (check_counter++ % 200 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/mailbox_diff_on", F_OK) == 0);
    }
    return enabled;
}

static void mailbox_snapshot_once(void)
{
    if (!global_mmap_addr) return;
    if (access("/data/UserData/move-anything/mailbox_snapshot_on", F_OK) != 0) return;

    FILE *snap = fopen("/data/UserData/move-anything/mailbox_snapshot.log", "w");
    if (snap) {
        fprintf(snap, "Mailbox snapshot (4096 bytes):\n");
        for (int i = 0; i < MAILBOX_SIZE; i++) {
            if (i % 256 == 0) fprintf(snap, "\n=== OFFSET %d (0x%x) ===\n", i, i);
            fprintf(snap, "%02x ", (unsigned char)global_mmap_addr[i]);
            if ((i + 1) % 32 == 0) fprintf(snap, "\n");
        }
        fclose(snap);
    }
    unlink("/data/UserData/move-anything/mailbox_snapshot_on");
}

static void mailbox_diff_probe(void)
{
    static unsigned char prev[MAILBOX_SIZE];
    static int has_prev = 0;
    static unsigned int counter = 0;

    if (!global_mmap_addr) return;
    mailbox_snapshot_once();

    if (!mailbox_diff_enabled()) return;
    if (++counter % 10 != 0) return;

    mailbox_diff_log_open();
    if (!mailbox_diff_log) return;

    if (!has_prev) {
        memcpy(prev, global_mmap_addr, MAILBOX_SIZE);
        fprintf(mailbox_diff_log, "INIT snapshot\n");
        fflush(mailbox_diff_log);
        has_prev = 1;
        return;
    }

    for (int i = 0; i < MAILBOX_SIZE - 2; i++) {
        unsigned char b = global_mmap_addr[i];
        unsigned char p = prev[i];
        if (b == p) continue;

        if ((b >= 0x80 && b <= 0xEF) || (p >= 0x80 && p <= 0xEF)) {
            fprintf(mailbox_diff_log,
                    "DIFF[%d]: %02x->%02x next=%02x %02x\n",
                    i, p, b, global_mmap_addr[i + 1], global_mmap_addr[i + 2]);
        }
    }

    fflush(mailbox_diff_log);
    memcpy(prev, global_mmap_addr, MAILBOX_SIZE);
}

/* Scan full mailbox for strict 3-byte MIDI with channel 3 status bytes. */
static FILE *mailbox_midi_log = NULL;
static void mailbox_midi_scan_strict(void)
{
    static int enabled = -1;
    static int check_counter = 0;
    if (check_counter++ % 200 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/midi_strict_on", F_OK) == 0);
    }

    if (!enabled || !global_mmap_addr) return;

    if (!mailbox_midi_log) {
        mailbox_midi_log = fopen("/data/UserData/move-anything/midi_strict.log", "a");
    }
    if (!mailbox_midi_log) return;

    for (int i = 0; i < MAILBOX_SIZE - 2; i++) {
        uint8_t status = global_mmap_addr[i];
        if (status != 0x92 && status != 0x82) continue;

        uint8_t d1 = global_mmap_addr[i + 1];
        uint8_t d2 = global_mmap_addr[i + 2];
        if (d1 >= 0x80 || d2 >= 0x80) continue;

        const char *region = "OTHER";
        if (i >= MIDI_OUT_OFFSET && i < MIDI_OUT_OFFSET + MIDI_BUFFER_SIZE) {
            region = "MIDI_OUT";
        } else if (i >= MIDI_IN_OFFSET && i < MIDI_IN_OFFSET + MIDI_BUFFER_SIZE) {
            region = "MIDI_IN";
        } else if (i >= AUDIO_OUT_OFFSET && i < AUDIO_OUT_OFFSET + AUDIO_BUFFER_SIZE) {
            region = "AUDIO_OUT";
        } else if (i >= AUDIO_IN_OFFSET && i < AUDIO_IN_OFFSET + AUDIO_BUFFER_SIZE) {
            region = "AUDIO_IN";
        }

        if (i > 0) {
            uint8_t b0 = global_mmap_addr[i - 1];
            fprintf(mailbox_midi_log, "MIDI[%d] %s: %02x %02x %02x %02x\n",
                    i, region, b0, status, d1, d2);
        } else {
            fprintf(mailbox_midi_log, "MIDI[%d] %s: %02x %02x %02x\n",
                    i, region, status, d1, d2);
        }
    }

    fflush(mailbox_midi_log);
}

/*
 * USB-MIDI Packet Format (per USB Device Class Definition for MIDI Devices 1.0)
 * https://www.usb.org/sites/default/files/midi10.pdf
 *
 * Each packet is 4 bytes:
 *   Byte 0: [Cable Number (4 bits)] [CIN (4 bits)]
 *   Byte 1: MIDI Status byte
 *   Byte 2: MIDI Data 1
 *   Byte 3: MIDI Data 2
 *
 * CIN (Code Index Number) for channel voice messages:
 *   0x08 = Note Off
 *   0x09 = Note On
 *   0x0A = Poly Aftertouch
 *   0x0B = Control Change
 *   0x0C = Program Change
 *   0x0D = Channel Pressure
 *   0x0E = Pitch Bend
 */

/* Scan for USB-MIDI 4-byte packets anywhere in the mailbox. */
static FILE *mailbox_usb_log = NULL;
static void mailbox_usb_midi_scan(void)
{
    static int enabled = -1;
    static int check_counter = 0;
    if (check_counter++ % 200 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/usb_midi_on", F_OK) == 0);
    }

    if (!enabled || !global_mmap_addr) return;

    if (!mailbox_usb_log) {
        mailbox_usb_log = fopen("/data/UserData/move-anything/usb_midi.log", "a");
    }
    if (!mailbox_usb_log) return;

    for (int i = 0; i < MAILBOX_SIZE - 4; i += 4) {
        uint8_t cin = global_mmap_addr[i] & 0x0F;
        if (cin < 0x08 || cin > 0x0E) continue;

        uint8_t status = global_mmap_addr[i + 1];
        uint8_t d1 = global_mmap_addr[i + 2];
        uint8_t d2 = global_mmap_addr[i + 3];
        if (status < 0x80 || status > 0xEF) continue;
        if (d1 >= 0x80 || d2 >= 0x80) continue;

        const char *region = "OTHER";
        if (i >= MIDI_OUT_OFFSET && i < MIDI_OUT_OFFSET + MIDI_BUFFER_SIZE) {
            region = "MIDI_OUT";
        } else if (i >= MIDI_IN_OFFSET && i < MIDI_IN_OFFSET + MIDI_BUFFER_SIZE) {
            region = "MIDI_IN";
        } else if (i >= AUDIO_OUT_OFFSET && i < AUDIO_OUT_OFFSET + AUDIO_BUFFER_SIZE) {
            region = "AUDIO_OUT";
        } else if (i >= AUDIO_IN_OFFSET && i < AUDIO_IN_OFFSET + AUDIO_BUFFER_SIZE) {
            region = "AUDIO_IN";
        }

        fprintf(mailbox_usb_log, "USB[%d] %s: %02x %02x %02x %02x\n",
                i, region,
                global_mmap_addr[i],
                status, d1, d2);
    }

    fflush(mailbox_usb_log);
}

/* Scan MIDI_IN/OUT regions only for strict 3-byte MIDI status/data patterns. */
static FILE *midi_region_log = NULL;
static void mailbox_midi_region_scan(void)
{
    static int enabled = -1;
    static int check_counter = 0;
    if (check_counter++ % 200 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/midi_region_on", F_OK) == 0);
    }

    if (!enabled || !global_mmap_addr) return;

    if (!midi_region_log) {
        midi_region_log = fopen("/data/UserData/move-anything/midi_region.log", "a");
    }
    if (!midi_region_log) return;

    for (int i = 0; i < MIDI_BUFFER_SIZE - 2; i++) {
        uint8_t status = global_mmap_addr[MIDI_OUT_OFFSET + i];
        uint8_t d1 = global_mmap_addr[MIDI_OUT_OFFSET + i + 1];
        uint8_t d2 = global_mmap_addr[MIDI_OUT_OFFSET + i + 2];
        if (status >= 0x80 && status <= 0xEF && d1 < 0x80 && d2 < 0x80) {
            fprintf(midi_region_log, "OUT[%d]: %02x %02x %02x\n", i, status, d1, d2);
        }
    }

    for (int i = 0; i < MIDI_BUFFER_SIZE - 2; i++) {
        uint8_t status = global_mmap_addr[MIDI_IN_OFFSET + i];
        uint8_t d1 = global_mmap_addr[MIDI_IN_OFFSET + i + 1];
        uint8_t d2 = global_mmap_addr[MIDI_IN_OFFSET + i + 2];
        if (status >= 0x80 && status <= 0xEF && d1 < 0x80 && d2 < 0x80) {
            fprintf(midi_region_log, "IN [%d]: %02x %02x %02x\n", i, status, d1, d2);
        }
    }

    fflush(midi_region_log);
}

/* Log MIDI_OUT changes across frames to reverse-engineer encoding. */
static FILE *midi_frame_log = NULL;
static void mailbox_midi_out_frame_log(void)
{
    static int enabled = -1;
    static int check_counter = 0;
    static int frame_count = 0;
    static uint8_t prev[MIDI_BUFFER_SIZE];
    static int has_prev = 0;

    if (check_counter++ % 50 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/midi_frame_on", F_OK) == 0);
        if (!enabled) {
            frame_count = 0;
            has_prev = 0;
        }
    }

    if (!enabled || !global_mmap_addr) return;

    if (!midi_frame_log) {
        midi_frame_log = fopen("/data/UserData/move-anything/midi_frame.log", "a");
    }
    if (!midi_frame_log) return;

    uint8_t *src = global_mmap_addr + MIDI_OUT_OFFSET;
    if (!has_prev) {
        memcpy(prev, src, MIDI_BUFFER_SIZE);
        fprintf(midi_frame_log, "FRAME %d (init)\n", frame_count);
        fflush(midi_frame_log);
        has_prev = 1;
        return;
    }

    fprintf(midi_frame_log, "FRAME %d\n", frame_count);
    for (int i = 0; i < MIDI_BUFFER_SIZE; i++) {
        if (prev[i] != src[i]) {
            fprintf(midi_frame_log, "  %03d %02x->%02x\n", i, prev[i], src[i]);
        }
    }
    fflush(midi_frame_log);
    memcpy(prev, src, MIDI_BUFFER_SIZE);

    frame_count++;
    if (frame_count >= 30) {
        unlink("/data/UserData/move-anything/midi_frame_on");
    }
}

/* ============================================================================
 * SPI IOCTL TRACE (XMOS PATH DISCOVERY)
 * ============================================================================
 * Log SPI transfers when enabled by spi_trace_on to locate MIDI bytes.
 * ============================================================================ */

static FILE *spi_trace_log = NULL;
static void spi_trace_log_open(void)
{
    if (!spi_trace_log) {
        spi_trace_log = fopen("/data/UserData/move-anything/spi_trace.log", "a");
    }
}

static int spi_trace_enabled(void)
{
    static int enabled = -1;
    static int check_counter = 0;
    if (check_counter++ % 200 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/spi_trace_on", F_OK) == 0);
    }
    return enabled;
}

static void spi_trace_log_buf(const char *tag, const uint8_t *buf, size_t len)
{
    if (!spi_trace_log) return;
    size_t max = len > 64 ? 64 : len;
    fprintf(spi_trace_log, "%s len=%zu bytes:", tag, len);
    for (size_t i = 0; i < max; i++) {
        fprintf(spi_trace_log, " %02x", buf[i]);
    }
    if (len > max) fprintf(spi_trace_log, " ...");
    fprintf(spi_trace_log, "\n");
}

static void spi_trace_ioctl(unsigned long request, char *argp)
{
    if (!spi_trace_enabled()) return;
    spi_trace_log_open();
    if (!spi_trace_log) return;

    static unsigned int counter = 0;
    if (++counter % 10 != 0) return;

    unsigned int size = _IOC_SIZE(request);
    fprintf(spi_trace_log, "IOCTL req=0x%lx size=%u\n", request, size);

    if (_IOC_TYPE(request) == SPI_IOC_MAGIC && size >= sizeof(struct spi_ioc_transfer)) {
        int n = (int)(size / sizeof(struct spi_ioc_transfer));
        struct spi_ioc_transfer *xfers = (struct spi_ioc_transfer *)argp;
        for (int i = 0; i < n; i++) {
            const struct spi_ioc_transfer *x = &xfers[i];
            fprintf(spi_trace_log, "  XFER[%d] len=%u tx=%p rx=%p\n",
                    i, x->len, (void *)(uintptr_t)x->tx_buf, (void *)(uintptr_t)x->rx_buf);
            if (x->tx_buf && x->len) {
                uint8_t tmp[256];
                size_t copy_len = x->len > sizeof(tmp) ? sizeof(tmp) : x->len;
                memcpy(tmp, (const void *)(uintptr_t)x->tx_buf, copy_len);
                spi_trace_log_buf("  TX", tmp, copy_len);
            }
            if (x->rx_buf && x->len) {
                uint8_t tmp[256];
                size_t copy_len = x->len > sizeof(tmp) ? sizeof(tmp) : x->len;
                memcpy(tmp, (const void *)(uintptr_t)x->rx_buf, copy_len);
                spi_trace_log_buf("  RX", tmp, copy_len);
            }
        }
    }

    fflush(spi_trace_log);
}

/* ============================================================================
 * IN-PROCESS SHADOW CHAIN (MULTI-PATCH)
 * ============================================================================
 * Load the chain DSP inside the shim and render in the ioctl audio cadence.
 * This avoids IPC timing drift and provides a stable audio mix proof-of-concept.
 * ============================================================================ */

#define SHADOW_INPROCESS_POC 1
#define SHADOW_DISABLE_POST_IOCTL_MIDI 0  /* Set to 1 to disable post-ioctl MIDI forwarding for debugging */
#define SHADOW_CHAIN_MODULE_DIR "/data/UserData/move-anything/modules/chain"
#define SHADOW_CHAIN_DSP_PATH "/data/UserData/move-anything/modules/chain/dsp.so"
#define SHADOW_CHAIN_CONFIG_PATH "/data/UserData/move-anything/shadow_chain_config.json"
/* SHADOW_CHAIN_INSTANCES from shadow_constants.h */

/* System volume - for now just a placeholder, we'll find the real source */
static float shadow_master_gain = 1.0f;

/* Forward declaration */
static uint64_t now_mono_ms(void);

#if SHADOW_DEBUG
/* Debug: dump full mailbox to find volume/control data in SPI */
static uint64_t mailbox_dump_last_ms = 0;
static uint8_t mailbox_dump_prev[4096];
static int mailbox_dump_init = 0;
static int mailbox_dump_count = 0;

static void debug_dump_mailbox_changes(void) {
    if (!global_mmap_addr) return;

    /* Only check if debug file exists */
    static int enabled = -1;
    static int check_counter = 0;
    if (check_counter++ % 1000 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/mailbox_dump_on", F_OK) == 0);
    }
    if (!enabled) return;

    uint64_t now = now_mono_ms();
    if (now - mailbox_dump_last_ms < 500) return;  /* Twice per second */
    mailbox_dump_last_ms = now;

    if (!mailbox_dump_init) {
        memcpy(mailbox_dump_prev, global_mmap_addr, MAILBOX_SIZE);
        mailbox_dump_init = 1;
        return;
    }

    /* Check for changes in NON-AUDIO regions (skip audio data which changes constantly) */
    /* Layout: 0-256 MIDI_OUT, 256-768 AUDIO_OUT, 768-1792 DISPLAY,
       1792-2048 unknown, 2048-2304 MIDI_IN, 2304-2816 AUDIO_IN, 2816-4096 unknown */
    int changed = 0;
    /* Check MIDI_OUT region (0-256) - might have control data */
    for (int i = 0; i < 256; i++) {
        if (global_mmap_addr[i] != mailbox_dump_prev[i]) { changed = 1; break; }
    }
    /* Check region between display and MIDI_IN (1792-2048) */
    if (!changed) {
        for (int i = 1792; i < 2048; i++) {
            if (global_mmap_addr[i] != mailbox_dump_prev[i]) { changed = 1; break; }
        }
    }
    /* Check region after AUDIO_IN (2816-4096) */
    if (!changed) {
        for (int i = 2816; i < 4096; i++) {
            if (global_mmap_addr[i] != mailbox_dump_prev[i]) { changed = 1; break; }
        }
    }

    /* Also dump first few samples if triggered, to see if audio level changes */
    FILE *f = fopen("/data/UserData/move-anything/mailbox_dump.log", "a");
    if (f) {
        if (changed || mailbox_dump_count < 3) {
            fprintf(f, "=== Mailbox snapshot #%d at %llu ===\n", mailbox_dump_count, (unsigned long long)now);

            /* Dump MIDI_OUT region (0-256) - look for control bytes */
            fprintf(f, "MIDI_OUT (0-256) non-zero bytes:\n");
            for (int i = 0; i < 256; i++) {
                if (global_mmap_addr[i] != 0) {
                    fprintf(f, "  [%d]=0x%02x", i, global_mmap_addr[i]);
                }
            }
            fprintf(f, "\n");

            /* Dump first 16 bytes of audio out as potential header */
            fprintf(f, "AUDIO_OUT first 32 bytes: ");
            for (int i = 256; i < 288; i++) {
                fprintf(f, "%02x ", global_mmap_addr[i]);
            }
            fprintf(f, "\n");

            /* Check audio levels (RMS of first few samples) */
            int16_t *audio = (int16_t*)(global_mmap_addr + AUDIO_OUT_OFFSET);
            int64_t sum_sq = 0;
            for (int i = 0; i < 32; i++) {
                sum_sq += (int64_t)audio[i] * audio[i];
            }
            double rms = sqrt((double)sum_sq / 32);
            fprintf(f, "Audio RMS (first 32 samples): %.1f\n", rms);

            /* Dump unknown regions */
            fprintf(f, "Region 1792-2048: ");
            int any_nonzero = 0;
            for (int i = 1792; i < 2048; i++) {
                if (global_mmap_addr[i] != 0) {
                    fprintf(f, "[%d]=0x%02x ", i, global_mmap_addr[i]);
                    any_nonzero = 1;
                }
            }
            if (!any_nonzero) fprintf(f, "(all zeros)");
            fprintf(f, "\n");

            fprintf(f, "Region 2816-4096: ");
            any_nonzero = 0;
            for (int i = 2816; i < 4096; i++) {
                if (global_mmap_addr[i] != 0) {
                    fprintf(f, "[%d]=0x%02x ", i, global_mmap_addr[i]);
                    any_nonzero = 1;
                }
            }
            if (!any_nonzero) fprintf(f, "(all zeros)");
            fprintf(f, "\n\n");

            mailbox_dump_count++;
        }
        fclose(f);
    }
    memcpy(mailbox_dump_prev, global_mmap_addr, MAILBOX_SIZE);
}
#endif /* SHADOW_DEBUG */

static void *shadow_dsp_handle = NULL;
static const plugin_api_v2_t *shadow_plugin_v2 = NULL;
static host_api_v1_t shadow_host_api;
static int shadow_inprocess_ready = 0;

/* Startup mod wheel reset countdown - resets mod wheel after Move finishes its startup MIDI burst */
#define STARTUP_MODWHEEL_RESET_FRAMES 20  /* ~0.6 seconds at 128 frames/block */
static int shadow_startup_modwheel_countdown = 0;

/* Deferred DSP rendering buffer - rendered post-ioctl, mixed pre-ioctl next frame */
static int16_t shadow_deferred_dsp_buffer[FRAMES_PER_BLOCK * 2];
static int shadow_deferred_dsp_valid = 0;

/* ==========================================================================
 * Shadow Capture Rules - Allow slots to capture specific MIDI controls
 * ========================================================================== */

/* Control group alias definitions */
#define CAPTURE_PADS_NOTE_MIN     68
#define CAPTURE_PADS_NOTE_MAX     99
#define CAPTURE_STEPS_NOTE_MIN    16
#define CAPTURE_STEPS_NOTE_MAX    31
#define CAPTURE_TRACKS_CC_MIN     40
#define CAPTURE_TRACKS_CC_MAX     43
#define CAPTURE_KNOBS_CC_MIN      71
#define CAPTURE_KNOBS_CC_MAX      78
#define CAPTURE_JOG_CC            14

/* Capture rules: bitmaps for which notes/CCs a slot captures */
typedef struct shadow_capture_rules_t {
    uint8_t notes[16];   /* bitmap: 128 notes, 16 bytes */
    uint8_t ccs[16];     /* bitmap: 128 CCs, 16 bytes */
} shadow_capture_rules_t;

/* Set a single bit in a capture bitmap */
static void capture_set_bit(uint8_t *bitmap, int index)
{
    if (index >= 0 && index < 128) {
        bitmap[index / 8] |= (1 << (index % 8));
    }
}

/* Set a range of bits in a capture bitmap */
static void capture_set_range(uint8_t *bitmap, int min, int max)
{
    for (int i = min; i <= max && i < 128; i++) {
        if (i >= 0) {
            capture_set_bit(bitmap, i);
        }
    }
}

/* Check if a bit is set in a capture bitmap */
static int capture_has_bit(const uint8_t *bitmap, int index)
{
    if (index >= 0 && index < 128) {
        return (bitmap[index / 8] >> (index % 8)) & 1;
    }
    return 0;
}

/* Check if a note is captured */
static int capture_has_note(const shadow_capture_rules_t *rules, uint8_t note)
{
    return capture_has_bit(rules->notes, note);
}

/* Check if a CC is captured */
static int capture_has_cc(const shadow_capture_rules_t *rules, uint8_t cc)
{
    return capture_has_bit(rules->ccs, cc);
}

/* Clear all capture rules */
static void capture_clear(shadow_capture_rules_t *rules)
{
    memset(rules->notes, 0, sizeof(rules->notes));
    memset(rules->ccs, 0, sizeof(rules->ccs));
}

/* Apply a named group alias to capture rules */
static void capture_apply_group(shadow_capture_rules_t *rules, const char *group)
{
    if (!group || !rules) return;

    if (strcmp(group, "pads") == 0) {
        capture_set_range(rules->notes, CAPTURE_PADS_NOTE_MIN, CAPTURE_PADS_NOTE_MAX);
    } else if (strcmp(group, "steps") == 0) {
        capture_set_range(rules->notes, CAPTURE_STEPS_NOTE_MIN, CAPTURE_STEPS_NOTE_MAX);
    } else if (strcmp(group, "tracks") == 0) {
        capture_set_range(rules->ccs, CAPTURE_TRACKS_CC_MIN, CAPTURE_TRACKS_CC_MAX);
    } else if (strcmp(group, "knobs") == 0) {
        capture_set_range(rules->ccs, CAPTURE_KNOBS_CC_MIN, CAPTURE_KNOBS_CC_MAX);
    } else if (strcmp(group, "jog") == 0) {
        capture_set_bit(rules->ccs, CAPTURE_JOG_CC);
    }
}

/* Parse capture rules from patch JSON.
 * Handles: groups, notes, note_ranges, ccs, cc_ranges */
static void capture_parse_json(shadow_capture_rules_t *rules, const char *json)
{
    if (!rules || !json) return;
    capture_clear(rules);

    /* Find "capture" object */
    const char *capture_start = strstr(json, "\"capture\"");
    if (!capture_start) return;

    const char *brace = strchr(capture_start, '{');
    if (!brace) return;

    /* Find matching closing brace (simple - no nested objects expected) */
    const char *end = strchr(brace, '}');
    if (!end) return;

    /* Parse "groups" array: ["steps", "pads"] */
    const char *groups = strstr(brace, "\"groups\"");
    if (groups && groups < end) {
        const char *arr_start = strchr(groups, '[');
        if (arr_start && arr_start < end) {
            const char *arr_end = strchr(arr_start, ']');
            if (arr_end && arr_end < end) {
                /* Extract each quoted string */
                const char *p = arr_start;
                while (p < arr_end) {
                    const char *q1 = strchr(p, '"');
                    if (!q1 || q1 >= arr_end) break;
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (!q2 || q2 >= arr_end) break;
                    
                    /* Extract group name */
                    char group[32];
                    size_t len = (size_t)(q2 - q1);
                    if (len < sizeof(group)) {
                        memcpy(group, q1, len);
                        group[len] = '\0';
                        capture_apply_group(rules, group);
                    }
                    p = q2 + 1;
                }
            }
        }
    }
    
    /* Parse "notes" array: [60, 61, 62] */
    const char *notes = strstr(brace, "\"notes\"");
    if (notes && notes < end) {
        const char *arr_start = strchr(notes, '[');
        if (arr_start && arr_start < end) {
            const char *arr_end = strchr(arr_start, ']');
            if (arr_end && arr_end < end) {
                const char *p = arr_start + 1;
                while (p < arr_end) {
                    while (p < arr_end && (*p == ' ' || *p == ',')) p++;
                    if (p >= arr_end) break;
                    int val = atoi(p);
                    if (val >= 0 && val < 128) {
                        capture_set_bit(rules->notes, val);
                    }
                    while (p < arr_end && *p != ',' && *p != ']') p++;
                }
            }
        }
    }
    
    /* Parse "note_ranges" array: [[68, 75], [80, 90]] */
    const char *note_ranges = strstr(brace, "\"note_ranges\"");
    if (note_ranges && note_ranges < end) {
        const char *arr_start = strchr(note_ranges, '[');
        if (arr_start && arr_start < end) {
            const char *arr_end = strchr(arr_start, ']');
            /* Find the outer closing bracket (skip inner arrays) */
            int depth = 1;
            const char *p = arr_start + 1;
            while (p < end && depth > 0) {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }
            arr_end = p - 1;
            
            /* Parse each [min, max] pair */
            p = arr_start + 1;
            while (p < arr_end) {
                const char *inner_start = strchr(p, '[');
                if (!inner_start || inner_start >= arr_end) break;
                const char *inner_end = strchr(inner_start, ']');
                if (!inner_end || inner_end >= arr_end) break;
                
                /* Parse two numbers */
                int min = -1, max = -1;
                const char *n = inner_start + 1;
                while (n < inner_end && (*n == ' ' || *n == ',')) n++;
                min = atoi(n);
                while (n < inner_end && *n != ',') n++;
                if (n < inner_end) {
                    n++;
                    while (n < inner_end && *n == ' ') n++;
                    max = atoi(n);
                }
                if (min >= 0 && max >= min && max < 128) {
                    capture_set_range(rules->notes, min, max);
                }
                p = inner_end + 1;
            }
        }
    }
    
    /* Parse "ccs" array: [118, 119] */
    const char *ccs = strstr(brace, "\"ccs\"");
    if (ccs && ccs < end) {
        const char *arr_start = strchr(ccs, '[');
        if (arr_start && arr_start < end) {
            const char *arr_end = strchr(arr_start, ']');
            if (arr_end && arr_end < end) {
                const char *p = arr_start + 1;
                while (p < arr_end) {
                    while (p < arr_end && (*p == ' ' || *p == ',')) p++;
                    if (p >= arr_end) break;
                    int val = atoi(p);
                    if (val >= 0 && val < 128) {
                        capture_set_bit(rules->ccs, val);
                    }
                    while (p < arr_end && *p != ',' && *p != ']') p++;
                }
            }
        }
    }
    
    /* Parse "cc_ranges" array: [[100, 110]] */
    const char *cc_ranges = strstr(brace, "\"cc_ranges\"");
    if (cc_ranges && cc_ranges < end) {
        const char *arr_start = strchr(cc_ranges, '[');
        if (arr_start && arr_start < end) {
            /* Find the outer closing bracket */
            int depth = 1;
            const char *p = arr_start + 1;
            while (p < end && depth > 0) {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }
            const char *arr_end = p - 1;
            
            /* Parse each [min, max] pair */
            p = arr_start + 1;
            while (p < arr_end) {
                const char *inner_start = strchr(p, '[');
                if (!inner_start || inner_start >= arr_end) break;
                const char *inner_end = strchr(inner_start, ']');
                if (!inner_end || inner_end >= arr_end) break;
                
                int min = -1, max = -1;
                const char *n = inner_start + 1;
                while (n < inner_end && (*n == ' ' || *n == ',')) n++;
                min = atoi(n);
                while (n < inner_end && *n != ',') n++;
                if (n < inner_end) {
                    n++;
                    while (n < inner_end && *n == ' ') n++;
                    max = atoi(n);
                }
                if (min >= 0 && max >= min && max < 128) {
                    capture_set_range(rules->ccs, min, max);
                }
                p = inner_end + 1;
            }
        }
    }
}

typedef struct shadow_chain_slot_t {
    void *instance;
    int channel;
    int patch_index;
    int active;
    float volume;           /* 0.0 to 1.0, applied to audio output */
    int forward_channel;    /* -1 = none, 0-15 = forward MIDI to this channel */
    char patch_name[64];
    shadow_capture_rules_t capture;  /* MIDI controls this slot captures when focused */
} shadow_chain_slot_t;;

static shadow_chain_slot_t shadow_chain_slots[SHADOW_CHAIN_INSTANCES];

static const char *shadow_chain_default_patches[SHADOW_CHAIN_INSTANCES] = {
    "",  /* No default patch - user must select */
    "",
    "",
    ""
};

/* Master FX chain - 4 FX slots that process mixed shadow audio output */
#define MASTER_FX_SLOTS 4

typedef struct {
    void *handle;                    /* dlopen handle */
    audio_fx_api_v2_t *api;          /* FX API pointer */
    void *instance;                  /* FX instance */
    char module_path[256];           /* Full DSP path */
    char module_id[64];              /* Module ID for display */
    shadow_capture_rules_t capture;  /* Capture rules for this FX */
    char chain_params_cache[2048];   /* Cached chain_params to avoid file I/O in audio thread */
    int chain_params_cached;         /* 1 if cache is valid */
} master_fx_slot_t;

static master_fx_slot_t shadow_master_fx_slots[MASTER_FX_SLOTS];

/* Legacy single-slot pointers for backward compatibility during transition */
#define shadow_master_fx_handle (shadow_master_fx_slots[0].handle)
#define shadow_master_fx (shadow_master_fx_slots[0].api)
#define shadow_master_fx_instance (shadow_master_fx_slots[0].instance)
#define shadow_master_fx_module (shadow_master_fx_slots[0].module_path)
#define shadow_master_fx_capture (shadow_master_fx_slots[0].capture)

/* ==========================================================================
 * D-Bus Volume Sync - Monitor Move's track volume via accessibility D-Bus
 * ========================================================================== */

/* Forward declarations */
static void shadow_log(const char *msg);
static void shadow_save_state(void);

/* Track button hold state for volume sync: -1 = none held, 0-3 = track 1-4 */
static volatile int shadow_held_track = -1;

/* Selected slot for Shift+Knob routing: 0-3, persists even when shadow UI is off */
static volatile int shadow_selected_slot = 0;

/* D-Bus connection for monitoring */
static DBusConnection *shadow_dbus_conn = NULL;
static pthread_t shadow_dbus_thread;
static volatile int shadow_dbus_running = 0;

/* Parse dB value from "Track Volume X dB" string and convert to linear */
static float shadow_parse_volume_db(const char *text)
{
    /* Format: "Track Volume X dB" or "Track Volume -inf dB" */
    if (!text) return -1.0f;

    const char *prefix = "Track Volume ";
    if (strncmp(text, prefix, strlen(prefix)) != 0) return -1.0f;

    const char *val_start = text + strlen(prefix);

    /* Handle -inf dB */
    if (strncmp(val_start, "-inf", 4) == 0) {
        return 0.0f;
    }

    /* Parse dB value */
    float db = strtof(val_start, NULL);

    /* Convert dB to linear: 10^(dB/20) */
    float linear = powf(10.0f, db / 20.0f);

    /* Clamp to reasonable range */
    if (linear < 0.0f) linear = 0.0f;
    if (linear > 4.0f) linear = 4.0f;  /* +12 dB max */

    return linear;
}

/* Handle a screenreader text signal */
static void shadow_dbus_handle_text(const char *text)
{
    if (!text || !text[0]) return;

    /* Debug: log all D-Bus text messages */
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "D-Bus text: \"%s\" (held_track=%d)", text, shadow_held_track);
        shadow_log(msg);
    }

    /* Check if it's a volume message */
    if (strncmp(text, "Track Volume ", 13) == 0) {
        float volume = shadow_parse_volume_db(text);
        if (volume >= 0.0f && shadow_held_track >= 0 && shadow_held_track < SHADOW_CHAIN_INSTANCES) {
            /* Update the held track's slot volume */
            shadow_chain_slots[shadow_held_track].volume = volume;

            /* Log the volume sync */
            char msg[128];
            snprintf(msg, sizeof(msg), "D-Bus volume sync: slot %d = %.3f (%s)",
                     shadow_held_track, volume, text);
            shadow_log(msg);

            /* Persist slot volumes */
            shadow_save_state();
        }
    }
}

/* D-Bus filter function to receive signals */
static DBusHandlerResult shadow_dbus_filter(DBusConnection *conn, DBusMessage *msg, void *data)
{
    (void)conn;
    (void)data;

    if (dbus_message_is_signal(msg, "com.ableton.move.ScreenReader", "text")) {
        DBusMessageIter args;
        if (dbus_message_iter_init(msg, &args)) {
            if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
                const char *text = NULL;
                dbus_message_iter_get_basic(&args, &text);
                shadow_dbus_handle_text(text);
            }
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* D-Bus monitoring thread */
static void *shadow_dbus_thread_func(void *arg)
{
    (void)arg;

    DBusError err;
    dbus_error_init(&err);

    /* Connect to system bus */
    shadow_dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
        shadow_log("D-Bus: Failed to connect to system bus");
        dbus_error_free(&err);
        return NULL;
    }

    if (!shadow_dbus_conn) {
        shadow_log("D-Bus: Connection is NULL");
        return NULL;
    }

    /* Subscribe to screenreader signals */
    const char *rule = "type='signal',interface='com.ableton.move.ScreenReader',member='text'";
    dbus_bus_add_match(shadow_dbus_conn, rule, &err);
    dbus_connection_flush(shadow_dbus_conn);

    if (dbus_error_is_set(&err)) {
        shadow_log("D-Bus: Failed to add match rule");
        dbus_error_free(&err);
        return NULL;
    }

    /* Add message filter */
    if (!dbus_connection_add_filter(shadow_dbus_conn, shadow_dbus_filter, NULL, NULL)) {
        shadow_log("D-Bus: Failed to add filter");
        return NULL;
    }

    shadow_log("D-Bus: Connected and listening for screenreader signals");

    /* Main loop - process D-Bus messages */
    while (shadow_dbus_running) {
        /* Non-blocking read with timeout */
        dbus_connection_read_write(shadow_dbus_conn, 100);  /* 100ms timeout */

        /* Dispatch any pending messages */
        while (dbus_connection_dispatch(shadow_dbus_conn) == DBUS_DISPATCH_DATA_REMAINS) {
            /* Keep dispatching */
        }
    }

    shadow_log("D-Bus: Thread exiting");
    return NULL;
}

/* Start D-Bus monitoring thread */
static void shadow_dbus_start(void)
{
    if (shadow_dbus_running) return;

    shadow_dbus_running = 1;
    if (pthread_create(&shadow_dbus_thread, NULL, shadow_dbus_thread_func, NULL) != 0) {
        shadow_log("D-Bus: Failed to create thread");
        shadow_dbus_running = 0;
    }
}

/* Stop D-Bus monitoring thread */
static void shadow_dbus_stop(void)
{
    if (!shadow_dbus_running) return;

    shadow_dbus_running = 0;
    pthread_join(shadow_dbus_thread, NULL);

    if (shadow_dbus_conn) {
        dbus_connection_unref(shadow_dbus_conn);
        shadow_dbus_conn = NULL;
    }
}

/* Update track button hold state from MIDI (called from ioctl hook) */
static void shadow_update_held_track(uint8_t cc, int pressed)
{
    /* Track buttons are CCs 40-43, but in reverse order:
     * CC 43 = Track 1 → slot 0
     * CC 42 = Track 2 → slot 1
     * CC 41 = Track 3 → slot 2
     * CC 40 = Track 4 → slot 3 */
    if (cc >= 40 && cc <= 43) {
        int slot = 43 - cc;  /* Reverse: CC43→0, CC42→1, CC41→2, CC40→3 */
        int old_held = shadow_held_track;
        if (pressed) {
            shadow_held_track = slot;
        } else if (shadow_held_track == slot) {
            shadow_held_track = -1;
        }
        /* Log state changes */
        if (shadow_held_track != old_held) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Track button: CC%d (track %d) %s -> held_track=%d",
                     cc, 4 - (cc - 40), pressed ? "pressed" : "released", shadow_held_track);
            shadow_log(msg);
        }
    }
}

/* ==========================================================================
 * Master Volume Sync - Read from display buffer when volume overlay shown
 * ========================================================================== */

/* Master volume for all shadow audio output (0.0 - 1.0) */
static volatile float shadow_master_volume = 1.0f;
/* Is volume knob currently being touched? (note 8) */
static volatile int shadow_volume_knob_touched = 0;
/* Is shift button currently held? (CC 49) - global for cross-function access */
static volatile int shadow_shift_held = 0;

/* ==========================================================================
 * Shift+Knob Overlay - Show parameter overlay on Move's display
 * ========================================================================== */

/* Overlay state for Shift+Knob in Move mode */
static int shift_knob_overlay_active = 0;
static int shift_knob_overlay_timeout = 0;  /* Frames until overlay disappears */
static int shift_knob_overlay_slot = 0;     /* Which slot is being adjusted */
static int shift_knob_overlay_knob = 0;     /* Which knob (1-8) */
static char shift_knob_overlay_patch[64] = "";   /* Patch name */
static char shift_knob_overlay_param[64] = "";   /* Parameter name */
static char shift_knob_overlay_value[32] = "";   /* Parameter value */

/* Config: enable Shift+Knob in Move mode */
static int shift_knob_enabled = 1;  /* Default enabled */

#define SHIFT_KNOB_OVERLAY_FRAMES 60  /* ~1 second at 60fps */

/* Minimal 5x7 font for overlay text (ASCII 32-127) */
static const uint8_t overlay_font_5x7[96][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 32 space */
    {0x04,0x04,0x04,0x04,0x00,0x04,0x00}, /* 33 ! */
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, /* 34 " */
    {0x0A,0x1F,0x0A,0x1F,0x0A,0x00,0x00}, /* 35 # */
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* 36 $ */
    {0x19,0x1A,0x04,0x0B,0x13,0x00,0x00}, /* 37 % */
    {0x08,0x14,0x08,0x15,0x12,0x0D,0x00}, /* 38 & */
    {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, /* 39 ' */
    {0x02,0x04,0x04,0x04,0x04,0x02,0x00}, /* 40 ( */
    {0x08,0x04,0x04,0x04,0x04,0x08,0x00}, /* 41 ) */
    {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00}, /* 42 * */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* 43 + */
    {0x00,0x00,0x00,0x00,0x04,0x04,0x08}, /* 44 , */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* 45 - */
    {0x00,0x00,0x00,0x00,0x00,0x04,0x00}, /* 46 . */
    {0x01,0x02,0x04,0x08,0x10,0x00,0x00}, /* 47 / */
    {0x0E,0x11,0x13,0x15,0x19,0x0E,0x00}, /* 48 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x0E,0x00}, /* 49 1 */
    {0x0E,0x11,0x01,0x06,0x08,0x1F,0x00}, /* 50 2 */
    {0x0E,0x11,0x02,0x01,0x11,0x0E,0x00}, /* 51 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x00}, /* 52 4 */
    {0x1F,0x10,0x1E,0x01,0x11,0x0E,0x00}, /* 53 5 */
    {0x06,0x08,0x1E,0x11,0x11,0x0E,0x00}, /* 54 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x00}, /* 55 7 */
    {0x0E,0x11,0x0E,0x11,0x11,0x0E,0x00}, /* 56 8 */
    {0x0E,0x11,0x11,0x0F,0x02,0x0C,0x00}, /* 57 9 */
    {0x00,0x04,0x00,0x00,0x04,0x00,0x00}, /* 58 : */
    {0x00,0x04,0x00,0x00,0x04,0x08,0x00}, /* 59 ; */
    {0x02,0x04,0x08,0x04,0x02,0x00,0x00}, /* 60 < */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* 61 = */
    {0x08,0x04,0x02,0x04,0x08,0x00,0x00}, /* 62 > */
    {0x0E,0x11,0x02,0x04,0x00,0x04,0x00}, /* 63 ? */
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, /* 64 @ */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x00}, /* 65 A */
    {0x1E,0x11,0x1E,0x11,0x11,0x1E,0x00}, /* 66 B */
    {0x0E,0x11,0x10,0x10,0x11,0x0E,0x00}, /* 67 C */
    {0x1E,0x11,0x11,0x11,0x11,0x1E,0x00}, /* 68 D */
    {0x1F,0x10,0x1E,0x10,0x10,0x1F,0x00}, /* 69 E */
    {0x1F,0x10,0x1E,0x10,0x10,0x10,0x00}, /* 70 F */
    {0x0E,0x11,0x10,0x13,0x11,0x0F,0x00}, /* 71 G */
    {0x11,0x11,0x1F,0x11,0x11,0x11,0x00}, /* 72 H */
    {0x0E,0x04,0x04,0x04,0x04,0x0E,0x00}, /* 73 I */
    {0x01,0x01,0x01,0x01,0x11,0x0E,0x00}, /* 74 J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* 75 K */
    {0x10,0x10,0x10,0x10,0x10,0x1F,0x00}, /* 76 L */
    {0x11,0x1B,0x15,0x11,0x11,0x11,0x00}, /* 77 M */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x00}, /* 78 N */
    {0x0E,0x11,0x11,0x11,0x11,0x0E,0x00}, /* 79 O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x00}, /* 80 P */
    {0x0E,0x11,0x11,0x15,0x12,0x0D,0x00}, /* 81 Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x00}, /* 82 R */
    {0x0E,0x11,0x10,0x0E,0x01,0x1E,0x00}, /* 83 S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x00}, /* 84 T */
    {0x11,0x11,0x11,0x11,0x11,0x0E,0x00}, /* 85 U */
    {0x11,0x11,0x11,0x0A,0x0A,0x04,0x00}, /* 86 V */
    {0x11,0x11,0x15,0x15,0x0A,0x0A,0x00}, /* 87 W */
    {0x11,0x0A,0x04,0x04,0x0A,0x11,0x00}, /* 88 X */
    {0x11,0x0A,0x04,0x04,0x04,0x04,0x00}, /* 89 Y */
    {0x1F,0x01,0x02,0x04,0x08,0x1F,0x00}, /* 90 Z */
    {0x0E,0x08,0x08,0x08,0x08,0x0E,0x00}, /* 91 [ */
    {0x10,0x08,0x04,0x02,0x01,0x00,0x00}, /* 92 \ */
    {0x0E,0x02,0x02,0x02,0x02,0x0E,0x00}, /* 93 ] */
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* 94 ^ */
    {0x00,0x00,0x00,0x00,0x00,0x1F,0x00}, /* 95 _ */
    {0x08,0x04,0x00,0x00,0x00,0x00,0x00}, /* 96 ` */
    {0x00,0x0E,0x01,0x0F,0x11,0x0F,0x00}, /* 97 a */
    {0x10,0x10,0x1E,0x11,0x11,0x1E,0x00}, /* 98 b */
    {0x00,0x0E,0x10,0x10,0x11,0x0E,0x00}, /* 99 c */
    {0x01,0x01,0x0F,0x11,0x11,0x0F,0x00}, /* 100 d */
    {0x00,0x0E,0x11,0x1F,0x10,0x0E,0x00}, /* 101 e */
    {0x06,0x08,0x1C,0x08,0x08,0x08,0x00}, /* 102 f */
    {0x00,0x0F,0x11,0x0F,0x01,0x0E,0x00}, /* 103 g */
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x00}, /* 104 h */
    {0x04,0x00,0x0C,0x04,0x04,0x0E,0x00}, /* 105 i */
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, /* 106 j */
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, /* 107 k */
    {0x0C,0x04,0x04,0x04,0x04,0x0E,0x00}, /* 108 l */
    {0x00,0x1A,0x15,0x15,0x11,0x11,0x00}, /* 109 m */
    {0x00,0x1E,0x11,0x11,0x11,0x11,0x00}, /* 110 n */
    {0x00,0x0E,0x11,0x11,0x11,0x0E,0x00}, /* 111 o */
    {0x00,0x1E,0x11,0x1E,0x10,0x10,0x00}, /* 112 p */
    {0x00,0x0F,0x11,0x0F,0x01,0x01,0x00}, /* 113 q */
    {0x00,0x16,0x19,0x10,0x10,0x10,0x00}, /* 114 r */
    {0x00,0x0E,0x10,0x0E,0x01,0x1E,0x00}, /* 115 s */
    {0x08,0x1C,0x08,0x08,0x09,0x06,0x00}, /* 116 t */
    {0x00,0x11,0x11,0x11,0x13,0x0D,0x00}, /* 117 u */
    {0x00,0x11,0x11,0x0A,0x0A,0x04,0x00}, /* 118 v */
    {0x00,0x11,0x11,0x15,0x15,0x0A,0x00}, /* 119 w */
    {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}, /* 120 x */
    {0x00,0x11,0x11,0x0F,0x01,0x0E,0x00}, /* 121 y */
    {0x00,0x1F,0x02,0x04,0x08,0x1F,0x00}, /* 122 z */
    {0x02,0x04,0x08,0x04,0x02,0x00,0x00}, /* 123 { */
    {0x04,0x04,0x04,0x04,0x04,0x04,0x00}, /* 124 | */
    {0x08,0x04,0x02,0x04,0x08,0x00,0x00}, /* 125 } */
    {0x00,0x08,0x15,0x02,0x00,0x00,0x00}, /* 126 ~ */
    {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F}, /* 127 DEL (solid block) */
};

/* Draw a character to column-major display buffer at (x, y) */
static void overlay_draw_char(uint8_t *buf, int x, int y, char c, int color)
{
    if (c < 32 || c > 127) c = '?';
    const uint8_t *glyph = overlay_font_5x7[c - 32];

    for (int row = 0; row < 7; row++) {
        int screen_y = y + row;
        if (screen_y < 0 || screen_y >= 64) continue;

        int page = screen_y / 8;
        int bit = screen_y % 8;

        for (int col = 0; col < 5; col++) {
            int screen_x = x + col;
            if (screen_x < 0 || screen_x >= 128) continue;

            int byte_idx = page * 128 + screen_x;
            int pixel_on = (glyph[row] >> (4 - col)) & 1;

            if (pixel_on) {
                if (color)
                    buf[byte_idx] |= (1 << bit);   /* Set pixel */
                else
                    buf[byte_idx] &= ~(1 << bit);  /* Clear pixel */
            }
        }
    }
}

/* Draw a string to column-major display buffer */
static void overlay_draw_string(uint8_t *buf, int x, int y, const char *str, int color)
{
    while (*str) {
        overlay_draw_char(buf, x, y, *str, color);
        x += 6;  /* 5 pixel width + 1 pixel spacing */
        str++;
    }
}

/* Draw a filled rectangle (for overlay background) */
static void overlay_fill_rect(uint8_t *buf, int x, int y, int w, int h, int color)
{
    for (int row = y; row < y + h && row < 64; row++) {
        if (row < 0) continue;
        int page = row / 8;
        int bit = row % 8;

        for (int col = x; col < x + w && col < 128; col++) {
            if (col < 0) continue;
            int byte_idx = page * 128 + col;

            if (color)
                buf[byte_idx] |= (1 << bit);
            else
                buf[byte_idx] &= ~(1 << bit);
        }
    }
}

/* Draw the shift+knob overlay onto a display buffer */
static void overlay_draw_shift_knob(uint8_t *buf)
{
    if (!shift_knob_overlay_active || shift_knob_overlay_timeout <= 0) return;

    /* Box dimensions: 3 lines of text + padding */
    int box_w = 100;
    int box_h = 30;
    int box_x = (128 - box_w) / 2;
    int box_y = (64 - box_h) / 2;

    /* Draw background (black) and border (white) */
    overlay_fill_rect(buf, box_x, box_y, box_w, box_h, 0);
    overlay_fill_rect(buf, box_x, box_y, box_w, 1, 1);           /* Top border */
    overlay_fill_rect(buf, box_x, box_y + box_h - 1, box_w, 1, 1); /* Bottom border */
    overlay_fill_rect(buf, box_x, box_y, 1, box_h, 1);           /* Left border */
    overlay_fill_rect(buf, box_x + box_w - 1, box_y, 1, box_h, 1); /* Right border */

    /* Draw text lines */
    int text_x = box_x + 4;
    int text_y = box_y + 3;

    overlay_draw_string(buf, text_x, text_y, shift_knob_overlay_patch, 1);
    overlay_draw_string(buf, text_x, text_y + 9, shift_knob_overlay_param, 1);
    overlay_draw_string(buf, text_x, text_y + 18, shift_knob_overlay_value, 1);
}

/* Update overlay state when a knob CC is processed in Move mode with Shift held */
static void shift_knob_update_overlay(int slot, int knob_num, uint8_t cc_value)
{
    (void)cc_value;  /* No longer used - we show "Unmapped" instead */
    if (!shift_knob_enabled) return;
    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) return;

    shift_knob_overlay_slot = slot;
    shift_knob_overlay_knob = knob_num;  /* 1-8 */
    shift_knob_overlay_active = 1;
    shift_knob_overlay_timeout = SHIFT_KNOB_OVERLAY_FRAMES;

    /* Copy slot name with "S#: " prefix */
    const char *name = shadow_chain_slots[slot].patch_name;
    if (name[0] == '\0') {
        snprintf(shift_knob_overlay_patch, sizeof(shift_knob_overlay_patch),
                 "S%d", slot + 1);
    } else {
        snprintf(shift_knob_overlay_patch, sizeof(shift_knob_overlay_patch),
                 "S%d: %s", slot + 1, name);
    }

    /* Query parameter name and value from DSP */
    int mapped = 0;
    if (shadow_plugin_v2 && shadow_plugin_v2->get_param && shadow_chain_slots[slot].instance) {
        char key[32];
        char buf[64];
        int len;

        /* Get knob_N_name - if this succeeds, the knob is mapped */
        snprintf(key, sizeof(key), "knob_%d_name", knob_num);
        len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance, key, buf, sizeof(buf));
        if (len > 0) {
            mapped = 1;
            buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
            strncpy(shift_knob_overlay_param, buf, sizeof(shift_knob_overlay_param) - 1);
            shift_knob_overlay_param[sizeof(shift_knob_overlay_param) - 1] = '\0';

            /* Get knob_N_value */
            snprintf(key, sizeof(key), "knob_%d_value", knob_num);
            len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance, key, buf, sizeof(buf));
            if (len > 0) {
                buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
                strncpy(shift_knob_overlay_value, buf, sizeof(shift_knob_overlay_value) - 1);
                shift_knob_overlay_value[sizeof(shift_knob_overlay_value) - 1] = '\0';
            } else {
                strncpy(shift_knob_overlay_value, "?", sizeof(shift_knob_overlay_value) - 1);
            }
        }
    }

    /* Show "Unmapped" if knob has no mapping */
    if (!mapped) {
        snprintf(shift_knob_overlay_param, sizeof(shift_knob_overlay_param), "Knob %d", knob_num);
        strncpy(shift_knob_overlay_value, "Unmapped", sizeof(shift_knob_overlay_value) - 1);
        shift_knob_overlay_value[sizeof(shift_knob_overlay_value) - 1] = '\0';
    }
}

/* Read initial volume from Move's Settings.json */
static void shadow_read_initial_volume(void)
{
    FILE *f = fopen("/data/UserData/settings/Settings.json", "r");
    if (!f) {
        shadow_log("Master volume: Settings.json not found, defaulting to 1.0");
        return;
    }

    /* Read file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 8192) {
        fclose(f);
        return;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return;
    }

    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);

    /* Find "globalVolume": X.X */
    const char *key = "\"globalVolume\":";
    char *pos = strstr(json, key);
    if (pos) {
        pos += strlen(key);
        while (*pos == ' ') pos++;
        float db = strtof(pos, NULL);
        /* globalVolume is in dB, convert to linear */
        /* 0 dB = 1.0, -inf = 0.0 */
        if (db <= -60.0f) {
            shadow_master_volume = 0.0f;
        } else {
            shadow_master_volume = powf(10.0f, db / 20.0f);
            if (shadow_master_volume > 1.0f) shadow_master_volume = 1.0f;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Master volume: read %.1f dB -> %.3f linear", db, shadow_master_volume);
        shadow_log(msg);
    }

    free(json);
}

/* ==========================================================================
 * Shadow State Persistence - Save/load slot volumes to shadow_chain_config.json
 * ========================================================================== */

#define SHADOW_CONFIG_PATH "/data/UserData/move-anything/shadow_chain_config.json"

static void shadow_save_state(void)
{
    /* Read existing config to preserve patches and master_fx fields */
    FILE *f = fopen(SHADOW_CONFIG_PATH, "r");
    char patches_buf[4096] = "";
    char master_fx[256] = "";
    char master_fx_path[256] = "";

    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (size > 0 && size < 8192) {
            char *json = malloc(size + 1);
            if (json) {
                fread(json, 1, size, f);
                json[size] = '\0';

                /* Extract patches array (preserve as-is) */
                char *patches_start = strstr(json, "\"patches\":");
                if (patches_start) {
                    char *arr_start = strchr(patches_start, '[');
                    if (arr_start) {
                        int depth = 1;
                        char *arr_end = arr_start + 1;
                        while (*arr_end && depth > 0) {
                            if (*arr_end == '[') depth++;
                            else if (*arr_end == ']') depth--;
                            arr_end++;
                        }
                        int len = arr_end - arr_start;
                        if (len < (int)sizeof(patches_buf) - 1) {
                            strncpy(patches_buf, arr_start, len);
                            patches_buf[len] = '\0';
                        }
                    }
                }

                /* Extract master_fx string */
                char *mfx = strstr(json, "\"master_fx\":");
                if (mfx) {
                    mfx = strchr(mfx, ':');
                    if (mfx) {
                        mfx++;
                        while (*mfx == ' ' || *mfx == '"') mfx++;
                        char *end = mfx;
                        while (*end && *end != '"' && *end != ',' && *end != '\n') end++;
                        int len = end - mfx;
                        if (len < (int)sizeof(master_fx) - 1) {
                            strncpy(master_fx, mfx, len);
                            master_fx[len] = '\0';
                        }
                    }
                }

                /* Extract master_fx_path string */
                char *mfxp = strstr(json, "\"master_fx_path\":");
                if (mfxp) {
                    mfxp = strchr(mfxp, ':');
                    if (mfxp) {
                        mfxp++;
                        while (*mfxp == ' ' || *mfxp == '"') mfxp++;
                        char *end = mfxp;
                        while (*end && *end != '"' && *end != ',' && *end != '\n') end++;
                        int len = end - mfxp;
                        if (len < (int)sizeof(master_fx_path) - 1) {
                            strncpy(master_fx_path, mfxp, len);
                            master_fx_path[len] = '\0';
                        }
                    }
                }

                free(json);
            }
        }
        fclose(f);
    }

    /* Write complete config file */
    f = fopen(SHADOW_CONFIG_PATH, "w");
    if (!f) {
        shadow_log("shadow_save_state: failed to open for writing");
        return;
    }

    fprintf(f, "{\n");
    if (patches_buf[0]) {
        fprintf(f, "  \"patches\": %s,\n", patches_buf);
    }
    fprintf(f, "  \"master_fx\": \"%s\",\n", master_fx);
    if (master_fx_path[0]) {
        fprintf(f, "  \"master_fx_path\": \"%s\",\n", master_fx_path);
    }
    fprintf(f, "  \"slot_volumes\": [%.3f, %.3f, %.3f, %.3f]\n",
            shadow_chain_slots[0].volume,
            shadow_chain_slots[1].volume,
            shadow_chain_slots[2].volume,
            shadow_chain_slots[3].volume);
    fprintf(f, "}\n");
    fclose(f);

    char msg[128];
    snprintf(msg, sizeof(msg), "Saved slot volumes: [%.2f, %.2f, %.2f, %.2f]",
             shadow_chain_slots[0].volume,
             shadow_chain_slots[1].volume,
             shadow_chain_slots[2].volume,
             shadow_chain_slots[3].volume);
    shadow_log(msg);
}

static void shadow_load_state(void)
{
    FILE *f = fopen(SHADOW_CONFIG_PATH, "r");
    if (!f) {
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 8192) {
        fclose(f);
        return;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return;
    }

    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);

    /* Parse slot_volumes array */
    const char *key = "\"slot_volumes\":";
    char *pos = strstr(json, key);
    if (pos) {
        pos = strchr(pos, '[');
        if (pos) {
            float v0, v1, v2, v3;
            if (sscanf(pos, "[%f, %f, %f, %f]", &v0, &v1, &v2, &v3) == 4) {
                shadow_chain_slots[0].volume = v0;
                shadow_chain_slots[1].volume = v1;
                shadow_chain_slots[2].volume = v2;
                shadow_chain_slots[3].volume = v3;

                char msg[128];
                snprintf(msg, sizeof(msg), "Loaded slot volumes: [%.2f, %.2f, %.2f, %.2f]",
                         v0, v1, v2, v3);
                shadow_log(msg);
            }
        }
    }

    free(json);
}

/* Parse volume from display buffer (vertical line position)
 * The volume overlay is a vertical line that moves left-to-right.
 * We scan the middle row (row 32) for a white pixel.
 * X position (0-127) maps to volume. */

static int shadow_chain_parse_channel(int ch) {
    /* Config uses 1-based MIDI channels; convert to 0-based for status nibble. */
    if (ch >= 1 && ch <= 16) {
        return ch - 1;
    }
    return ch;
}

static int shadow_inprocess_log_enabled(void) {
    static int enabled = -1;
    static int check_counter = 0;
    if (enabled < 0 || (check_counter++ % 200 == 0)) {
        enabled = (access("/data/UserData/move-anything/shadow_inprocess_log_on", F_OK) == 0);
    }
    return enabled;
}

static void shadow_log(const char *msg) {
    if (!shadow_inprocess_log_enabled()) return;
    FILE *log = fopen("/data/UserData/move-anything/shadow_inprocess.log", "a");
    if (log) {
        fprintf(log, "%s\n", msg ? msg : "(null)");
        fclose(log);
    }
}

static FILE *shadow_midi_out_log = NULL;

static int shadow_midi_out_log_enabled(void)
{
    static int enabled = 0;
    static int announced = 0;
    enabled = (access("/data/UserData/move-anything/shadow_midi_out_log_on", F_OK) == 0);
    if (!enabled && shadow_midi_out_log) {
        fclose(shadow_midi_out_log);
        shadow_midi_out_log = NULL;
    }
    if (enabled && !announced) {
        shadow_log("shadow_midi_out_log enabled");
        announced = 1;
    }
    return enabled;
}

static void shadow_midi_out_logf(const char *fmt, ...)
{
    if (!shadow_midi_out_log_enabled()) return;
    if (!shadow_midi_out_log) {
        shadow_midi_out_log = fopen("/data/UserData/move-anything/shadow_midi_out.log", "a");
        if (!shadow_midi_out_log) return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(shadow_midi_out_log, fmt, args);
    va_end(args);
    fputc('\n', shadow_midi_out_log);
    fflush(shadow_midi_out_log);
}

static void shadow_chain_defaults(void) {
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        shadow_chain_slots[i].instance = NULL;
        shadow_chain_slots[i].active = 0;
        shadow_chain_slots[i].patch_index = -1;
        shadow_chain_slots[i].channel = shadow_chain_parse_channel(1 + i);
        shadow_chain_slots[i].volume = 1.0f;
        shadow_chain_slots[i].forward_channel = -1;
        capture_clear(&shadow_chain_slots[i].capture);
        strncpy(shadow_chain_slots[i].patch_name,
                shadow_chain_default_patches[i],
                sizeof(shadow_chain_slots[i].patch_name) - 1);
        shadow_chain_slots[i].patch_name[sizeof(shadow_chain_slots[i].patch_name) - 1] = '\0';
    }
    /* Clear all master FX slots */
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        memset(&shadow_master_fx_slots[i], 0, sizeof(master_fx_slot_t));
    }
}

static void shadow_ui_state_update_slot(int slot) {
    if (!shadow_ui_state) return;
    if (slot < 0 || slot >= SHADOW_UI_SLOTS) return;
    shadow_ui_state->slot_channels[slot] = (uint8_t)(shadow_chain_slots[slot].channel + 1);
    shadow_ui_state->slot_volumes[slot] = (uint8_t)(shadow_chain_slots[slot].volume * 100.0f);
    shadow_ui_state->slot_forward_ch[slot] = (int8_t)shadow_chain_slots[slot].forward_channel;
    strncpy(shadow_ui_state->slot_names[slot],
            shadow_chain_slots[slot].patch_name,
            SHADOW_UI_NAME_LEN - 1);
    shadow_ui_state->slot_names[slot][SHADOW_UI_NAME_LEN - 1] = '\0';
}

static void shadow_ui_state_refresh(void) {
    if (!shadow_ui_state) return;
    shadow_ui_state->slot_count = SHADOW_UI_SLOTS;
    for (int i = 0; i < SHADOW_UI_SLOTS; i++) {
        shadow_ui_state_update_slot(i);
    }
}

static void shadow_chain_load_config(void) {
    shadow_chain_defaults();

    FILE *f = fopen(SHADOW_CHAIN_CONFIG_PATH, "r");
    if (!f) {
        shadow_ui_state_refresh();
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 4096) {
        fclose(f);
        shadow_ui_state_refresh();
        return;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        shadow_ui_state_refresh();
        return;
    }

    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);

    char *cursor = json;
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        char *name_pos = strstr(cursor, "\"name\"");
        if (!name_pos) break;
        char *colon = strchr(name_pos, ':');
        if (colon) {
            char *q1 = strchr(colon, '"');
            if (q1) {
                q1++;
                char *q2 = strchr(q1, '"');
                if (q2 && q2 > q1) {
                    size_t len = (size_t)(q2 - q1);
                    if (len < sizeof(shadow_chain_slots[i].patch_name)) {
                        memcpy(shadow_chain_slots[i].patch_name, q1, len);
                        shadow_chain_slots[i].patch_name[len] = '\0';
                    }
                }
            }
        }

        char *chan_pos = strstr(name_pos, "\"channel\"");
        if (chan_pos) {
            char *chan_colon = strchr(chan_pos, ':');
            if (chan_colon) {
                int ch = atoi(chan_colon + 1);
                if (ch >= 1 && ch <= 16) {
                    shadow_chain_slots[i].channel = shadow_chain_parse_channel(ch);
                }
            }
            cursor = chan_pos + 8;
        } else {
            cursor = name_pos + 6;
        }

        /* Parse volume (0.0 - 1.0) */
        char *vol_pos = strstr(name_pos, "\"volume\"");
        if (vol_pos) {
            char *vol_colon = strchr(vol_pos, ':');
            if (vol_colon) {
                float vol = atof(vol_colon + 1);
                if (vol >= 0.0f && vol <= 1.0f) {
                    shadow_chain_slots[i].volume = vol;
                }
            }
        }

        /* Parse forward_channel (-1 = none, 1-16 = channel) */
        char *fwd_pos = strstr(name_pos, "\"forward_channel\"");
        if (fwd_pos) {
            char *fwd_colon = strchr(fwd_pos, ':');
            if (fwd_colon) {
                int ch = atoi(fwd_colon + 1);
                if (ch >= -1 && ch <= 16) {
                    shadow_chain_slots[i].forward_channel = (ch > 0) ? ch - 1 : -1;
                }
            }
        }
    }

    free(json);
    shadow_ui_state_refresh();
}

static int shadow_chain_find_patch_index(void *instance, const char *name) {
    if (!shadow_plugin_v2 || !shadow_plugin_v2->get_param || !instance || !name || !name[0]) {
        return -1;
    }
    char buf[128];
    int len = shadow_plugin_v2->get_param(instance, "patch_count", buf, sizeof(buf));
    if (len <= 0) return -1;
    buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
    int count = atoi(buf);
    if (count <= 0) return -1;

    for (int i = 0; i < count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "patch_name_%d", i);
        len = shadow_plugin_v2->get_param(instance, key, buf, sizeof(buf));
        if (len <= 0) continue;
        buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
        if (strcmp(buf, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Unload a specific master FX slot */
static void shadow_master_fx_slot_unload(int slot) {
    if (slot < 0 || slot >= MASTER_FX_SLOTS) return;
    master_fx_slot_t *s = &shadow_master_fx_slots[slot];

    if (s->instance && s->api && s->api->destroy_instance) {
        s->api->destroy_instance(s->instance);
    }
    s->instance = NULL;
    s->api = NULL;
    if (s->handle) {
        dlclose(s->handle);
        s->handle = NULL;
    }
    s->module_path[0] = '\0';
    s->module_id[0] = '\0';
    capture_clear(&s->capture);
}

/* Unload all master FX slots */
static void shadow_master_fx_unload_all(void) {
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        shadow_master_fx_slot_unload(i);
    }
}

/* Load a master FX module into a specific slot by full DSP path.
 * Returns 0 on success, -1 on failure. */
static int shadow_master_fx_slot_load(int slot, const char *dsp_path) {
    if (slot < 0 || slot >= MASTER_FX_SLOTS) return -1;
    master_fx_slot_t *s = &shadow_master_fx_slots[slot];

    if (!dsp_path || !dsp_path[0]) {
        shadow_master_fx_slot_unload(slot);
        return 0;  /* Empty = disable this slot */
    }

    /* Already loaded? */
    if (strcmp(s->module_path, dsp_path) == 0 && s->instance) {
        return 0;
    }

    /* Unload previous */
    shadow_master_fx_slot_unload(slot);

    s->handle = dlopen(dsp_path, RTLD_NOW | RTLD_LOCAL);
    if (!s->handle) {
        fprintf(stderr, "Shadow master FX[%d]: failed to load %s: %s\n", slot, dsp_path, dlerror());
        return -1;
    }

    /* Look for audio FX v2 init function */
    audio_fx_init_v2_fn init_fn = (audio_fx_init_v2_fn)dlsym(s->handle, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init_fn) {
        fprintf(stderr, "Shadow master FX[%d]: %s not found in %s\n", slot, AUDIO_FX_INIT_V2_SYMBOL, dsp_path);
        dlclose(s->handle);
        s->handle = NULL;
        return -1;
    }

    s->api = init_fn(&shadow_host_api);
    if (!s->api || !s->api->create_instance) {
        fprintf(stderr, "Shadow master FX[%d]: init failed for %s\n", slot, dsp_path);
        dlclose(s->handle);
        s->handle = NULL;
        s->api = NULL;
        return -1;
    }

    /* Extract module directory from dsp_path (remove filename) */
    char module_dir[256];
    strncpy(module_dir, dsp_path, sizeof(module_dir) - 1);
    module_dir[sizeof(module_dir) - 1] = '\0';
    char *last_slash = strrchr(module_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
    }

    s->instance = s->api->create_instance(module_dir, NULL);
    if (!s->instance) {
        fprintf(stderr, "Shadow master FX[%d]: create_instance failed for %s\n", slot, dsp_path);
        dlclose(s->handle);
        s->handle = NULL;
        s->api = NULL;
        return -1;
    }

    strncpy(s->module_path, dsp_path, sizeof(s->module_path) - 1);
    s->module_path[sizeof(s->module_path) - 1] = '\0';

    /* Extract module ID from path (e.g., "/path/to/cloudseed/dsp.so" -> "cloudseed") */
    const char *id_start = strrchr(module_dir, '/');
    if (id_start) {
        strncpy(s->module_id, id_start + 1, sizeof(s->module_id) - 1);
    } else {
        strncpy(s->module_id, module_dir, sizeof(s->module_id) - 1);
    }
    s->module_id[sizeof(s->module_id) - 1] = '\0';

    /* Load capture rules from module.json capabilities */
    char module_json_path[512];
    snprintf(module_json_path, sizeof(module_json_path), "%s/module.json", module_dir);
    s->chain_params_cached = 0;
    s->chain_params_cache[0] = '\0';
    FILE *f = fopen(module_json_path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0 && size < 16384) {
            char *json = malloc(size + 1);
            if (json) {
                fread(json, 1, size, f);
                json[size] = '\0';
                const char *caps = strstr(json, "\"capabilities\"");
                if (caps) {
                    capture_parse_json(&s->capture, caps);
                }
                /* Cache chain_params to avoid file I/O in audio thread */
                const char *chain_params = strstr(json, "\"chain_params\"");
                if (chain_params) {
                    const char *arr_start = strchr(chain_params, '[');
                    if (arr_start) {
                        int depth = 1;
                        const char *arr_end = arr_start + 1;
                        while (*arr_end && depth > 0) {
                            if (*arr_end == '[') depth++;
                            else if (*arr_end == ']') depth--;
                            arr_end++;
                        }
                        int len = (int)(arr_end - arr_start);
                        if (len > 0 && len < (int)sizeof(s->chain_params_cache) - 1) {
                            memcpy(s->chain_params_cache, arr_start, len);
                            s->chain_params_cache[len] = '\0';
                            s->chain_params_cached = 1;
                        }
                    }
                }
                free(json);
            }
        }
        fclose(f);
    }

    fprintf(stderr, "Shadow master FX[%d]: loaded %s\n", slot, dsp_path);
    return 0;
}

/* Legacy wrapper: load into slot 0 for backward compatibility */
static int shadow_master_fx_load(const char *dsp_path) {
    return shadow_master_fx_slot_load(0, dsp_path);
}

/* Legacy wrapper: unload slot 0 */
static void shadow_master_fx_unload(void) {
    shadow_master_fx_slot_unload(0);
}

/* Forward declaration for capture loading */
static void shadow_slot_load_capture(int slot, int patch_index);

static int shadow_inprocess_load_chain(void) {
    if (shadow_inprocess_ready) return 0;

    shadow_dsp_handle = dlopen(SHADOW_CHAIN_DSP_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!shadow_dsp_handle) {
        fprintf(stderr, "Shadow inprocess: failed to load %s: %s\n",
                SHADOW_CHAIN_DSP_PATH, dlerror());
        return -1;
    }

    memset(&shadow_host_api, 0, sizeof(shadow_host_api));
    shadow_host_api.api_version = MOVE_PLUGIN_API_VERSION;
    shadow_host_api.sample_rate = MOVE_SAMPLE_RATE;
    shadow_host_api.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    shadow_host_api.mapped_memory = global_mmap_addr;
    shadow_host_api.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    shadow_host_api.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    shadow_host_api.log = shadow_log;

    move_plugin_init_v2_fn init_v2 = (move_plugin_init_v2_fn)dlsym(
        shadow_dsp_handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (!init_v2) {
        fprintf(stderr, "Shadow inprocess: %s not found\n", MOVE_PLUGIN_INIT_V2_SYMBOL);
        dlclose(shadow_dsp_handle);
        shadow_dsp_handle = NULL;
        return -1;
    }

    shadow_plugin_v2 = init_v2(&shadow_host_api);
    if (!shadow_plugin_v2 || !shadow_plugin_v2->create_instance) {
        fprintf(stderr, "Shadow inprocess: chain v2 init failed\n");
        dlclose(shadow_dsp_handle);
        shadow_dsp_handle = NULL;
        shadow_plugin_v2 = NULL;
        return -1;
    }

    shadow_chain_load_config();
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        shadow_chain_slots[i].instance = shadow_plugin_v2->create_instance(
            SHADOW_CHAIN_MODULE_DIR, NULL);
        if (!shadow_chain_slots[i].instance) {
            continue;
        }
        /* Check for "none" - means slot should be inactive */
        if (strcasecmp(shadow_chain_slots[i].patch_name, "none") == 0 ||
            shadow_chain_slots[i].patch_name[0] == '\0') {
            shadow_chain_slots[i].active = 0;
            shadow_chain_slots[i].patch_index = -1;
            continue;
        }
        int idx = shadow_chain_find_patch_index(shadow_chain_slots[i].instance,
                                                shadow_chain_slots[i].patch_name);
        shadow_chain_slots[i].patch_index = idx;
        if (idx >= 0 && shadow_plugin_v2->set_param) {
            char idx_str[16];
            snprintf(idx_str, sizeof(idx_str), "%d", idx);
            shadow_plugin_v2->set_param(shadow_chain_slots[i].instance, "load_patch", idx_str);
            shadow_chain_slots[i].active = 1;
            /* Load capture rules from the patch file */
            shadow_slot_load_capture(i, idx);
            /* Query synth's default forward channel after patch load */
            if (shadow_plugin_v2->get_param) {
                char fwd_buf[16];
                int len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                    "synth:default_forward_channel", fwd_buf, sizeof(fwd_buf));
                if (len > 0) {
                    fwd_buf[len < (int)sizeof(fwd_buf) ? len : (int)sizeof(fwd_buf) - 1] = '\0';
                    int default_fwd = atoi(fwd_buf);
                    if (default_fwd >= 0 && default_fwd <= 15) {
                        shadow_chain_slots[i].forward_channel = default_fwd;
                    }
                }
            }
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Shadow inprocess: patch not found: %s",
                     shadow_chain_slots[i].patch_name);
            shadow_log(msg);
        }
    }

    shadow_ui_state_refresh();
    shadow_inprocess_ready = 1;
    /* Start countdown for delayed mod wheel reset after Move's startup MIDI settles */
    shadow_startup_modwheel_countdown = STARTUP_MODWHEEL_RESET_FRAMES;
    if (shadow_control) {
        /* Allow display hotkey when running in-process DSP. */
        shadow_control->shadow_ready = 1;
    }
    launch_shadow_ui();
    shadow_log("Shadow inprocess: chain loaded");
    return 0;
}

static int shadow_chain_slot_for_channel(int ch) {
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        if (shadow_chain_slots[i].channel != ch) continue;
        if (shadow_chain_slots[i].active) {
            return i;
        }
        if (shadow_plugin_v2 && shadow_plugin_v2->get_param &&
            shadow_chain_slots[i].instance) {
            char buf[64];
            int len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                                                  "synth_module", buf, sizeof(buf));
            if (len > 0) {
                if (len < (int)sizeof(buf)) buf[len] = '\0';
                else buf[sizeof(buf) - 1] = '\0';
                if (buf[0] != '\0') {
                    shadow_chain_slots[i].active = 1;
                    shadow_ui_state_update_slot(i);
                    return i;
                }
            }
        }
    }
    return -1;
}

/* Apply forward channel remapping for a slot.
 * If forward_channel >= 0, remap to that specific channel.
 * If forward_channel == -1 (auto), use the slot's receive channel. */
static inline uint8_t shadow_chain_remap_channel(int slot, uint8_t status) {
    int fwd_ch = shadow_chain_slots[slot].forward_channel;
    if (fwd_ch >= 0 && fwd_ch <= 15) {
        /* Specific forward channel */
        return (status & 0xF0) | (uint8_t)fwd_ch;
    }
    /* Auto: use the receive channel (pass through unchanged) */
    return (status & 0xF0) | (uint8_t)shadow_chain_slots[slot].channel;
}

static int shadow_is_internal_control_note(uint8_t note)
{
    /* Capacitive touch (0-9) and track buttons (40-43) are internal.
     * Note: Step buttons (16-31) are NOT included - they overlap with musical notes E0-G1. */
    return (note < 10) || (note >= 40 && note <= 43);
}

/* Note: shadow_allow_midi_to_dsp and shadow_route_knob_cc_to_focused_slot removed.
 * MIDI_IN is no longer routed directly to DSP. Shadow UI handles knobs via set_param. */

static uint32_t shadow_ui_request_seen = 0;
/* SHADOW_PATCH_INDEX_NONE from shadow_constants.h */

/* Helper to write debug to log file (shadow_log isn't available yet) */
static void capture_debug_log(const char *msg) {
    FILE *log = fopen("/data/UserData/move-anything/shadow_capture_debug.log", "a");
    if (log) {
        fprintf(log, "%s\n", msg);
        fclose(log);
    }
}

/* Load capture rules for a slot by reading its patch file */
static void shadow_slot_load_capture(int slot, int patch_index)
{
    char dbg[512];
    snprintf(dbg, sizeof(dbg), "shadow_slot_load_capture: slot=%d patch_index=%d", slot, patch_index);
    capture_debug_log(dbg);

    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) return;
    if (!shadow_chain_slots[slot].instance) {
        capture_debug_log("  -> no instance");
        return;
    }
    if (!shadow_plugin_v2 || !shadow_plugin_v2->get_param) {
        capture_debug_log("  -> no plugin_v2/get_param");
        return;
    }

    /* Clear existing capture rules */
    capture_clear(&shadow_chain_slots[slot].capture);

    /* Get the patch file path from chain module */
    char key[32];
    char path[512];
    snprintf(key, sizeof(key), "patch_path_%d", patch_index);
    int len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance, key, path, sizeof(path));
    snprintf(dbg, sizeof(dbg), "  -> get_param(%s) len=%d", key, len);
    capture_debug_log(dbg);
    if (len <= 0) return;
    path[len < (int)sizeof(path) ? len : (int)sizeof(path) - 1] = '\0';
    snprintf(dbg, sizeof(dbg), "  -> path: %s", path);
    capture_debug_log(dbg);

    /* Read the patch file */
    FILE *f = fopen(path, "r");
    if (!f) {
        capture_debug_log("  -> fopen failed");
        return;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 16384) {
        fclose(f);
        return;
    }
    
    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return;
    }
    
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);
    
    /* Parse capture rules from JSON */
    capture_parse_json(&shadow_chain_slots[slot].capture, json);
    free(json);

    /* Log capture rules summary */
    int has_notes = 0, has_ccs = 0;
    for (int b = 0; b < 16; b++) {
        if (shadow_chain_slots[slot].capture.notes[b]) has_notes = 1;
        if (shadow_chain_slots[slot].capture.ccs[b]) has_ccs = 1;
    }
    snprintf(dbg, sizeof(dbg), "  -> capture parsed: has_notes=%d has_ccs=%d", has_notes, has_ccs);
    capture_debug_log(dbg);
    /* Debug: check if note 16 is captured */
    snprintf(dbg, sizeof(dbg), "  -> note 16 captured: %d", capture_has_note(&shadow_chain_slots[slot].capture, 16));
    capture_debug_log(dbg);
    if (has_notes || has_ccs) {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "Slot %d capture loaded: notes=%d ccs=%d",
                 slot, has_notes, has_ccs);
        shadow_log(dbg);
    }
}

static void shadow_inprocess_handle_ui_request(void) {
    if (!shadow_control || !shadow_plugin_v2 || !shadow_plugin_v2->set_param) return;

    uint32_t request_id = shadow_control->ui_request_id;
    if (request_id == shadow_ui_request_seen) return;
    shadow_ui_request_seen = request_id;

    int slot = shadow_control->ui_slot;
    int patch_index = shadow_control->ui_patch_index;

    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "UI request: slot=%d patch=%d instance=%p",
                 slot, patch_index, shadow_chain_slots[slot < SHADOW_CHAIN_INSTANCES ? slot : 0].instance);
        shadow_log(dbg);
    }

    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) return;
    if (patch_index < 0) return;
    if (!shadow_chain_slots[slot].instance) {
        shadow_log("UI request: slot instance is NULL, aborting");
        return;
    }

    /* Handle "none" special value - clear the slot */
    if (patch_index == SHADOW_PATCH_INDEX_NONE) {
        /* Unload synth and FX modules */
        if (shadow_plugin_v2->set_param && shadow_chain_slots[slot].instance) {
            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, "synth:module", "");
            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, "fx1:module", "");
            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, "fx2:module", "");
        }
        shadow_chain_slots[slot].active = 0;
        shadow_chain_slots[slot].patch_index = -1;
        capture_clear(&shadow_chain_slots[slot].capture);
        strncpy(shadow_chain_slots[slot].patch_name, "", sizeof(shadow_chain_slots[slot].patch_name) - 1);
        shadow_chain_slots[slot].patch_name[sizeof(shadow_chain_slots[slot].patch_name) - 1] = '\0';
        /* Update UI state */
        if (shadow_ui_state && slot < SHADOW_UI_SLOTS) {
            strncpy(shadow_ui_state->slot_names[slot], "", SHADOW_UI_NAME_LEN - 1);
            shadow_ui_state->slot_names[slot][SHADOW_UI_NAME_LEN - 1] = '\0';
        }
        return;
    }

    if (shadow_plugin_v2->get_param) {
        char buf[32];
        int len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
                                              "patch_count", buf, sizeof(buf));
        if (len > 0) {
            buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
            int patch_count = atoi(buf);
            if (patch_count > 0 && patch_index >= patch_count) {
                return;
            }
        }
    }

    char idx_str[16];
    snprintf(idx_str, sizeof(idx_str), "%d", patch_index);
    shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, "load_patch", idx_str);
    shadow_chain_slots[slot].patch_index = patch_index;
    shadow_chain_slots[slot].active = 1;

    if (shadow_plugin_v2->get_param) {
        char key[32];
        char buf[128];
        int len = 0;
        snprintf(key, sizeof(key), "patch_name_%d", patch_index);
        len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance, key, buf, sizeof(buf));
        if (len > 0) {
            buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
            strncpy(shadow_chain_slots[slot].patch_name, buf, sizeof(shadow_chain_slots[slot].patch_name) - 1);
            shadow_chain_slots[slot].patch_name[sizeof(shadow_chain_slots[slot].patch_name) - 1] = '\0';
        }
    }

    /* Load capture rules from the patch file */
    shadow_slot_load_capture(slot, patch_index);

    shadow_ui_state_update_slot(slot);
}

/* Handle slot-level param (volume, forward_channel, etc.) - returns 1 if handled */
static int shadow_handle_slot_param_set(int slot, const char *key, const char *value) {
    if (strcmp(key, "slot:volume") == 0) {
        float vol = atof(value);
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;
        shadow_chain_slots[slot].volume = vol;
        shadow_ui_state_update_slot(slot);
        return 1;
    }
    if (strcmp(key, "slot:forward_channel") == 0) {
        int ch = atoi(value);
        if (ch < -1) ch = -1;
        if (ch > 15) ch = 15;
        shadow_chain_slots[slot].forward_channel = ch;
        shadow_ui_state_update_slot(slot);
        return 1;
    }
    if (strcmp(key, "slot:receive_channel") == 0) {
        int ch = atoi(value);
        if (ch >= 1 && ch <= 16) {
            shadow_chain_slots[slot].channel = ch - 1;  /* Store 0-based */
            shadow_ui_state_update_slot(slot);
        }
        return 1;
    }
    return 0;  /* Not a slot param */
}

/* Handle slot-level param get - returns length if handled, -1 if not */
static int shadow_handle_slot_param_get(int slot, const char *key, char *buf, int buf_len) {
    if (strcmp(key, "slot:volume") == 0) {
        return snprintf(buf, buf_len, "%.2f", shadow_chain_slots[slot].volume);
    }
    if (strcmp(key, "slot:forward_channel") == 0) {
        return snprintf(buf, buf_len, "%d", shadow_chain_slots[slot].forward_channel);
    }
    if (strcmp(key, "slot:receive_channel") == 0) {
        return snprintf(buf, buf_len, "%d", shadow_chain_slots[slot].channel + 1);  /* Return 1-based */
    }
    return -1;  /* Not a slot param */
}

static void shadow_inprocess_handle_param_request(void) {
    if (!shadow_param) return;

    uint8_t req_type = shadow_param->request_type;
    if (req_type == 0) return;  /* No pending request */

    /* Handle master FX chain params: master_fx:fx1:module, master_fx:fx2:wet, etc. */
    if (strncmp(shadow_param->key, "master_fx:", 10) == 0) {
        const char *fx_key = shadow_param->key + 10;
        int mfx_slot = -1;  /* -1 = legacy (slot 0), 0-3 = specific slot */
        const char *param_key = fx_key;

        /* Parse slot: fx1:, fx2:, fx3:, fx4: */
        if (strncmp(fx_key, "fx1:", 4) == 0) { mfx_slot = 0; param_key = fx_key + 4; }
        else if (strncmp(fx_key, "fx2:", 4) == 0) { mfx_slot = 1; param_key = fx_key + 4; }
        else if (strncmp(fx_key, "fx3:", 4) == 0) { mfx_slot = 2; param_key = fx_key + 4; }
        else if (strncmp(fx_key, "fx4:", 4) == 0) { mfx_slot = 3; param_key = fx_key + 4; }
        else { mfx_slot = 0; param_key = fx_key; }  /* Legacy: default to slot 0 */

        master_fx_slot_t *mfx = &shadow_master_fx_slots[mfx_slot];

        if (req_type == 1) {  /* SET */
            if (strcmp(param_key, "module") == 0) {
                /* Load or unload master FX slot */
                int result = shadow_master_fx_slot_load(mfx_slot, shadow_param->value);
                shadow_param->error = (result == 0) ? 0 : 7;
                shadow_param->result_len = 0;
            } else if (strcmp(param_key, "param") == 0 && mfx->api && mfx->instance) {
                /* Set master FX param: value is "key=value" */
                char *eq = strchr(shadow_param->value, '=');
                if (eq && mfx->api->set_param) {
                    *eq = '\0';
                    mfx->api->set_param(mfx->instance, shadow_param->value, eq + 1);
                    *eq = '=';
                    shadow_param->error = 0;
                } else {
                    shadow_param->error = 8;
                }
                shadow_param->result_len = 0;
            } else if (mfx->api && mfx->instance && mfx->api->set_param) {
                /* Direct param set: master_fx:fx1:wet -> set_param("wet", value) */
                mfx->api->set_param(mfx->instance, param_key, shadow_param->value);
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else {
                shadow_param->error = 9;
                shadow_param->result_len = -1;
            }
        } else if (req_type == 2) {  /* GET */
            if (strcmp(param_key, "module") == 0) {
                strncpy(shadow_param->value, mfx->module_path, SHADOW_PARAM_VALUE_LEN - 1);
                shadow_param->value[SHADOW_PARAM_VALUE_LEN - 1] = '\0';
                shadow_param->error = 0;
                shadow_param->result_len = strlen(shadow_param->value);
            } else if (strcmp(param_key, "name") == 0) {
                /* Return module ID (display name) */
                strncpy(shadow_param->value, mfx->module_id, SHADOW_PARAM_VALUE_LEN - 1);
                shadow_param->value[SHADOW_PARAM_VALUE_LEN - 1] = '\0';
                shadow_param->error = 0;
                shadow_param->result_len = strlen(shadow_param->value);
            } else if (strcmp(param_key, "error") == 0) {
                /* Return load error from master FX module (if any) */
                shadow_param->value[0] = '\0';
                shadow_param->error = 0;
                shadow_param->result_len = 0;
                if (mfx->api && mfx->instance && mfx->api->get_param) {
                    int len = mfx->api->get_param(mfx->instance, "load_error",
                                                   shadow_param->value, SHADOW_PARAM_VALUE_LEN);
                    if (len > 0) {
                        shadow_param->result_len = len;
                    }
                }
            } else if (strcmp(param_key, "chain_params") == 0) {
                /* Try module's get_param first (for dynamic params like CLAP FX) */
                if (mfx->api && mfx->instance && mfx->api->get_param) {
                    int len = mfx->api->get_param(mfx->instance, "chain_params",
                                                   shadow_param->value, SHADOW_PARAM_VALUE_LEN);
                    if (len > 2) {  /* More than empty "[]" */
                        shadow_param->error = 0;
                        shadow_param->result_len = len;
                        shadow_param->response_ready = 1;
                        shadow_param->request_type = 0;
                        return;
                    }
                }

                /* Use cached chain_params (avoids file I/O in audio thread) */
                if (mfx->chain_params_cached && mfx->chain_params_cache[0]) {
                    int len = strlen(mfx->chain_params_cache);
                    if (len < SHADOW_PARAM_VALUE_LEN - 1) {
                        memcpy(shadow_param->value, mfx->chain_params_cache, len + 1);
                        shadow_param->error = 0;
                        shadow_param->result_len = len;
                        shadow_param->response_ready = 1;
                        shadow_param->request_type = 0;
                        return;
                    }
                }
                /* Fall through if chain_params not cached */
                shadow_param->value[0] = '[';
                shadow_param->value[1] = ']';
                shadow_param->value[2] = '\0';
                shadow_param->error = 0;
                shadow_param->result_len = 2;
            } else if (strcmp(param_key, "ui_hierarchy") == 0) {
                /* Read ui_hierarchy from module.json */
                char module_dir[256];
                strncpy(module_dir, mfx->module_path, sizeof(module_dir) - 1);
                module_dir[sizeof(module_dir) - 1] = '\0';
                char *last_slash = strrchr(module_dir, '/');
                if (last_slash) *last_slash = '\0';

                char json_path[512];
                snprintf(json_path, sizeof(json_path), "%s/module.json", module_dir);

                FILE *f = fopen(json_path, "r");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long size = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    /* Allow larger files - we only extract ui_hierarchy object */
                    if (size > 0 && size < 32768) {
                        char *json = malloc(size + 1);
                        if (json) {
                            fread(json, 1, size, f);
                            json[size] = '\0';

                            /* Find ui_hierarchy in capabilities */
                            const char *ui_hier = strstr(json, "\"ui_hierarchy\"");
                            if (ui_hier) {
                                const char *obj_start = strchr(ui_hier + 14, '{');
                                if (obj_start) {
                                    int depth = 1;
                                    const char *obj_end = obj_start + 1;
                                    while (*obj_end && depth > 0) {
                                        if (*obj_end == '{') depth++;
                                        else if (*obj_end == '}') depth--;
                                        obj_end++;
                                    }
                                    int len = (int)(obj_end - obj_start);
                                    if (len > 0 && len < SHADOW_PARAM_VALUE_LEN - 1) {
                                        memcpy(shadow_param->value, obj_start, len);
                                        shadow_param->value[len] = '\0';
                                        shadow_param->error = 0;
                                        shadow_param->result_len = len;
                                        free(json);
                                        fclose(f);
                                        shadow_param->response_ready = 1;
                                        shadow_param->request_type = 0;
                                        return;
                                    }
                                }
                            }
                            free(json);
                        }
                    }
                    fclose(f);
                }
                /* ui_hierarchy not found - return null (will fall back to chain_params in JS) */
                shadow_param->error = 12;
                shadow_param->result_len = -1;
            } else if (mfx->api && mfx->instance && mfx->api->get_param) {
                /* Get master FX param by key */
                int len = mfx->api->get_param(mfx->instance, param_key,
                                               shadow_param->value, SHADOW_PARAM_VALUE_LEN);
                if (len >= 0) {
                    shadow_param->error = 0;
                    shadow_param->result_len = len;
                } else {
                    shadow_param->error = 10;
                    shadow_param->result_len = -1;
                }
            } else {
                shadow_param->error = 11;
                shadow_param->result_len = -1;
            }
        } else {
            shadow_param->error = 6;
            shadow_param->result_len = -1;
        }
        shadow_param->response_ready = 1;
        shadow_param->request_type = 0;
        return;
    }

    int slot = shadow_param->slot;
    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) {
        shadow_param->error = 1;
        shadow_param->result_len = -1;
        shadow_param->response_ready = 1;
        shadow_param->request_type = 0;
        return;
    }

    /* Handle slot-level params first */
    if (req_type == 1) {  /* SET param */
        if (shadow_handle_slot_param_set(slot, shadow_param->key, shadow_param->value)) {
            shadow_param->error = 0;
            shadow_param->result_len = 0;
            shadow_param->response_ready = 1;
            shadow_param->request_type = 0;
            return;
        }
    }
    else if (req_type == 2) {  /* GET param */
        int len = shadow_handle_slot_param_get(slot, shadow_param->key,
                                                shadow_param->value, SHADOW_PARAM_VALUE_LEN);
        if (len >= 0) {
            shadow_param->error = 0;
            shadow_param->result_len = len;
            shadow_param->response_ready = 1;
            shadow_param->request_type = 0;
            return;
        }
    }

    /* Not a slot param - forward to plugin */
    if (!shadow_plugin_v2 || !shadow_chain_slots[slot].instance) {
        shadow_param->error = 2;
        shadow_param->result_len = -1;
        shadow_param->response_ready = 1;
        shadow_param->request_type = 0;
        return;
    }

    if (req_type == 1) {  /* SET param */
        if (shadow_plugin_v2->set_param) {
            /* Make local copies - shared memory may be modified during set_param */
            char key_copy[SHADOW_PARAM_KEY_LEN];
            char value_copy[SHADOW_PARAM_VALUE_LEN];  /* Full size for patch JSON with state */
            strncpy(key_copy, shadow_param->key, sizeof(key_copy) - 1);
            key_copy[sizeof(key_copy) - 1] = '\0';
            strncpy(value_copy, shadow_param->value, sizeof(value_copy) - 1);
            value_copy[sizeof(value_copy) - 1] = '\0';

            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance,
                                        key_copy, value_copy);
            shadow_param->error = 0;
            shadow_param->result_len = 0;

            /* Activate slot when synth module is loaded */
            if (strcmp(key_copy, "synth:module") == 0) {
                if (value_copy[0] != '\0') {
                    shadow_chain_slots[slot].active = 1;

                    /* Query synth's default forward channel and apply if valid */
                    if (shadow_plugin_v2->get_param) {
                        char fwd_buf[16];
                        int len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
                            "synth:default_forward_channel", fwd_buf, sizeof(fwd_buf));
                        if (len > 0) {
                            fwd_buf[len < (int)sizeof(fwd_buf) ? len : (int)sizeof(fwd_buf) - 1] = '\0';
                            int default_fwd = atoi(fwd_buf);
                            if (default_fwd >= 0 && default_fwd <= 15) {
                                shadow_chain_slots[slot].forward_channel = default_fwd;
                                shadow_ui_state_update_slot(slot);
                            }
                        }
                    }
                } else {
                    shadow_chain_slots[slot].active = 0;
                }
            }
            /* Activate slot when a patch is loaded via set_param */
            if (strcmp(key_copy, "load_patch") == 0 ||
                strcmp(key_copy, "patch") == 0) {
                int idx = atoi(value_copy);
                if (idx < 0 || idx == SHADOW_PATCH_INDEX_NONE) {
                    shadow_chain_slots[slot].active = 0;
                    shadow_chain_slots[slot].patch_index = -1;
                    capture_clear(&shadow_chain_slots[slot].capture);
                    shadow_chain_slots[slot].patch_name[0] = '\0';
                } else {
                    shadow_chain_slots[slot].active = 1;
                    shadow_chain_slots[slot].patch_index = idx;
                    shadow_slot_load_capture(slot, idx);

                    /* Query synth's default forward channel after patch load */
                    if (shadow_plugin_v2->get_param) {
                        char fwd_buf[16];
                        int len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
                            "synth:default_forward_channel", fwd_buf, sizeof(fwd_buf));
                        if (len > 0) {
                            fwd_buf[len < (int)sizeof(fwd_buf) ? len : (int)sizeof(fwd_buf) - 1] = '\0';
                            int default_fwd = atoi(fwd_buf);
                            if (default_fwd >= 0 && default_fwd <= 15) {
                                shadow_chain_slots[slot].forward_channel = default_fwd;
                            }
                        }
                    }
                }
                shadow_ui_state_update_slot(slot);
            }

            if (shadow_midi_out_log_enabled()) {
                if (strcmp(key_copy, "synth:module") == 0 ||
                    strcmp(key_copy, "fx1:module") == 0 ||
                    strcmp(key_copy, "fx2:module") == 0 ||
                    strcmp(key_copy, "midi_fx1:module") == 0) {
                    shadow_midi_out_logf("param_set: slot=%d key=%s val=%s active=%d",
                        slot, key_copy, value_copy, shadow_chain_slots[slot].active);
                }
            }
        } else {
            shadow_param->error = 3;
            shadow_param->result_len = -1;
        }
    }
    else if (req_type == 2) {  /* GET param */
        if (shadow_plugin_v2->get_param) {
            /* Clear buffer before get_param to prevent any stale data */
            memset(shadow_param->value, 0, 256);  /* Clear first 256 bytes */
            int len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
                                                  shadow_param->key,
                                                  shadow_param->value,
                                                  SHADOW_PARAM_VALUE_LEN);
            if (len >= 0) {
                if (len < SHADOW_PARAM_VALUE_LEN) {
                    shadow_param->value[len] = '\0';
                } else {
                    shadow_param->value[SHADOW_PARAM_VALUE_LEN - 1] = '\0';
                }
                shadow_param->error = 0;
                shadow_param->result_len = len;
            } else {
                shadow_param->error = 4;
                shadow_param->result_len = -1;
            }
        } else {
            shadow_param->error = 5;
            shadow_param->result_len = -1;
        }
    }
    else {
        shadow_param->error = 6;  /* Unknown request type */
        shadow_param->result_len = -1;
    }

    shadow_param->response_ready = 1;
    shadow_param->request_type = 0;
}

/* Forward CC, pitch bend, aftertouch from external MIDI (MIDI_IN cable 2) to MIDI_OUT.
 * Move echoes notes but not these message types, so we inject them into MIDI_OUT
 * so the DSP routing can pick them up alongside the echoed notes. */
static void shadow_forward_external_cc_to_out(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    uint8_t *in_src = global_mmap_addr + MIDI_IN_OFFSET;
    uint8_t *out_dst = global_mmap_addr + MIDI_OUT_OFFSET;

    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = in_src[i] & 0x0F;
        uint8_t cable = (in_src[i] >> 4) & 0x0F;

        /* Only process external MIDI (cable 2) */
        if (cable != 0x02) continue;
        if (cin < 0x08 || cin > 0x0E) continue;

        uint8_t status = in_src[i + 1];
        uint8_t type = status & 0xF0;

        /* Only forward CC (0xB0), pitch bend (0xE0), channel aftertouch (0xD0), poly aftertouch (0xA0) */
        if (type != 0xB0 && type != 0xE0 && type != 0xD0 && type != 0xA0) continue;

        /* Find an empty slot in MIDI_OUT and inject the message */
        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            if (out_dst[j] == 0 && out_dst[j+1] == 0 && out_dst[j+2] == 0 && out_dst[j+3] == 0) {
                /* Copy the packet, keeping cable 2 */
                out_dst[j] = in_src[i];
                out_dst[j + 1] = in_src[i + 1];
                out_dst[j + 2] = in_src[i + 2];
                out_dst[j + 3] = in_src[i + 3];
                break;
            }
        }
    }
}

static void shadow_inprocess_process_midi(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    /* Delayed mod wheel reset - fires after Move's startup MIDI burst settles.
     * This ensures any stale mod wheel values from Move's track state are cleared. */
    if (shadow_startup_modwheel_countdown > 0) {
        shadow_startup_modwheel_countdown--;
        if (shadow_startup_modwheel_countdown == 0) {
            shadow_log("Sending startup mod wheel reset to all slots");
            if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
                    if (shadow_chain_slots[s].active && shadow_chain_slots[s].instance) {
                        /* Send CC 1 = 0 (mod wheel reset) on all 16 channels */
                        for (int ch = 0; ch < 16; ch++) {
                            uint8_t mod_reset[3] = {(uint8_t)(0xB0 | ch), 1, 0};
                            shadow_plugin_v2->on_midi(shadow_chain_slots[s].instance, mod_reset, 3,
                                                      MOVE_MIDI_SOURCE_HOST);
                        }
                    }
                }
            }
        }
    }

    /* MIDI_IN (internal controls) is NOT routed to DSP here.
     * - Shadow UI handles knobs via set_param based on ui_hierarchy
     * - Capture rules are handled in shadow_filter_move_input (post-ioctl)
     * - Internal notes/CCs should only reach Move, not DSP */

    /* MIDI_OUT → DSP: Move's track output contains only musical notes.
     * Internal controls (knob touches, step buttons) do NOT appear in MIDI_OUT.
     * We must clear packets after reading to avoid re-processing stale data. */
    uint8_t *out_src = global_mmap_addr + MIDI_OUT_OFFSET;
    int log_on = shadow_midi_out_log_enabled();
    static int midi_log_count = 0;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        const uint8_t *pkt = &out_src[i];
        if (pkt[0] == 0 && pkt[1] == 0 && pkt[2] == 0 && pkt[3] == 0) continue;

        uint8_t cin = pkt[0] & 0x0F;
        uint8_t cable = (pkt[0] >> 4) & 0x0F;
        uint8_t status_usb = pkt[1];
        uint8_t status_raw = pkt[0];

        /* Handle system realtime messages (CIN=0x0F): clock, start, continue, stop
         * These are 1-byte messages that should be broadcast to ALL active slots */
        if (cin == 0x0F && status_usb >= 0xF8 && status_usb <= 0xFF) {
            /* Filter cable 0 (Move UI events) - track output is on cable 2 */
            if (cable == 0) {
                continue;
            }
            /* Broadcast to all active slots */
            if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                uint8_t msg[3] = { status_usb, 0, 0 };
                for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
                    if (shadow_chain_slots[s].active && shadow_chain_slots[s].instance) {
                        shadow_plugin_v2->on_midi(shadow_chain_slots[s].instance, msg, 1,
                                                  MOVE_MIDI_SOURCE_EXTERNAL);
                    }
                }
            }
            continue;  /* Done with this packet */
        }

        /* USB MIDI format: CIN in low nibble of byte 0 */
        if (cin >= 0x08 && cin <= 0x0E && (status_usb & 0x80)) {
            if ((status_usb & 0xF0) < 0x80 || (status_usb & 0xF0) > 0xE0) continue;

            /* Validate CIN matches status type (filter garbage/stale data) */
            uint8_t type = status_usb & 0xF0;
            uint8_t expected_cin = (type >> 4);  /* Note-off=0x8, Note-on=0x9, etc. */
            if (cin != expected_cin) {
                continue;  /* CIN doesn't match status - skip invalid packet */
            }

            /* Filter cable 0 (Move UI events) - track output is on cable 2 */
            if (cable == 0) {
                continue;
            }

            /* Filter internal control notes: knob touches (0-9) */
            uint8_t note = pkt[2];
            if ((type == 0x90 || type == 0x80) && note < 10) {
                continue;
            }
            int slot = shadow_chain_slot_for_channel(status_usb & 0x0F);
            if (log_on && type == 0x90 && pkt[3] > 0) {
                if (midi_log_count < 100) {
                    char dbg[256];
                    if (slot < 0) {
                        snprintf(dbg, sizeof(dbg),
                            "midi_out: note=%u vel=%u ch=%u slot=-1 slots=[0:%d/%d 1:%d/%d 2:%d/%d 3:%d/%d]",
                            note, pkt[3], status_usb & 0x0F,
                            shadow_chain_slots[0].active, shadow_chain_slots[0].channel,
                            shadow_chain_slots[1].active, shadow_chain_slots[1].channel,
                            shadow_chain_slots[2].active, shadow_chain_slots[2].channel,
                            shadow_chain_slots[3].active, shadow_chain_slots[3].channel);
                    } else {
                        snprintf(dbg, sizeof(dbg),
                            "midi_out: note=%u vel=%u ch=%u slot=%d slot_active=%d slot_ch=%d",
                            note, pkt[3], status_usb & 0x0F, slot,
                            shadow_chain_slots[slot].active,
                            shadow_chain_slots[slot].channel);
                    }
                    shadow_log(dbg);
                    midi_log_count++;
                }
                if (slot < 0) {
                    shadow_midi_out_logf(
                        "midi_out: note=%u vel=%u ch=%u slot=-1 slots=[0:%d/%d 1:%d/%d 2:%d/%d 3:%d/%d]",
                        note, pkt[3], status_usb & 0x0F,
                        shadow_chain_slots[0].active, shadow_chain_slots[0].channel,
                        shadow_chain_slots[1].active, shadow_chain_slots[1].channel,
                        shadow_chain_slots[2].active, shadow_chain_slots[2].channel,
                        shadow_chain_slots[3].active, shadow_chain_slots[3].channel);
                } else {
                    shadow_midi_out_logf(
                        "midi_out: note=%u vel=%u ch=%u slot=%d slot_active=%d slot_ch=%d",
                        note, pkt[3], status_usb & 0x0F, slot,
                        shadow_chain_slots[slot].active,
                        shadow_chain_slots[slot].channel);
                }
            }
            if (slot < 0) continue;
            if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                uint8_t msg[3] = { shadow_chain_remap_channel(slot, pkt[1]), pkt[2], pkt[3] };
                shadow_plugin_v2->on_midi(shadow_chain_slots[slot].instance, msg, 3,
                                          MOVE_MIDI_SOURCE_EXTERNAL);
            }
        }
        /* Raw MIDI format fallback removed - was matching garbage/stale data.
         * USB MIDI format (with CIN validation) is the proper format for this buffer. */
    }
}

static void shadow_inprocess_mix_audio(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    int32_t mix[FRAMES_PER_BLOCK * 2];
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        mix[i] = mailbox_audio[i];
    }

    if (shadow_plugin_v2 && shadow_plugin_v2->render_block) {
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!shadow_chain_slots[s].active || !shadow_chain_slots[s].instance) continue;
            int16_t render_buffer[FRAMES_PER_BLOCK * 2];
            memset(render_buffer, 0, sizeof(render_buffer));
            shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                           render_buffer,
                                           MOVE_FRAMES_PER_BLOCK);
            float vol = shadow_chain_slots[s].volume;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                mix[i] += (int32_t)(render_buffer[i] * vol);
            }
        }
    }

    /* Clamp and write to output buffer */
    int16_t output_buffer[FRAMES_PER_BLOCK * 2];
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        if (mix[i] > 32767) mix[i] = 32767;
        if (mix[i] < -32768) mix[i] = -32768;
        output_buffer[i] = (int16_t)mix[i];
    }

    /* Apply master FX chain - process through all 4 slots in series */
    for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
        master_fx_slot_t *s = &shadow_master_fx_slots[fx];
        if (s->instance && s->api && s->api->process_block) {
            s->api->process_block(s->instance, output_buffer, FRAMES_PER_BLOCK);
        }
    }

    /* Apply master volume to shadow output */
    float mv = shadow_master_volume;
    if (mv < 1.0f) {
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            output_buffer[i] = (int16_t)(output_buffer[i] * mv);
        }
    }

    /* Write final output to mailbox */
    memcpy(mailbox_audio, output_buffer, sizeof(output_buffer));
}

/* === DEFERRED DSP RENDERING ===
 * Render DSP into buffer (slow, ~300µs) - called POST-ioctl
 * This renders audio for the NEXT frame, adding one frame of latency (~3ms)
 * but allowing Move to process pad events faster after ioctl returns.
 */
static void shadow_inprocess_render_to_buffer(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    /* Clear the deferred buffer */
    memset(shadow_deferred_dsp_buffer, 0, sizeof(shadow_deferred_dsp_buffer));

    /* Render each slot's DSP */
    if (shadow_plugin_v2 && shadow_plugin_v2->render_block) {
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!shadow_chain_slots[s].active || !shadow_chain_slots[s].instance) continue;
            int16_t render_buffer[FRAMES_PER_BLOCK * 2];
            memset(render_buffer, 0, sizeof(render_buffer));
            shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                           render_buffer,
                                           MOVE_FRAMES_PER_BLOCK);
            float vol = shadow_chain_slots[s].volume;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                int32_t mixed = shadow_deferred_dsp_buffer[i] + (int32_t)(render_buffer[i] * vol);
                /* Clamp during accumulation */
                if (mixed > 32767) mixed = 32767;
                if (mixed < -32768) mixed = -32768;
                shadow_deferred_dsp_buffer[i] = (int16_t)mixed;
            }
        }
    }

    /* Note: Master FX is applied in mix_from_buffer() AFTER mixing with Move's audio */

    shadow_deferred_dsp_valid = 1;
}

/* Mix from pre-rendered buffer (fast, ~5µs) - called PRE-ioctl
 * Mixes shadow DSP with Move's audio, then applies Master FX to combined audio.
 */
static void shadow_inprocess_mix_from_buffer(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;
    if (!shadow_deferred_dsp_valid) return;  /* No buffer to mix yet */

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);

    /* Mix deferred buffer into mailbox audio */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t mixed = mailbox_audio[i] + shadow_deferred_dsp_buffer[i];
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        mailbox_audio[i] = (int16_t)mixed;
    }

    /* Apply master FX chain to combined audio - process through all 4 slots in series */
    for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
        master_fx_slot_t *s = &shadow_master_fx_slots[fx];
        if (s->instance && s->api && s->api->process_block) {
            s->api->process_block(s->instance, mailbox_audio, FRAMES_PER_BLOCK);
        }
    }

    /* Apply master volume */
    float mv = shadow_master_volume;
    if (mv < 1.0f) {
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            mailbox_audio[i] = (int16_t)(mailbox_audio[i] * mv);
        }
    }
}

/* Shared memory segment names from shadow_constants.h */

#define NUM_AUDIO_BUFFERS 3  /* Triple buffering */

/* Shadow shared memory pointers */
static int16_t *shadow_audio_shm = NULL;    /* Shadow's mixed output */
static int16_t *shadow_movein_shm = NULL;   /* Move's audio for shadow to read */
static uint8_t *shadow_midi_shm = NULL;
static uint8_t *shadow_ui_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;
static shadow_midi_out_t *shadow_midi_out_shm = NULL;  /* MIDI output from shadow UI */
static uint8_t last_shadow_midi_out_ready = 0;

/* Shadow shared memory file descriptors */
static int shm_audio_fd = -1;
static int shm_movein_fd = -1;
static int shm_midi_fd = -1;
static int shm_ui_midi_fd = -1;
static int shm_display_fd = -1;
static int shm_control_fd = -1;
static int shm_ui_fd = -1;
static int shm_param_fd = -1;
static int shm_midi_out_fd = -1;

/* Shadow initialization state */
static int shadow_shm_initialized = 0;

/* Initialize shadow shared memory segments */
static void init_shadow_shm(void)
{
    if (shadow_shm_initialized) return;

    printf("Shadow: Initializing shared memory...\n");

    /* Create/open audio shared memory - triple buffered */
    size_t triple_audio_size = AUDIO_BUFFER_SIZE * NUM_AUDIO_BUFFERS;
    shm_audio_fd = shm_open(SHM_SHADOW_AUDIO, O_CREAT | O_RDWR, 0666);
    if (shm_audio_fd >= 0) {
        ftruncate(shm_audio_fd, triple_audio_size);
        shadow_audio_shm = (int16_t *)mmap(NULL, triple_audio_size,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, shm_audio_fd, 0);
        if (shadow_audio_shm == MAP_FAILED) {
            shadow_audio_shm = NULL;
            printf("Shadow: Failed to mmap audio shm\n");
        } else {
            memset(shadow_audio_shm, 0, triple_audio_size);
        }
    } else {
        printf("Shadow: Failed to create audio shm\n");
    }

    /* Create/open Move audio input shared memory (for shadow to read Move's audio) */
    shm_movein_fd = shm_open(SHM_SHADOW_MOVEIN, O_CREAT | O_RDWR, 0666);
    if (shm_movein_fd >= 0) {
        ftruncate(shm_movein_fd, AUDIO_BUFFER_SIZE);
        shadow_movein_shm = (int16_t *)mmap(NULL, AUDIO_BUFFER_SIZE,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, shm_movein_fd, 0);
        if (shadow_movein_shm == MAP_FAILED) {
            shadow_movein_shm = NULL;
            printf("Shadow: Failed to mmap movein shm\n");
        } else {
            memset(shadow_movein_shm, 0, AUDIO_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create movein shm\n");
    }

    /* Create/open MIDI shared memory */
    shm_midi_fd = shm_open(SHM_SHADOW_MIDI, O_CREAT | O_RDWR, 0666);
    if (shm_midi_fd >= 0) {
        ftruncate(shm_midi_fd, MIDI_BUFFER_SIZE);
        shadow_midi_shm = (uint8_t *)mmap(NULL, MIDI_BUFFER_SIZE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, shm_midi_fd, 0);
        if (shadow_midi_shm == MAP_FAILED) {
            shadow_midi_shm = NULL;
            printf("Shadow: Failed to mmap MIDI shm\n");
        } else {
            memset(shadow_midi_shm, 0, MIDI_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create MIDI shm\n");
    }

    /* Create/open UI MIDI shared memory */
    shm_ui_midi_fd = shm_open(SHM_SHADOW_UI_MIDI, O_CREAT | O_RDWR, 0666);
    if (shm_ui_midi_fd >= 0) {
        ftruncate(shm_ui_midi_fd, MIDI_BUFFER_SIZE);
        shadow_ui_midi_shm = (uint8_t *)mmap(NULL, MIDI_BUFFER_SIZE,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, shm_ui_midi_fd, 0);
        if (shadow_ui_midi_shm == MAP_FAILED) {
            shadow_ui_midi_shm = NULL;
            printf("Shadow: Failed to mmap UI MIDI shm\n");
        } else {
            memset(shadow_ui_midi_shm, 0, MIDI_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create UI MIDI shm\n");
    }

    /* Create/open display shared memory */
    shm_display_fd = shm_open(SHM_SHADOW_DISPLAY, O_CREAT | O_RDWR, 0666);
    if (shm_display_fd >= 0) {
        ftruncate(shm_display_fd, DISPLAY_BUFFER_SIZE);
        shadow_display_shm = (uint8_t *)mmap(NULL, DISPLAY_BUFFER_SIZE,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED, shm_display_fd, 0);
        if (shadow_display_shm == MAP_FAILED) {
            shadow_display_shm = NULL;
            printf("Shadow: Failed to mmap display shm\n");
        } else {
            memset(shadow_display_shm, 0, DISPLAY_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create display shm\n");
    }

    /* Create/open control shared memory - DON'T zero it, shadow_poc owns the state */
    shm_control_fd = shm_open(SHM_SHADOW_CONTROL, O_CREAT | O_RDWR, 0666);
    if (shm_control_fd >= 0) {
        ftruncate(shm_control_fd, CONTROL_BUFFER_SIZE);
        shadow_control = (shadow_control_t *)mmap(NULL, CONTROL_BUFFER_SIZE,
                                                   PROT_READ | PROT_WRITE,
                                                   MAP_SHARED, shm_control_fd, 0);
        if (shadow_control == MAP_FAILED) {
            shadow_control = NULL;
            printf("Shadow: Failed to mmap control shm\n");
        }
        if (shadow_control) {
            /* Avoid sticky shadow state across restarts. */
            shadow_display_mode = 0;
            shadow_control->display_mode = 0;
            shadow_control->should_exit = 0;
            shadow_control->midi_ready = 0;
            shadow_control->write_idx = 0;
            shadow_control->read_idx = 0;
            shadow_control->ui_slot = 0;
            shadow_control->ui_flags = 0;
            shadow_control->ui_patch_index = 0;
            shadow_control->ui_request_id = 0;
        }
    } else {
        printf("Shadow: Failed to create control shm\n");
    }

    /* Create/open UI shared memory (slot labels/state) */
    shm_ui_fd = shm_open(SHM_SHADOW_UI, O_CREAT | O_RDWR, 0666);
    if (shm_ui_fd >= 0) {
        ftruncate(shm_ui_fd, SHADOW_UI_BUFFER_SIZE);
        shadow_ui_state = (shadow_ui_state_t *)mmap(NULL, SHADOW_UI_BUFFER_SIZE,
                                                    PROT_READ | PROT_WRITE,
                                                    MAP_SHARED, shm_ui_fd, 0);
        if (shadow_ui_state == MAP_FAILED) {
            shadow_ui_state = NULL;
            printf("Shadow: Failed to mmap UI shm\n");
        } else {
            memset(shadow_ui_state, 0, SHADOW_UI_BUFFER_SIZE);
            shadow_ui_state->version = 1;
            shadow_ui_state->slot_count = SHADOW_UI_SLOTS;
        }
    } else {
        printf("Shadow: Failed to create UI shm\n");
    }

    /* Create/open param shared memory (for set_param/get_param requests) */
    shm_param_fd = shm_open(SHM_SHADOW_PARAM, O_CREAT | O_RDWR, 0666);
    if (shm_param_fd >= 0) {
        ftruncate(shm_param_fd, SHADOW_PARAM_BUFFER_SIZE);
        shadow_param = (shadow_param_t *)mmap(NULL, SHADOW_PARAM_BUFFER_SIZE,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED, shm_param_fd, 0);
        if (shadow_param == MAP_FAILED) {
            shadow_param = NULL;
            printf("Shadow: Failed to mmap param shm\n");
        } else {
            memset(shadow_param, 0, SHADOW_PARAM_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create param shm\n");
    }

    /* Create/open MIDI out shared memory (for shadow UI to send MIDI) */
    shm_midi_out_fd = shm_open(SHM_SHADOW_MIDI_OUT, O_CREAT | O_RDWR, 0666);
    if (shm_midi_out_fd >= 0) {
        ftruncate(shm_midi_out_fd, sizeof(shadow_midi_out_t));
        shadow_midi_out_shm = (shadow_midi_out_t *)mmap(NULL, sizeof(shadow_midi_out_t),
                                                         PROT_READ | PROT_WRITE,
                                                         MAP_SHARED, shm_midi_out_fd, 0);
        if (shadow_midi_out_shm == MAP_FAILED) {
            shadow_midi_out_shm = NULL;
            printf("Shadow: Failed to mmap midi_out shm\n");
        } else {
            memset(shadow_midi_out_shm, 0, sizeof(shadow_midi_out_t));
        }
    } else {
        printf("Shadow: Failed to create midi_out shm\n");
    }

    shadow_shm_initialized = 1;
    printf("Shadow: Shared memory initialized (audio=%p, midi=%p, ui_midi=%p, display=%p, control=%p, ui=%p, param=%p, midi_out=%p)\n",
           shadow_audio_shm, shadow_midi_shm, shadow_ui_midi_shm,
           shadow_display_shm, shadow_control, shadow_ui_state, shadow_param, shadow_midi_out_shm);
}

#if SHADOW_DEBUG
/* Debug: detailed dump of control regions and offset 256 area */
static void debug_full_mailbox_dump(void) {
    static int dump_count = 0;
    static FILE *dump_file = NULL;

    /* Only dump occasionally */
    if (dump_count++ % 10000 != 0 || dump_count > 50000) return;

    if (!dump_file) {
        dump_file = fopen("/data/UserData/move-anything/mailbox_dump.log", "a");
    }

    if (dump_file && global_mmap_addr) {
        fprintf(dump_file, "\n=== Dump %d ===\n", dump_count);

        /* Dump first 512 bytes in detail (includes offset 256 audio area) */
        fprintf(dump_file, "First 512 bytes (includes audio out @ 256):\n");
        for (int row = 0; row < 512; row += 32) {
            fprintf(dump_file, "%4d: ", row);
            for (int i = 0; i < 32; i++) {
                fprintf(dump_file, "%02x ", global_mmap_addr[row + i]);
            }
            fprintf(dump_file, "\n");
        }

        /* Dump last 128 bytes (offset 3968-4095) for control flags */
        fprintf(dump_file, "\nLast 128 bytes (control region?):\n");
        for (int row = 3968; row < 4096; row += 32) {
            fprintf(dump_file, "%4d: ", row);
            for (int i = 0; i < 32; i++) {
                fprintf(dump_file, "%02x ", global_mmap_addr[row + i]);
            }
            fprintf(dump_file, "\n");
        }
        fflush(dump_file);
    }
}

/* Debug: continuously log non-zero audio regions */
static void debug_audio_offset(void) {
    /* DISABLED - using ioctl logging instead */
    return;
}
#endif /* SHADOW_DEBUG */

/* Mix shadow audio into mailbox audio buffer - TRIPLE BUFFERED */
static void shadow_mix_audio(void)
{
    if (!shadow_audio_shm || !global_mmap_addr) return;
    if (!shadow_control || !shadow_control->shadow_ready) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);

    /* Increment shim counter for shadow's drift correction */
    shadow_control->shim_counter++;

    /* Copy Move's audio to shared memory so shadow can mix it */
    if (shadow_movein_shm) {
        memcpy(shadow_movein_shm, mailbox_audio, AUDIO_BUFFER_SIZE);
    }

    /*
     * Triple buffering read strategy:
     * - Read from buffer that's 2 behind write (gives shadow time to render)
     * - This adds ~6ms latency but smooths out timing jitter
     */
    uint8_t write_idx = shadow_control->write_idx;
    uint8_t read_idx = (write_idx + NUM_AUDIO_BUFFERS - 2) % NUM_AUDIO_BUFFERS;

    /* Update read index for shadow's reference */
    shadow_control->read_idx = read_idx;

    /* Get pointer to the buffer we should read */
    int16_t *src_buffer = shadow_audio_shm + (read_idx * FRAMES_PER_BLOCK * 2);

    /* 0 = mix shadow with Move, 1 = replace Move audio entirely */
    #define SHADOW_AUDIO_REPLACE 0

    #if SHADOW_AUDIO_REPLACE
    /* Replace Move's audio entirely with shadow audio */
    memcpy(mailbox_audio, src_buffer, AUDIO_BUFFER_SIZE);
    #else
    /* Mix shadow audio with Move's audio */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)src_buffer[i];
        /* Clip to int16 range */
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        mailbox_audio[i] = (int16_t)mixed;
    }
    #endif
}

/* Inject shadow UI MIDI out into mailbox before ioctl. */
static void shadow_inject_ui_midi_out(void) {
    if (!shadow_midi_out_shm) return;
    if (shadow_midi_out_shm->ready == last_shadow_midi_out_ready) return;

    last_shadow_midi_out_ready = shadow_midi_out_shm->ready;

    /* Inject into shadow_mailbox at MIDI_OUT_OFFSET */
    uint8_t *midi_out = shadow_mailbox + MIDI_OUT_OFFSET;

    /* Find empty slots and copy packets (original v0.3.22 behavior) */
    int hw_offset = 0;
    for (int i = 0; i < shadow_midi_out_shm->write_idx && i < SHADOW_MIDI_OUT_BUFFER_SIZE; i += 4) {
        /* Find empty 4-byte slot */
        while (hw_offset < MIDI_BUFFER_SIZE) {
            if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                break;
            }
            hw_offset += 4;
        }
        if (hw_offset >= MIDI_BUFFER_SIZE) break;  /* Buffer full */

        memcpy(&midi_out[hw_offset], &shadow_midi_out_shm->buffer[i], 4);
        hw_offset += 4;
    }

    /* Clear after processing */
    shadow_midi_out_shm->write_idx = 0;
    memset(shadow_midi_out_shm->buffer, 0, SHADOW_MIDI_OUT_BUFFER_SIZE);
}

static int shadow_has_midi_packets(const uint8_t *src)
{
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = src[i] & 0x0F;
        if (cin < 0x08 || cin > 0x0E) {
            continue;
        }
        if (src[i + 1] + src[i + 2] + src[i + 3] == 0) {
            continue;
        }
        return 1;
    }
    return 0;
}

static int shadow_append_ui_midi(uint8_t *dst, int offset, const uint8_t *src)
{
    /* Prefer USB-MIDI CC packets if present */
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = src[i] & 0x0F;
        if (cin < 0x08 || cin > 0x0E) continue;
        uint8_t status = src[i + 1];
        uint8_t d1 = src[i + 2];
        uint8_t d2 = src[i + 3];
        if ((status & 0xF0) != 0xB0) continue;
        if (d1 >= 0x80 || d2 >= 0x80) continue;
        if (offset + 4 > MIDI_BUFFER_SIZE) return offset;
        memcpy(dst + offset, src + i, 4);
        offset += 4;
    }

    if (offset > 0) {
        return offset;
    }

    /* Fallback: scan for raw 3-byte CC packets */
    for (int i = 0; i < MIDI_BUFFER_SIZE - 2; i++) {
        uint8_t status = src[i];
        uint8_t d1 = src[i + 1];
        uint8_t d2 = src[i + 2];
        if ((status & 0xF0) != 0xB0) continue;
        if (d1 >= 0x80 || d2 >= 0x80) continue;
        if (offset + 4 > MIDI_BUFFER_SIZE) break;
        dst[offset] = 0x0B; /* USB-MIDI CIN for CC on cable 0 */
        dst[offset + 1] = status;
        dst[offset + 2] = d1;
        dst[offset + 3] = d2;
        offset += 4;
        i += 2;
    }

    return offset;
}

/* Copy incoming MIDI from mailbox to shadow shared memory */
static void shadow_forward_midi(void)
{
    if (!shadow_midi_shm || !global_mmap_addr) return;
    if (!shadow_control) return;

    /* Cache flag file checks - re-check frequently so debug flags take effect quickly. */
    static int cache_counter = 0;
    static int cached_ch3_only = 0;
    static int cached_block_ch1 = 0;
    static int cached_allow_ch5_8 = 0;
    static int cached_notes_only = 0;
    static int cached_allow_cable0 = 0;
    static int cached_drop_cable_f = 0;
    static int cached_log_on = 0;
    static int cached_drop_ui = 0;
    static int cache_initialized = 0;

    /* Only check on first call and then every 200 calls */
    if (!cache_initialized || (cache_counter++ % 200 == 0)) {
        cache_initialized = 1;
        cached_ch3_only = (access("/data/UserData/move-anything/shadow_midi_ch3_only", F_OK) == 0);
        cached_block_ch1 = (access("/data/UserData/move-anything/shadow_midi_block_ch1", F_OK) == 0);
        cached_allow_ch5_8 = (access("/data/UserData/move-anything/shadow_midi_allow_ch5_8", F_OK) == 0);
        cached_notes_only = (access("/data/UserData/move-anything/shadow_midi_notes_only", F_OK) == 0);
        cached_allow_cable0 = (access("/data/UserData/move-anything/shadow_midi_allow_cable0", F_OK) == 0);
        cached_drop_cable_f = (access("/data/UserData/move-anything/shadow_midi_drop_cable_f", F_OK) == 0);
        cached_log_on = (access("/data/UserData/move-anything/shadow_midi_log_on", F_OK) == 0);
        cached_drop_ui = (access("/data/UserData/move-anything/shadow_midi_drop_ui", F_OK) == 0);
    }

    uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;
    int ch3_only = cached_ch3_only;
    int block_ch1 = cached_block_ch1;
    int allow_ch5_8 = cached_allow_ch5_8;
    int notes_only = cached_notes_only;
    int allow_cable0 = cached_allow_cable0;
    int drop_cable_f = cached_drop_cable_f;
    int log_on = cached_log_on;
    int drop_ui = cached_drop_ui;
    static FILE *log = NULL;

    /* Only copy if there's actual MIDI data (check first 64 bytes for non-zero) */
    int has_midi = 0;
    uint8_t filtered[MIDI_BUFFER_SIZE];
    memset(filtered, 0, sizeof(filtered));

    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = src[i] & 0x0F;
        uint8_t cable = (src[i] >> 4) & 0x0F;
        if (cin < 0x08 || cin > 0x0E) {
            continue;
        }
        if (allow_cable0 && cable != 0x00) {
            continue;
        }
        if (drop_cable_f && cable == 0x0F) {
            continue;
        }
        uint8_t status = src[i + 1];
        if (cable == 0x00) {
            uint8_t type = status & 0xF0;
            if (drop_ui) {
                if ((type == 0x90 || type == 0x80) && src[i + 2] < 10) {
                    continue; /* Filter knob-touch notes from internal MIDI */
                }
                if (type == 0xB0) {
                    uint8_t cc = src[i + 2];
                    if ((cc >= CC_STEP_UI_FIRST && cc <= CC_STEP_UI_LAST) ||
                        cc == CC_SHIFT || cc == CC_JOG_CLICK || cc == CC_BACK ||
                        cc == CC_MENU || cc == CC_CAPTURE || cc == CC_UP ||
                        cc == CC_DOWN || cc == CC_UNDO || cc == CC_LOOP ||
                        cc == CC_COPY || cc == CC_LEFT || cc == CC_RIGHT ||
                        cc == CC_KNOB1 || cc == CC_KNOB2 || cc == CC_KNOB3 ||
                        cc == CC_KNOB4 || cc == CC_KNOB5 || cc == CC_KNOB6 ||
                        cc == CC_KNOB7 || cc == CC_KNOB8 || cc == CC_MASTER_KNOB ||
                        cc == CC_PLAY || cc == CC_REC || cc == CC_MUTE ||
                        cc == CC_RECORD || cc == CC_DELETE ||
                        cc == CC_MIC_IN_DETECT || cc == CC_LINE_OUT_DETECT) {
                        continue; /* Filter UI CCs and LED-only controls */
                    }
                }
            }
        }
        if (notes_only) {
            if ((status & 0xF0) != 0x90 && (status & 0xF0) != 0x80) {
                continue;
            }
        }
        if (ch3_only) {
            if ((status & 0x80) == 0) {
                continue;
            }
            if ((status & 0x0F) != 0x02) {
                continue;
            }
        } else if (block_ch1) {
            if ((status & 0x80) != 0 && (status & 0xF0) < 0xF0 && (status & 0x0F) == 0x00) {
                continue;
            }
        } else if (allow_ch5_8) {
            if ((status & 0x80) == 0) {
                continue;
            }
            if ((status & 0xF0) < 0xF0) {
                uint8_t ch = status & 0x0F;
                if (ch < 0x04 || ch > 0x07) {
                    continue;
                }
            }
        }
        filtered[i] = src[i];
        filtered[i + 1] = src[i + 1];
        filtered[i + 2] = src[i + 2];
        filtered[i + 3] = src[i + 3];
        if (log_on) {
            if (!log) {
                log = fopen("/data/UserData/move-anything/shadow_midi_forward.log", "a");
            }
            if (log) {
                fprintf(log, "fwd: idx=%d cable=%u cin=%u status=%02x d1=%02x d2=%02x\n",
                        i, cable, cin, src[i + 1], src[i + 2], src[i + 3]);
                fflush(log);
            }
        }
        has_midi = 1;
    }

    if (has_midi) {
        memcpy(shadow_midi_shm, filtered, MIDI_BUFFER_SIZE);
        shadow_control->midi_ready++;
    }
}

static int shadow_is_transport_cc(uint8_t cc)
{
    return cc == CC_PLAY || cc == CC_REC || cc == CC_MUTE || cc == CC_RECORD;
}

static int shadow_is_hotkey_event(uint8_t status, uint8_t data1)
{
    uint8_t type = status & 0xF0;
    if (type == 0xB0) {
        return data1 == 0x31; /* Shift */
    }
    if (type == 0x90 || type == 0x80) {
        return data1 == 0x00 || data1 == 0x08; /* Knob 1 / Volume touch */
    }
    return 0;
}

static void shadow_capture_midi_for_ui(void)
{
    if (!shadow_ui_midi_shm || !shadow_control || !global_mmap_addr) return;
    if (!shadow_display_mode) return;

    uint8_t *src_in = global_mmap_addr + MIDI_IN_OFFSET;
    uint8_t *src_out = global_mmap_addr + MIDI_OUT_OFFSET;
    uint8_t merged[MIDI_BUFFER_SIZE];
    memset(merged, 0, sizeof(merged));

    int offset = 0;
    offset = shadow_append_ui_midi(merged, offset, src_in);
    if (offset == 0) {
        offset = shadow_append_ui_midi(merged, offset, src_out);
    }

    if (offset == 0) {
        return;
    }
    /* Note: removed deduplication check that was blocking repeated jog wheel events.
     * Jog sends the same CC value (e.g. 1 for clockwise) on each frame while turning,
     * and the dedup was comparing entire buffers, blocking all but the first event. */
    memcpy(shadow_ui_midi_shm, merged, MIDI_BUFFER_SIZE);
    shadow_control->midi_ready++;
}


/* Get capture rules for the focused slot (0-3 = chain, 4 = master FX) */
static const shadow_capture_rules_t *shadow_get_focused_capture(void)
{
    if (!shadow_control) return NULL;
    
    int slot = shadow_control->ui_slot;
    if (slot == SHADOW_CHAIN_INSTANCES) {
        /* Master FX is focused (slot 4) */
        return &shadow_master_fx_capture;
    }
    if (slot >= 0 && slot < SHADOW_CHAIN_INSTANCES) {
        return &shadow_chain_slots[slot].capture;
    }
    return NULL;
}

/* Route captured MIDI to the focused slot's DSP */
static void shadow_route_captured_to_focused(const uint8_t *msg, int len)
{
    if (!shadow_control || !shadow_inprocess_ready || len < 3) return;

    int slot = shadow_control->ui_slot;
    if (slot == SHADOW_CHAIN_INSTANCES) {
        /* Master FX is focused - route to master FX if it supports on_midi */
        if (shadow_master_fx && shadow_master_fx_instance && shadow_master_fx->on_midi) {
            shadow_master_fx->on_midi(shadow_master_fx_instance, msg, len,
                                      MOVE_MIDI_SOURCE_INTERNAL);
        }
    } else if (slot >= 0 && slot < SHADOW_CHAIN_INSTANCES) {
        /* Chain slot - route to chain instance */
        if (shadow_chain_slots[slot].active && shadow_chain_slots[slot].instance) {
            if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                shadow_plugin_v2->on_midi(shadow_chain_slots[slot].instance, msg, len,
                                          MOVE_MIDI_SOURCE_INTERNAL);
            }
        }
    }
}

static int shadow_filter_logged = 0;

static void shadow_filter_move_input(void)
{
    /* Log once when first called with shadow mode active */
    if (!shadow_filter_logged && shadow_display_mode) {
        shadow_filter_logged = 1;
        int slot = shadow_control ? shadow_control->ui_slot : -1;
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "shadow_filter_move_input: ACTIVE, focused_slot=%d", slot);
        capture_debug_log(dbg);
    }

    if (!shadow_control || !shadow_display_mode) return;
    if (!global_mmap_addr) return;

    uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;
    const shadow_capture_rules_t *capture = shadow_get_focused_capture();
    int overtake_mode = shadow_control->overtake_mode;

    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = src[i] & 0x0F;
        uint8_t cable = (src[i] >> 4) & 0x0F;
        if (cin < 0x08 || cin > 0x0E) {
            continue;
        }

        uint8_t status = src[i + 1];
        uint8_t type = status & 0xF0;
        uint8_t d1 = src[i + 2];
        uint8_t d2 = src[i + 3];

        /* Pass through non-cable-0 events (external MIDI) */
        if (cable != 0x00) {
            continue;
        }

        /* In overtake mode, forward ALL MIDI to shadow UI and block from Move */
        if (overtake_mode) {
            /* Forward to shadow UI */
            if (shadow_ui_midi_shm) {
                for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                    if (shadow_ui_midi_shm[slot] == 0) {
                        shadow_ui_midi_shm[slot] = src[i];
                        shadow_ui_midi_shm[slot + 1] = status;
                        shadow_ui_midi_shm[slot + 2] = d1;
                        shadow_ui_midi_shm[slot + 3] = d2;
                        shadow_control->midi_ready++;
                        break;
                    }
                }
            }
            /* Block ALL from Move in overtake mode */
            src[i] = 0;
            src[i + 1] = 0;
            src[i + 2] = 0;
            src[i + 3] = 0;
            continue;
        }

        /* Handle CC events */
        if (type == 0xB0) {
            /* Shadow UI needs: jog (14), jog click (3), back (51), knobs (71-78) */
            int is_shadow_ui_cc = (d1 == 14 || d1 == 3 || d1 == 51 ||
                                   (d1 >= 71 && d1 <= 78));

            if (is_shadow_ui_cc) {
                /* Forward to shadow UI */
                if (shadow_ui_midi_shm) {
                    for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                        if (shadow_ui_midi_shm[slot] == 0) {
                            shadow_ui_midi_shm[slot] = 0x0B;
                            shadow_ui_midi_shm[slot + 1] = status;
                            shadow_ui_midi_shm[slot + 2] = d1;
                            shadow_ui_midi_shm[slot + 3] = d2;
                            shadow_control->midi_ready++;
                            break;
                        }
                    }
                }
                /* Knob CCs (71-78) are handled by Shadow UI via set_param based on ui_hierarchy.
                 * No direct DSP routing - params are changed through the hierarchy system. */
                /* Block from Move */
                src[i] = 0;
                src[i + 1] = 0;
                src[i + 2] = 0;
                src[i + 3] = 0;
                continue;
            }

            /* Check if this CC is captured by the focused slot */
            if (capture && capture_has_cc(capture, d1)) {
                uint8_t msg[3] = { status, d1, d2 };
                shadow_route_captured_to_focused(msg, 3);
                /* Block from Move */
                src[i] = 0;
                src[i + 1] = 0;
                src[i + 2] = 0;
                src[i + 3] = 0;
                continue;
            }

            /* Not a shadow UI CC and not captured - pass through to Move */
            continue;
        }

        /* Handle note events */
        if (type == 0x90 || type == 0x80) {
            /* Knob touch notes 0-7 pass through to Move for touch-to-peek in Chain UI.
             * Only block note 9 (jog wheel touch - not needed). */
            if (d1 == 9) {  /* Jog wheel touch - not needed */
                src[i] = 0;
                src[i + 1] = 0;
                src[i + 2] = 0;
                src[i + 3] = 0;
                continue;
            }

            /* Check if this note is captured by the focused slot.
             * Never route knob touch notes (0-9) to DSP even if in capture rules. */
            if (capture && d1 >= 10 && capture_has_note(capture, d1)) {
                /* Debug: log captured step notes */
                if (d1 >= 16 && d1 <= 31) {
                    char dbg[128];
                    snprintf(dbg, sizeof(dbg), "CAPTURED step note %d, routing to DSP", d1);
                    capture_debug_log(dbg);
                }
                uint8_t msg[3] = { status, d1, d2 };
                shadow_route_captured_to_focused(msg, 3);
                /* Block from Move */
                src[i] = 0;
                src[i + 1] = 0;
                src[i + 2] = 0;
                src[i + 3] = 0;
                continue;
            }

            /* Not captured - pass through to Move */
            /* Debug: log first few step notes passing through */
            if (d1 >= 16 && d1 <= 31) {
                static int passthrough_count = 0;
                if (passthrough_count < 5) {
                    passthrough_count++;
                    int slot = shadow_control ? shadow_control->ui_slot : -1;
                    char dbg[128];
                    snprintf(dbg, sizeof(dbg), "Step note %d PASSTHROUGH: focused_slot=%d capture=%s",
                             d1, slot, capture ? "yes" : "no");
                    capture_debug_log(dbg);
                }
            }
            continue;
        }

        /* Pass through all other MIDI (aftertouch, pitch bend, etc.) */
    }
}

static int is_usb_midi_data(uint8_t cin)
{
    return cin >= 0x08 && cin <= 0x0E;
}

/* Scan mailbox for raw MIDI status bytes (e.g., 0x92 for channel 3 note-on). */
static FILE *midi_scan_log = NULL;
static void shadow_scan_mailbox_raw(void)
{
    if (!global_mmap_addr) return;

    static int scan_enabled = -1;
    static int scan_check_counter = 0;
    if (scan_check_counter++ % 200 == 0 || scan_enabled < 0) {
        scan_enabled = (access("/data/UserData/move-anything/midi_scan_on", F_OK) == 0);
    }

    if (!scan_enabled) return;

    if (!midi_scan_log) {
        midi_scan_log = fopen("/data/UserData/move-anything/midi_scan.log", "a");
    }

    if (!midi_scan_log) return;

    for (int i = 0; i < MIDI_BUFFER_SIZE - 2; i++) {
        uint8_t status = global_mmap_addr[MIDI_OUT_OFFSET + i];
        if (status == 0x92 || status == 0x82) {
            fprintf(midi_scan_log, "OUT[%d]: %02x %02x %02x\n",
                    i, status,
                    global_mmap_addr[MIDI_OUT_OFFSET + i + 1],
                    global_mmap_addr[MIDI_OUT_OFFSET + i + 2]);
        }
    }

    for (int i = 0; i < MIDI_BUFFER_SIZE - 2; i++) {
        uint8_t status = global_mmap_addr[MIDI_IN_OFFSET + i];
        if (status == 0x92 || status == 0x82) {
            fprintf(midi_scan_log, "IN [%d]: %02x %02x %02x\n",
                    i, status,
                    global_mmap_addr[MIDI_IN_OFFSET + i + 1],
                    global_mmap_addr[MIDI_IN_OFFSET + i + 2]);
        }
    }

    fflush(midi_scan_log);
}

/* Log outgoing/incoming MIDI packets with valid CIN for probing */
static FILE *midi_probe_log = NULL;
static void shadow_capture_midi_probe(void)
{
    if (!global_mmap_addr) return;

    static int probe_enabled = -1;
    static int probe_check_counter = 0;
    if (probe_check_counter++ % 200 == 0 || probe_enabled < 0) {
        probe_enabled = (access("/data/UserData/move-anything/midi_probe_on", F_OK) == 0);
    }
    if (!probe_enabled) return;

    if (!midi_probe_log) {
        midi_probe_log = fopen("/data/UserData/move-anything/midi_probe.log", "a");
    }

    uint8_t *out_src = global_mmap_addr + MIDI_OUT_OFFSET;
    uint8_t *in_src = global_mmap_addr + MIDI_IN_OFFSET;

    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t *out_pkt = &out_src[i];
        uint8_t *in_pkt = &in_src[i];
        uint8_t out_cin = out_pkt[0] & 0x0F;
        uint8_t in_cin = in_pkt[0] & 0x0F;

        if (midi_probe_log && is_usb_midi_data(out_cin)) {
            fprintf(midi_probe_log, "OUT[%d]: %02x %02x %02x %02x\n",
                    i, out_pkt[0], out_pkt[1], out_pkt[2], out_pkt[3]);
        }
        if (midi_probe_log && is_usb_midi_data(out_pkt[1] & 0x0F)) {
            fprintf(midi_probe_log, "OUT1[%d]: %02x %02x %02x %02x\n",
                    i, out_pkt[0], out_pkt[1], out_pkt[2], out_pkt[3]);
        }
        if (midi_probe_log && is_usb_midi_data(in_cin)) {
            fprintf(midi_probe_log, "IN [%d]: %02x %02x %02x %02x\n",
                    i, in_pkt[0], in_pkt[1], in_pkt[2], in_pkt[3]);
        }
        if (midi_probe_log && is_usb_midi_data(in_pkt[1] & 0x0F)) {
            fprintf(midi_probe_log, "IN1[%d]: %02x %02x %02x %02x\n",
                    i, in_pkt[0], in_pkt[1], in_pkt[2], in_pkt[3]);
        }
    }

    if (midi_probe_log) {
        fflush(midi_probe_log);
    }
}

/* Swap display buffer if in shadow mode */
static void shadow_swap_display(void)
{
    static uint32_t ui_check_counter = 0;

    if (!shadow_display_shm || !global_mmap_addr) {
        return;
    }
    if (!shadow_control || !shadow_control->shadow_ready) {
        return;
    }
    if (!shadow_display_mode) {
        return;  /* Not in shadow mode */
    }
    if ((ui_check_counter++ % 256) == 0) {
        launch_shadow_ui();
    }

    /* Write full display to DISPLAY_OFFSET (768) */
    memcpy(global_mmap_addr + DISPLAY_OFFSET, shadow_display_shm, DISPLAY_BUFFER_SIZE);

    /* Write display using slice protocol - one slice per ioctl */
    /* No rate limiting because we must overwrite Move every ioctl */
    static int display_phase = 0;  /* 0-6: phases of display push */

    if (display_phase == 0) {
        /* Phase 0: Zero out slice area - signals start of new frame */
        global_mmap_addr[80] = 0;
        memset(global_mmap_addr + 84, 0, 172);
    } else {
        /* Phases 1-6: Write slices 0-5 */
        int slice = display_phase - 1;
        int slice_offset = slice * 172;
        int slice_bytes = (slice == 5) ? 164 : 172;
        global_mmap_addr[80] = slice + 1;
        memcpy(global_mmap_addr + 84, shadow_display_shm + slice_offset, slice_bytes);
    }

    display_phase = (display_phase + 1) % 7;  /* Cycle 0,1,2,3,4,5,6,0,... */
}

void print_mem()
{
    printf("\033[H\033[J");
    for (int i = 0; i < 4096; ++i)
    {
        printf("%02x ", (unsigned char)global_mmap_addr[i]);
        if (i == 2048 - 1)
        {
            printf("\n\n");
        }

        if (i == 2048 + 256 - 1)
        {
            printf("\n\n");
        }

        if (i == 2048 + 256 + 512 - 1)
        {
            printf("\n\n");
        }
    }
    printf("\n\n");
}

void write_mem()
{
    if (!output_file)
    {
        return;
    }

    // printf("\033[H\033[J");
    fprintf(output_file, "--------------------------------------------------------------------------------------------------------------");
    fprintf(output_file, "Frame: %d\n", frame_counter);
    for (int i = 0; i < 4096; ++i)
    {
        fprintf(output_file, "%02x ", (unsigned char)global_mmap_addr[i]);
        if (i == 2048 - 1)
        {
            fprintf(output_file, "\n\n");
        }

        if (i == 2048 + 256 - 1)
        {
            fprintf(output_file, "\n\n");
        }

        if (i == 2048 + 256 + 512 - 1)
        {
            fprintf(output_file, "\n\n");
        }
    }
    fprintf(output_file, "\n\n");

    sync();

    frame_counter++;
}

void *(*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;
static int (*real_open)(const char *pathname, int flags, ...) = NULL;
static int (*real_openat)(int dirfd, const char *pathname, int flags, ...) = NULL;
static int (*real_open64)(const char *pathname, int flags, ...) = NULL;
static int (*real_openat64)(int dirfd, const char *pathname, int flags, ...) = NULL;
static int (*real_close)(int fd) = NULL;
static ssize_t (*real_write)(int fd, const void *buf, size_t count) = NULL;
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{

    printf(">>>>>>>>>>>>>>>>>>>>>>>> Hooked mmap...\n");
    if (!real_mmap)
    {
        real_mmap = dlsym(RTLD_NEXT, "mmap");
        if (!real_mmap)
        {
            fprintf(stderr, "Error: dlsym failed to find mmap\n");
            exit(1);
        }
    }

    void *result = real_mmap(addr, length, prot, flags, fd, offset);

    if (length == 4096)
    {
        /* Store the real hardware mailbox address */
        hardware_mmap_addr = result;

        /* Give Move our shadow buffer instead - we'll sync in ioctl hook */
        global_mmap_addr = shadow_mailbox;
        memset(shadow_mailbox, 0, sizeof(shadow_mailbox));

        printf("Shadow mailbox: Move sees %p, hardware at %p\n",
               (void*)shadow_mailbox, result);

        /* Initialize shadow shared memory when we detect the SPI mailbox */
        init_shadow_shm();
#if SHADOW_INPROCESS_POC
        shadow_inprocess_load_chain();
        shadow_dbus_start();  /* Start D-Bus monitoring for volume sync */
        shadow_read_initial_volume();  /* Read initial master volume from settings */
        shadow_load_state();  /* Load saved slot volumes */
#endif

        /* Return shadow buffer to Move instead of hardware address */
        printf("mmap hooked! addr=%p, length=%zu, prot=%d, flags=%d, fd=%d, offset=%lld, result=%p (returning shadow)\n",
               addr, length, prot, flags, fd, (long long)offset, result);
        return shadow_mailbox;
    }

    printf("mmap hooked! addr=%p, length=%zu, prot=%d, flags=%d, fd=%d, offset=%lld, result=%p\n",
           addr, length, prot, flags, fd, (long long)offset, result);

    return result;
}

static int open_common(const char *pathname, int flags, va_list ap, int use_openat, int dirfd)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = (mode_t)va_arg(ap, int);
    }

    int fd;
    if (use_openat) {
        if (!real_openat) {
            real_openat = dlsym(RTLD_NEXT, "openat");
        }
        fd = real_openat ? real_openat(dirfd, pathname, flags, mode) : -1;
    } else {
        if (!real_open) {
            real_open = dlsym(RTLD_NEXT, "open");
        }
        fd = real_open ? real_open(pathname, flags, mode) : -1;
    }

    if (fd >= 0 && (path_matches_midi(pathname) || path_matches_spi(pathname))) {
        track_fd(fd, pathname);
        if (path_matches_midi(pathname) && trace_midi_fd_enabled()) {
            midi_fd_trace_log_open();
            if (midi_fd_trace_log) {
                fprintf(midi_fd_trace_log, "OPEN fd=%d path=%s\n", fd, pathname);
                fflush(midi_fd_trace_log);
            }
        }
        if (path_matches_spi(pathname) && trace_spi_io_enabled()) {
            spi_io_log_open();
            if (spi_io_log) {
                fprintf(spi_io_log, "OPEN fd=%d path=%s\n", fd, pathname);
                fflush(spi_io_log);
            }
        }
    }

    return fd;
}

int open(const char *pathname, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int fd = open_common(pathname, flags, ap, 0, AT_FDCWD);
    va_end(ap);
    return fd;
}

int open64(const char *pathname, int flags, ...)
{
    if (!real_open64) {
        real_open64 = dlsym(RTLD_NEXT, "open64");
    }
    va_list ap;
    va_start(ap, flags);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = (mode_t)va_arg(ap, int);
    }
    int fd = real_open64 ? real_open64(pathname, flags, mode) : -1;
    va_end(ap);
    if (fd >= 0 && (path_matches_midi(pathname) || path_matches_spi(pathname))) {
        track_fd(fd, pathname);
        if (path_matches_midi(pathname) && trace_midi_fd_enabled()) {
            midi_fd_trace_log_open();
            if (midi_fd_trace_log) {
                fprintf(midi_fd_trace_log, "OPEN64 fd=%d path=%s\n", fd, pathname);
                fflush(midi_fd_trace_log);
            }
        }
        if (path_matches_spi(pathname) && trace_spi_io_enabled()) {
            spi_io_log_open();
            if (spi_io_log) {
                fprintf(spi_io_log, "OPEN64 fd=%d path=%s\n", fd, pathname);
                fflush(spi_io_log);
            }
        }
    }
    return fd;
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int fd = open_common(pathname, flags, ap, 1, dirfd);
    va_end(ap);
    return fd;
}

int openat64(int dirfd, const char *pathname, int flags, ...)
{
    if (!real_openat64) {
        real_openat64 = dlsym(RTLD_NEXT, "openat64");
    }
    va_list ap;
    va_start(ap, flags);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = (mode_t)va_arg(ap, int);
    }
    int fd = real_openat64 ? real_openat64(dirfd, pathname, flags, mode) : -1;
    va_end(ap);
    if (fd >= 0 && (path_matches_midi(pathname) || path_matches_spi(pathname))) {
        track_fd(fd, pathname);
        if (path_matches_midi(pathname) && trace_midi_fd_enabled()) {
            midi_fd_trace_log_open();
            if (midi_fd_trace_log) {
                fprintf(midi_fd_trace_log, "OPENAT64 fd=%d path=%s\n", fd, pathname);
                fflush(midi_fd_trace_log);
            }
        }
        if (path_matches_spi(pathname) && trace_spi_io_enabled()) {
            spi_io_log_open();
            if (spi_io_log) {
                fprintf(spi_io_log, "OPENAT64 fd=%d path=%s\n", fd, pathname);
                fflush(spi_io_log);
            }
        }
    }
    return fd;
}

int close(int fd)
{
    if (!real_close) {
        real_close = dlsym(RTLD_NEXT, "close");
    }
    const char *path = tracked_path_for_fd(fd);
    if (path && path_matches_midi(path) && trace_midi_fd_enabled()) {
        midi_fd_trace_log_open();
        if (midi_fd_trace_log) {
            fprintf(midi_fd_trace_log, "CLOSE fd=%d path=%s\n", fd, path);
            fflush(midi_fd_trace_log);
        }
    }
    if (path && path_matches_spi(path) && trace_spi_io_enabled()) {
        spi_io_log_open();
        if (spi_io_log) {
            fprintf(spi_io_log, "CLOSE fd=%d path=%s\n", fd, path);
            fflush(spi_io_log);
        }
    }
    untrack_fd(fd);
    return real_close ? real_close(fd) : -1;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    if (!real_write) {
        real_write = dlsym(RTLD_NEXT, "write");
    }
    const char *path = tracked_path_for_fd(fd);
    if (path && buf && count > 0) {
        log_fd_bytes("WRITE", fd, path, (const unsigned char *)buf, count);
    }
    return real_write ? real_write(fd, buf, count) : -1;
}

ssize_t read(int fd, void *buf, size_t count)
{
    if (!real_read) {
        real_read = dlsym(RTLD_NEXT, "read");
    }
    ssize_t ret = real_read ? real_read(fd, buf, count) : -1;
    const char *path = tracked_path_for_fd(fd);
    if (path && buf && ret > 0) {
        log_fd_bytes("READ ", fd, path, (const unsigned char *)buf, (size_t)ret);
    }
    return ret;
}

void launchChildAndKillThisProcess(char *pBinPath, char*pBinName, char* pArgs)
{
    int pid = fork();

    if (pid < 0)
    {
        printf("Fork failed\n");
        exit(1);
    }
    else if (pid == 0)
    {
        // Child process
        setsid();
        // Perform detached task
        printf("Child process running in the background...\n");

        printf("Args: %s\n", pArgs);

        // Close all file descriptors, otherwise /dev/ablspi0.0 is held open
        // and the control surface code can't open it.
        printf("Closing file descriptors...\n");
        int fdlimit = (int)sysconf(_SC_OPEN_MAX);
        for (int i = STDERR_FILENO + 1; i < fdlimit; i++)
        {
            close(i);
        }

        // Let's a go!
        int ret = execl(pBinPath, pBinName, pArgs, (char *)0);
    }
    else
    {
        // parent
        kill(getpid(), SIGINT);
    }
}

static int shadow_ui_started = 0;
static pid_t shadow_ui_pid = -1;
static const char *shadow_ui_pid_path = "/data/UserData/move-anything/shadow_ui.pid";

static int shadow_ui_pid_alive(pid_t pid)
{
    if (pid <= 0) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int rpid = 0;
    char comm[64] = {0};
    char state = 0;
    int matched = fscanf(f, "%d %63s %c", &rpid, comm, &state);
    fclose(f);
    if (matched != 3) return 0;
    if (rpid != (int)pid) return 0;
    if (state == 'Z') return 0;
    return 1;
}

static pid_t shadow_ui_read_pid(void)
{
    FILE *pid_file = fopen(shadow_ui_pid_path, "r");
    if (!pid_file) return -1;
    long pid = -1;
    if (fscanf(pid_file, "%ld", &pid) != 1) {
        pid = -1;
    }
    fclose(pid_file);
    return (pid_t)pid;
}

static void shadow_ui_refresh_pid(void)
{
    if (shadow_ui_pid_alive(shadow_ui_pid)) {
        shadow_ui_started = 1;
        return;
    }
    pid_t pid = shadow_ui_read_pid();
    if (shadow_ui_pid_alive(pid)) {
        shadow_ui_pid = pid;
        shadow_ui_started = 1;
        return;
    }
    if (pid > 0) {
        unlink(shadow_ui_pid_path);
    }
    shadow_ui_pid = -1;
    shadow_ui_started = 0;
}

static void shadow_ui_reap(void)
{
    if (shadow_ui_pid <= 0) return;
    int status = 0;
    pid_t res = waitpid(shadow_ui_pid, &status, WNOHANG);
    if (res == shadow_ui_pid) {
        shadow_ui_pid = -1;
        shadow_ui_started = 0;
    }
}

static void launch_shadow_ui(void)
{
    shadow_ui_reap();
    shadow_ui_refresh_pid();
    if (shadow_ui_started && shadow_ui_pid_alive(shadow_ui_pid)) return;
    if (access("/data/UserData/move-anything/shadow/shadow_ui", X_OK) != 0) {
        return;
    }

    int pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        setsid();
        int fdlimit = (int)sysconf(_SC_OPEN_MAX);
        for (int i = STDERR_FILENO + 1; i < fdlimit; i++) {
            close(i);
        }
        execl("/data/UserData/move-anything/shadow/shadow_ui", "shadow_ui", (char *)0);
        _exit(1);
    }
    shadow_ui_started = 1;
    shadow_ui_pid = pid;
}

int (*real_ioctl)(int, unsigned long, ...) = NULL;

int shiftHeld = 0;
int volumeTouched = 0;
int wheelTouched = 0;
int knob8touched = 0;
int alreadyLaunched = 0;       /* Prevent multiple launches */

/* Debug logging disabled for performance - set to 1 to enable */
#define SHADOW_HOTKEY_DEBUG 0
#if SHADOW_HOTKEY_DEBUG
static FILE *hotkey_state_log = NULL;
#endif
static uint64_t shift_on_ms = 0;
static uint64_t vol_on_ms = 0;
static uint8_t hotkey_prev[MIDI_BUFFER_SIZE];
static int hotkey_prev_valid = 0;
static int shift_armed = 1;   /* Start armed so first press works */
static int volume_armed = 1;  /* Start armed so first press works */

static void log_hotkey_state(const char *tag);

static uint64_t now_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static int within_window(uint64_t now, uint64_t ts, uint64_t window_ms)
{
    return ts > 0 && now >= ts && (now - ts) <= window_ms;
}

#define SHADOW_HOTKEY_WINDOW_MS 1500
#define SHADOW_HOTKEY_GRACE_MS 2000
static uint64_t shadow_hotkey_enable_ms = 0;
static int shadow_inject_knob_release = 0;  /* Set when toggling shadow mode to inject note-offs */

/* Shift+Vol+Knob1 toggle removed - use Track buttons or Shift+Jog instead */

static void log_hotkey_state(const char *tag)
{
#if SHADOW_HOTKEY_DEBUG
    if (!hotkey_state_log)
    {
        hotkey_state_log = fopen("/data/UserData/move-anything/hotkey_state.log", "a");
    }
    if (hotkey_state_log)
    {
        time_t now = time(NULL);
        fprintf(hotkey_state_log, "%ld %s shift=%d vol=%d knob8=%d\n",
                (long)now, tag, shiftHeld, volumeTouched, knob8touched);
        fflush(hotkey_state_log);
    }
#else
    (void)tag;
#endif
}

void midi_monitor()
{
    if (!global_mmap_addr)
    {
        return;
    }

    uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;

    /* NOTE: Shadow mode MIDI filtering now happens AFTER ioctl in the ioctl() function.
     * This function only handles hotkey detection for shadow mode toggle. */

    if (!hotkey_prev_valid) {
        memcpy(hotkey_prev, src, MIDI_BUFFER_SIZE);
        hotkey_prev_valid = 1;
        return;
    }

    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4)
    {
        if (memcmp(&src[i], &hotkey_prev[i], 4) == 0) {
            continue;
        }
        memcpy(&hotkey_prev[i], &src[i], 4);

        unsigned char *byte = &src[i];
        unsigned char cable = (*byte & 0b11110000) >> 4;
        unsigned char code_index_number = (*byte & 0b00001111);
        unsigned char midi_0 = *(byte + 1);
        unsigned char midi_1 = *(byte + 2);
        unsigned char midi_2 = *(byte + 3);

        if (code_index_number == 2 || code_index_number == 1 || (cable == 0xf && code_index_number == 0xb && midi_0 == 176))
        {
            continue;
        }

        if (midi_0 + midi_1 + midi_2 == 0)
        {
            continue;
        }

        int controlMessage = 0xb0;
        if (midi_0 == controlMessage)
        {
            if (midi_1 == 0x31)
            {
                if (midi_2 == 0x7f)
                {
#if SHADOW_HOTKEY_DEBUG
                    printf("Shift on\n");
#endif

                    if (!shiftHeld && shift_armed) {
                        shiftHeld = 1;
                        shadow_shift_held = 1;  /* Sync global for cross-function access */
                        if (shadow_control) shadow_control->shift_held = 1;
                        shift_on_ms = now_mono_ms();
                        log_hotkey_state("shift_on");
                                            }
                }
                else
                {
#if SHADOW_HOTKEY_DEBUG
                    printf("Shift off\n");
#endif

                    shiftHeld = 0;
                    shadow_shift_held = 0;  /* Sync global for cross-function access */
                    if (shadow_control) shadow_control->shift_held = 0;
                    shift_armed = 1;
                    shift_on_ms = 0;
                    log_hotkey_state("shift_off");
                }
            }

        }

        if ((midi_0 & 0xF0) == 0x90 && midi_1 == 0x07)
        {
            if (midi_2 == 0x7f)
            {
                if (!knob8touched) {
                    knob8touched = 1;
#if SHADOW_HOTKEY_DEBUG
                    printf("Knob 8 touch start\n");
#endif
                    log_hotkey_state("knob8_on");
                }
            }
            else
            {
                knob8touched = 0;
#if SHADOW_HOTKEY_DEBUG
                printf("Knob 8 touch stop\n");
#endif
                log_hotkey_state("knob8_off");
            }
        }

        if ((midi_0 & 0xF0) == 0x90 && midi_1 == 0x08)
        {
            if (midi_2 == 0x7f)
            {
                if (!volumeTouched && volume_armed) {
                    volumeTouched = 1;
                    shadow_volume_knob_touched = 1;  /* Sync global for cross-function access */
                    vol_on_ms = now_mono_ms();
                    log_hotkey_state("vol_on");
                }
            }
            else
            {
                volumeTouched = 0;
                shadow_volume_knob_touched = 0;  /* Sync global for cross-function access */
                volume_armed = 1;
                vol_on_ms = 0;
                log_hotkey_state("vol_off");
            }
        }

        if ((midi_0 & 0xF0) == 0x90 && midi_1 == 0x09)
        {
            if (midi_2 == 0x7f)
            {
                wheelTouched = 1;
            }
            else
            {
                wheelTouched = 0;
            }
        }

        if (shiftHeld && volumeTouched && knob8touched && !alreadyLaunched)
        {
            alreadyLaunched = 1;
            printf("Launching Move Anything!\n");
            launchChildAndKillThisProcess("/data/UserData/move-anything/start.sh", "start.sh", "");
        }

    }
}

// unsigned long ioctlCounter = 0;
int ioctl(int fd, unsigned long request, ...)
{
    if (!real_ioctl)
    {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl)
        {
            fprintf(stderr, "Error: dlsym failed to find ioctl\n");
            exit(1);
        }
    }

    va_list ap;
    void *argp = NULL;
    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);

    /* === COMPREHENSIVE IOCTL TIMING === */
    static struct timespec ioctl_start, pre_end, post_start, ioctl_end;
    static uint64_t total_sum = 0, pre_sum = 0, ioctl_sum = 0, post_sum = 0;
    static uint64_t total_max = 0, pre_max = 0, ioctl_max = 0, post_max = 0;
    static int timing_count = 0;
    static int baseline_mode = -1;  /* -1 = unknown, 0 = full mode, 1 = baseline only */

    /* === GRANULAR PRE-IOCTL TIMING === */
    static struct timespec section_start, section_end;
    static uint64_t midi_mon_sum = 0, midi_mon_max = 0;
    static uint64_t fwd_midi_sum = 0, fwd_midi_max = 0;
    static uint64_t mix_audio_sum = 0, mix_audio_max = 0;
    static uint64_t ui_req_sum = 0, ui_req_max = 0;
    static uint64_t param_req_sum = 0, param_req_max = 0;
    static uint64_t proc_midi_sum = 0, proc_midi_max = 0;
    static uint64_t inproc_mix_sum = 0, inproc_mix_max = 0;
    static uint64_t display_sum = 0, display_max = 0;
    static int granular_count = 0;

#define TIME_SECTION_START() clock_gettime(CLOCK_MONOTONIC, &section_start)
#define TIME_SECTION_END(sum_var, max_var) do { \
    clock_gettime(CLOCK_MONOTONIC, &section_end); \
    uint64_t _section_us = (section_end.tv_sec - section_start.tv_sec) * 1000000 + \
                   (section_end.tv_nsec - section_start.tv_nsec) / 1000; \
    sum_var += _section_us; \
    if (_section_us > max_var) max_var = _section_us; \
} while(0)

    /* === OVERRUN DETECTION AND RECOVERY === */
    static int consecutive_overruns = 0;
    static int skip_dsp_this_frame = 0;
    static uint64_t last_frame_total_us = 0;
#define OVERRUN_THRESHOLD_US 2850  /* Start worrying at 2850µs (98% of budget) */
#define SKIP_DSP_THRESHOLD 3       /* Skip DSP after 3 consecutive overruns */

    /* Check for baseline timing mode (set SHADOW_BASELINE=1 to disable all processing) */
    if (baseline_mode < 0) {
        const char *env = getenv("SHADOW_BASELINE");
        baseline_mode = (env && env[0] == '1') ? 1 : 0;
#if SHADOW_TIMING_LOG
        if (baseline_mode) {
            FILE *f = fopen("/tmp/ioctl_timing.log", "a");
            if (f) { fprintf(f, "=== BASELINE MODE: All processing disabled ===\n"); fclose(f); }
        }
#endif
    }

    clock_gettime(CLOCK_MONOTONIC, &ioctl_start);

    /* Check if previous frame overran - if so, consider skipping expensive work */
    if (last_frame_total_us > OVERRUN_THRESHOLD_US) {
        consecutive_overruns++;
        if (consecutive_overruns >= SKIP_DSP_THRESHOLD) {
            skip_dsp_this_frame = 1;
#if SHADOW_TIMING_LOG
            static int skip_log_count = 0;
            if (skip_log_count++ < 10 || skip_log_count % 100 == 0) {
                FILE *f = fopen("/tmp/ioctl_timing.log", "a");
                if (f) {
                    fprintf(f, "SKIP_DSP: consecutive_overruns=%d, last_frame=%llu us\n",
                            consecutive_overruns, (unsigned long long)last_frame_total_us);
                    fclose(f);
                }
            }
#endif
        }
    } else {
        consecutive_overruns = 0;
        skip_dsp_this_frame = 0;
    }

    /* Skip all processing in baseline mode to measure pure Move ioctl time */
    if (baseline_mode) goto do_ioctl;

    // print_mem();
    // write_mem();

    // TODO: Consider using move-anything host code and quickjs for flexibility
    TIME_SECTION_START();
    midi_monitor();
    TIME_SECTION_END(midi_mon_sum, midi_mon_max);

    /* Check if shadow UI requested exit via shared memory */
    if (shadow_control && shadow_display_mode && !shadow_control->display_mode) {
        shadow_display_mode = 0;
        shadow_inject_knob_release = 1;  /* Inject note-offs when exiting shadow mode */
    }

    /* NOTE: MIDI filtering moved to AFTER ioctl - see post-ioctl section below */

#if SHADOW_TRACE_DEBUG
    /* Discovery/probe functions - only needed during development */
    spi_trace_ioctl(request, (char *)argp);
    shadow_capture_midi_probe();
    shadow_scan_mailbox_raw();
    mailbox_diff_probe();
    mailbox_midi_scan_strict();
    mailbox_usb_midi_scan();
    mailbox_midi_region_scan();
    mailbox_midi_out_frame_log();
#endif

    /* === SHADOW INSTRUMENT: PRE-IOCTL PROCESSING === */

    /* Forward MIDI BEFORE ioctl - hardware clears the buffer during transaction */
    TIME_SECTION_START();
    shadow_forward_midi();
    TIME_SECTION_END(fwd_midi_sum, fwd_midi_max);

    /* Mix shadow audio into mailbox BEFORE hardware transaction */
    TIME_SECTION_START();
    shadow_mix_audio();
    TIME_SECTION_END(mix_audio_sum, mix_audio_max);

#if SHADOW_INPROCESS_POC
    TIME_SECTION_START();
    shadow_inprocess_handle_ui_request();
    TIME_SECTION_END(ui_req_sum, ui_req_max);

    TIME_SECTION_START();
    shadow_inprocess_handle_param_request();
    TIME_SECTION_END(param_req_sum, param_req_max);

    /* Forward CC/pitch bend/aftertouch from external MIDI to MIDI_OUT
     * so DSP routing can pick them up (Move only echoes notes, not these) */
    shadow_forward_external_cc_to_out();

    TIME_SECTION_START();
    shadow_inprocess_process_midi();
    TIME_SECTION_END(proc_midi_sum, proc_midi_max);

    /* Pre-ioctl: Mix from pre-rendered buffer (FAST, ~5µs)
     * DSP was rendered post-ioctl in the previous frame.
     * This adds ~3ms latency but lets Move process pad events faster.
     */
    static uint64_t mix_time_sum = 0;
    static int mix_time_count = 0;
    static uint64_t mix_time_max = 0;

    if (!skip_dsp_this_frame) {
        struct timespec mix_start, mix_end;
        clock_gettime(CLOCK_MONOTONIC, &mix_start);

        shadow_inprocess_mix_from_buffer();  /* Fast: just memcpy+mix */

        clock_gettime(CLOCK_MONOTONIC, &mix_end);
        uint64_t mix_us = (mix_end.tv_sec - mix_start.tv_sec) * 1000000 +
                          (mix_end.tv_nsec - mix_start.tv_nsec) / 1000;
        mix_time_sum += mix_us;
        mix_time_count++;
        if (mix_us > mix_time_max) mix_time_max = mix_us;

        /* Track in granular timing */
        inproc_mix_sum += mix_us;
        if (mix_us > inproc_mix_max) inproc_mix_max = mix_us;
    }

    /* Log pre-ioctl mix timing every 1000 blocks (~23 seconds) */
    if (mix_time_count >= 1000) {
#if SHADOW_TIMING_LOG
        uint64_t avg = mix_time_sum / mix_time_count;
        FILE *f = fopen("/tmp/dsp_timing.log", "a");
        if (f) {
            fprintf(f, "Pre-ioctl mix (from buffer): avg=%llu us, max=%llu us\n",
                    (unsigned long long)avg, (unsigned long long)mix_time_max);
            fclose(f);
        }
#endif
        mix_time_sum = 0;
        mix_time_count = 0;
        mix_time_max = 0;
    }
#endif

    /* === SLICE-BASED DISPLAY CAPTURE FOR VOLUME === */
    TIME_SECTION_START();  /* Start timing display section */
    static uint8_t captured_slices[6][172];
    static uint8_t slice_fresh[6] = {0};  /* Reset each time we want new capture */
    static int volume_capture_active = 0;
    static int volume_capture_cooldown = 0;
    static int volume_capture_warmup = 0;  /* Wait for Move to render overlay */

    if (global_mmap_addr && !shadow_display_mode) {
        uint8_t *mem = (uint8_t *)global_mmap_addr;
        uint8_t slice_num = mem[80];

        /* Always capture incoming slices */
        if (slice_num >= 1 && slice_num <= 6) {
            int idx = slice_num - 1;
            memcpy(captured_slices[idx], mem + 84, 172);
            slice_fresh[idx] = 1;
        }

        /* When volume knob touched, start capturing */
        if (shadow_volume_knob_touched && shadow_held_track < 0) {
            if (!volume_capture_active) {
                volume_capture_active = 1;
                volume_capture_warmup = 18;  /* Wait ~3 frames (6 slices * 3) for overlay to render */
                memset(slice_fresh, 0, 6);  /* Reset freshness */
            }

            /* Decrement warmup and skip reading until warmup complete */
            if (volume_capture_warmup > 0) {
                volume_capture_warmup--;
                memset(slice_fresh, 0, 6);  /* Discard stale slices during warmup */
            }

            /* Check if all slices are fresh */
            int all_fresh = 1;
            for (int i = 0; i < 6; i++) {
                if (!slice_fresh[i]) all_fresh = 0;
            }

            if (all_fresh && volume_capture_cooldown == 0) {
                /* Reconstruct display */
                uint8_t full_display[1024];
                for (int s = 0; s < 6; s++) {
                    int offset = s * 172;
                    int bytes = (s == 5) ? 164 : 172;
                    memcpy(full_display + offset, captured_slices[s], bytes);
                }

                /* Check a single row in the bar area (row 30) using COLUMN-MAJOR format
                 * Display format: 8 pages of 128 bytes, each byte = 1 column, 8 vertical bits
                 * The volume bar is a vertical line; we just need to find which column is lit */
                int bar_row = 30;
                int page = bar_row / 8;  /* page 3 */
                int bit = bar_row % 8;   /* bit 6 */
                int bar_col = -1;

                for (int col = 0; col < 128; col++) {
                    int byte_idx = page * 128 + col;
                    if (full_display[byte_idx] & (1 << bit)) {
                        bar_col = col;
                        break;  /* Found the bar */
                    }
                }

                if (bar_col >= 0) {
                    /* Map column range 4-122 to volume 0.0-1.0 */
                    float normalized = (float)(bar_col - 4) / (122.0f - 4.0f);
                    if (normalized < 0.0f) normalized = 0.0f;
                    if (normalized > 1.0f) normalized = 1.0f;

                    if (fabsf(normalized - shadow_master_volume) > 0.01f) {
                        shadow_master_volume = normalized;
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Master volume: x=%d -> %.3f",
                                 bar_col, shadow_master_volume);
                        shadow_log(msg);
                    }
                }

                memset(slice_fresh, 0, 6);  /* Reset for next capture */
                volume_capture_cooldown = 50;  /* Wait ~50 frames before next */
            }
        } else {
            volume_capture_active = 0;
            volume_capture_warmup = 0;  /* Reset warmup for next touch */
        }

        if (volume_capture_cooldown > 0) volume_capture_cooldown--;

        /* === SHIFT+KNOB OVERLAY COMPOSITING ===
         * When overlay is active, draw it onto Move's display BEFORE ioctl sends it */
        if (shift_knob_overlay_active && shift_knob_overlay_timeout > 0 && slice_num >= 1 && slice_num <= 6) {
            static uint8_t overlay_display[1024];
            static int overlay_frame_ready = 0;

            /* When slice 1 arrives, build the overlay display from captured slices */
            if (slice_num == 1) {
                /* Check if we have all slices from previous frame */
                int all_present = 1;
                for (int i = 0; i < 6; i++) {
                    if (!slice_fresh[i]) all_present = 0;
                }

                if (all_present) {
                    /* Reconstruct display from captured slices */
                    for (int s = 0; s < 6; s++) {
                        int offset = s * 172;
                        int bytes = (s == 5) ? 164 : 172;
                        memcpy(overlay_display + offset, captured_slices[s], bytes);
                    }

                    /* Draw overlay onto the display */
                    overlay_draw_shift_knob(overlay_display);
                    overlay_frame_ready = 1;
                }

                /* Decrement timeout once per frame (when slice 1 arrives) */
                shift_knob_overlay_timeout--;
                if (shift_knob_overlay_timeout <= 0) {
                    shift_knob_overlay_active = 0;
                    overlay_frame_ready = 0;
                }
            }

            /* Copy overlay-composited slice back to mailbox */
            if (overlay_frame_ready) {
                int idx = slice_num - 1;
                int offset = idx * 172;
                int bytes = (idx == 5) ? 164 : 172;
                memcpy(mem + 84, overlay_display + offset, bytes);
            }
        }
    }

    /* Write display BEFORE ioctl - overwrites Move's content right before send */
    shadow_swap_display();
    TIME_SECTION_END(display_sum, display_max);  /* End timing display section */

    /* Mark end of pre-ioctl processing */
    clock_gettime(CLOCK_MONOTONIC, &pre_end);

do_ioctl:
    /* In baseline mode, pre_end wasn't set - set it now */
    if (baseline_mode) clock_gettime(CLOCK_MONOTONIC, &pre_end);

    /* === SHADOW UI MIDI OUT (PRE-IOCTL) ===
     * Inject any MIDI from shadow UI into the mailbox before sync.
     * In overtake mode, also clears Move's cable 0 packets when shadow has new data. */
    shadow_inject_ui_midi_out();

    /* === SHADOW MAILBOX SYNC (PRE-IOCTL) ===
     * Copy shadow mailbox to hardware before ioctl.
     * Move has been writing to shadow_mailbox; now we send that to hardware. */
    if (hardware_mmap_addr) {
        memcpy(hardware_mmap_addr, shadow_mailbox, MAILBOX_SIZE);
    }

    /* === HARDWARE TRANSACTION === */
    int result = real_ioctl(fd, request, argp);

    /* === SHADOW MAILBOX SYNC (POST-IOCTL) ===
     * Copy hardware mailbox back to shadow, filtering MIDI_IN.
     * Hardware has filled in new data; we filter it before Move sees it.
     * This eliminates race conditions - Move only sees our shadow buffer. */
    if (hardware_mmap_addr) {
        /* Copy non-MIDI sections directly */
        memcpy(shadow_mailbox + MIDI_OUT_OFFSET, hardware_mmap_addr + MIDI_OUT_OFFSET,
               AUDIO_OUT_OFFSET - MIDI_OUT_OFFSET);  /* MIDI_OUT: 0-255 */
        memcpy(shadow_mailbox + AUDIO_OUT_OFFSET, hardware_mmap_addr + AUDIO_OUT_OFFSET,
               DISPLAY_OFFSET - AUDIO_OUT_OFFSET);   /* AUDIO_OUT: 256-767 */
        memcpy(shadow_mailbox + DISPLAY_OFFSET, hardware_mmap_addr + DISPLAY_OFFSET,
               MIDI_IN_OFFSET - DISPLAY_OFFSET);     /* DISPLAY: 768-2047 */
        memcpy(shadow_mailbox + AUDIO_IN_OFFSET, hardware_mmap_addr + AUDIO_IN_OFFSET,
               MAILBOX_SIZE - AUDIO_IN_OFFSET);      /* AUDIO_IN: 2304-4095 */

        /* Copy MIDI_IN with filtering when in shadow display mode */
        uint8_t *hw_midi = hardware_mmap_addr + MIDI_IN_OFFSET;
        uint8_t *sh_midi = shadow_mailbox + MIDI_IN_OFFSET;
        int overtake_mode = shadow_control ? shadow_control->overtake_mode : 0;

        if (shadow_display_mode && shadow_control) {
            /* Filter MIDI_IN: zero out jog/back/knobs */
            for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
                uint8_t cin = hw_midi[j] & 0x0F;
                uint8_t cable = (hw_midi[j] >> 4) & 0x0F;
                uint8_t status = hw_midi[j + 1];
                uint8_t type = status & 0xF0;
                uint8_t d1 = hw_midi[j + 2];

                int filter = 0;

                /* Only filter internal cable (0x00) */
                if (cable == 0x00) {
                    /* In overtake mode, filter ALL cable 0 events from Move */
                    if (overtake_mode) {
                        filter = 1;
                    } else {
                        /* CC messages: filter jog/back controls (let up/down through for octave) */
                        if (cin == 0x0B && type == 0xB0) {
                            if (d1 == CC_JOG_WHEEL || d1 == CC_JOG_CLICK || d1 == CC_BACK) {
                                filter = 1;
                            }
                            /* Filter knob CCs when shift held */
                            if (d1 >= CC_KNOB1 && d1 <= CC_KNOB8) {
                                filter = 1;
                            }
                            /* Filter Menu and Jog Click CCs when Shift+Volume shortcut is active */
                            if ((d1 == CC_MENU || d1 == CC_JOG_CLICK) && shadow_shift_held && shadow_volume_knob_touched) {
                                filter = 1;
                            }
                        }
                        /* Note messages: filter knob touches (0-9) */
                        if ((cin == 0x09 || cin == 0x08) && (type == 0x90 || type == 0x80)) {
                            if (d1 <= 9) {
                                filter = 1;
                            }
                        }
                    }
                }

                if (filter) {
                    /* Zero the event in shadow buffer */
                    sh_midi[j] = 0;
                    sh_midi[j + 1] = 0;
                    sh_midi[j + 2] = 0;
                    sh_midi[j + 3] = 0;
                } else {
                    /* Copy event as-is */
                    sh_midi[j] = hw_midi[j];
                    sh_midi[j + 1] = hw_midi[j + 1];
                    sh_midi[j + 2] = hw_midi[j + 2];
                    sh_midi[j + 3] = hw_midi[j + 3];
                }
            }
        } else {
            /* Not in shadow mode - copy MIDI_IN directly */
            memcpy(sh_midi, hw_midi, MIDI_BUFFER_SIZE);
        }

        /* Memory barrier to ensure all writes are visible */
        __sync_synchronize();
    }

    /* Mark start of post-ioctl processing */
    clock_gettime(CLOCK_MONOTONIC, &post_start);

    /* Skip post-ioctl processing in baseline mode */
    if (baseline_mode) goto do_timing;

    /* === POST-IOCTL: TRACK BUTTON AND VOLUME KNOB DETECTION ===
     * Scan for track button CCs (40-43) for D-Bus volume sync,
     * and volume knob touch (note 8) for master volume display reading.
     * NOTE: We scan hardware_mmap_addr (unfiltered) because shadow_mailbox is already filtered. */
    if (hardware_mmap_addr && shadow_inprocess_ready) {
        uint8_t *src = hardware_mmap_addr + MIDI_IN_OFFSET;
        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            uint8_t cin = src[j] & 0x0F;
            uint8_t cable = (src[j] >> 4) & 0x0F;
            if (cable != 0x00) continue;  /* Only internal cable */

            uint8_t status = src[j + 1];
            uint8_t type = status & 0xF0;
            uint8_t d1 = src[j + 2];
            uint8_t d2 = src[j + 3];

            /* CC messages (CIN 0x0B) */
            if (cin == 0x0B && type == 0xB0) {
                /* Track buttons are CCs 40-43 */
                if (d1 >= 40 && d1 <= 43) {
                    int pressed = (d2 > 0);
                    shadow_update_held_track(d1, pressed);

                    /* Update selected slot when track is pressed (for Shift+Knob routing)
                     * Track buttons are reversed: CC43=Track1, CC42=Track2, CC41=Track3, CC40=Track4 */
                    if (pressed) {
                        int new_slot = 43 - d1;  /* Reverse: CC43→0, CC42→1, CC41→2, CC40→3 */
                        if (new_slot != shadow_selected_slot) {
                            shadow_selected_slot = new_slot;
                            /* Sync to shared memory for shadow UI and Shift+Knob routing */
                            if (shadow_control) {
                                shadow_control->selected_slot = (uint8_t)new_slot;
                                shadow_control->ui_slot = (uint8_t)new_slot;
                            }
                            char msg[64];
                            snprintf(msg, sizeof(msg), "Selected slot: %d (Track %d)", new_slot, new_slot + 1);
                            shadow_log(msg);
                        }

                        /* Shift + Volume + Track = jump to that slot's edit screen */
                        if (shadow_shift_held && shadow_volume_knob_touched && shadow_control) {
                            shadow_control->ui_slot = new_slot;
                            shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_SLOT;
                            if (!shadow_display_mode) {
                                /* From Move mode: launch shadow UI */
                                shadow_display_mode = 1;
                                shadow_control->display_mode = 1;
                                launch_shadow_ui();
                            }
                            /* If already in shadow mode, flag will be picked up by tick() */
                        }
                    }
                }

                /* Shift + Volume + Menu = jump to Master FX view */
                if (d1 == CC_MENU && d2 > 0) {
                    if (shadow_shift_held && shadow_volume_knob_touched && shadow_control) {
                        if (!shadow_display_mode) {
                            /* From Move mode: launch shadow UI and jump to Master FX */
                            shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_MASTER_FX;
                            shadow_display_mode = 1;
                            shadow_control->display_mode = 1;
                            launch_shadow_ui();
                        } else {
                            /* Already in shadow mode: set flag */
                            shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_MASTER_FX;
                        }
                        /* Block Menu CC from reaching Move */
                        src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                    }
                }

                /* Shift + Volume + Jog Click = toggle overtake module menu */
                if (d1 == CC_JOG_CLICK && d2 > 0) {
                    if (shadow_shift_held && shadow_volume_knob_touched && shadow_control) {
                        if (!shadow_display_mode) {
                            /* From Move mode: launch shadow UI and show overtake menu */
                            shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_OVERTAKE;
                            shadow_display_mode = 1;
                            shadow_control->display_mode = 1;
                            launch_shadow_ui();
                        } else {
                            /* Already in shadow mode: toggle - if in overtake, exit to Move */
                            shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_OVERTAKE;
                        }
                        /* Block Jog Click from reaching Move */
                        src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                    }
                }
            }

            /* Note On/Off messages (CIN 0x09/0x08) for knob touches */
            if ((cin == 0x09 || cin == 0x08) && (type == 0x90 || type == 0x80)) {
                int touched = (type == 0x90 && d2 > 0);

                /* Volume knob touch (note 8) */
                if (d1 == 8) {
                    if (touched != shadow_volume_knob_touched) {
                        shadow_volume_knob_touched = touched;
                        volumeTouched = touched;
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Volume knob touch: %s", touched ? "ON" : "OFF");
                        shadow_log(msg);
                    }
                }

                /* Knob 8 touch (note 7) */
                if (d1 == 7) {
                    if (touched != knob8touched) {
                        knob8touched = touched;
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Knob 8 touch: %s", touched ? "ON" : "OFF");
                        shadow_log(msg);
                    }
                }

                /* Shift + Volume + Knob8 = launch standalone Move Anything */
                if (shadow_shift_held && shadow_volume_knob_touched && knob8touched && !alreadyLaunched) {
                    alreadyLaunched = 1;
                    shadow_log("Launching Move Anything (Shift+Vol+Knob8)!");
                    launchChildAndKillThisProcess("/data/UserData/move-anything/start.sh", "start.sh", "");
                }
            }
        }
    }

    /* === POST-IOCTL: SHIFT+KNOB INTERCEPTION (MOVE MODE) ===
     * When in Move mode (not shadow mode) but Shift is held,
     * intercept knob CCs (71-78) and route to shadow chain DSP.
     * Also block knob touch notes (0-7) to prevent them reaching Move.
     * This allows adjusting shadow synth parameters while playing Move's sequencer. */
    if (!shadow_display_mode && shiftHeld && shift_knob_enabled &&
        shadow_inprocess_ready && global_mmap_addr) {
        uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;
        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            uint8_t cin = src[j] & 0x0F;
            uint8_t cable = (src[j] >> 4) & 0x0F;
            if (cable != 0x00) continue;  /* Only internal cable */

            uint8_t status = src[j + 1];
            uint8_t type = status & 0xF0;
            uint8_t d1 = src[j + 2];
            uint8_t d2 = src[j + 3];

            /* Handle knob touch notes 0-7 - block from Move, show overlay */
            if ((cin == 0x09 || cin == 0x08) && (type == 0x90 || type == 0x80) && d1 <= 7) {
                int knob_num = d1 + 1;  /* Note 0 = Knob 1, etc. */
                /* Use ui_slot from shadow UI navigation, fall back to track button selection */
                int slot = (shadow_control && shadow_control->ui_slot < SHADOW_CHAIN_INSTANCES)
                           ? shadow_control->ui_slot : shadow_selected_slot;
                if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) slot = 0;

                /* Note On (touch start) - show overlay and hold it */
                if (type == 0x90 && d2 > 0) {
                    shift_knob_update_overlay(slot, knob_num, 0);
                    /* Set timeout very high so it stays visible until Note Off */
                    shift_knob_overlay_timeout = 10000;
                }
                /* Note Off (touch release) - start normal timeout for fade */
                else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
                    /* Only fade if this is the knob that's currently shown */
                    if (shift_knob_overlay_active && shift_knob_overlay_knob == knob_num) {
                        shift_knob_overlay_timeout = SHIFT_KNOB_OVERLAY_FRAMES;
                    }
                }
                /* Block touch note from reaching Move */
                src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                continue;
            }

            /* Handle knob CC messages - adjust parameter via set_param */
            if (cin == 0x0B && type == 0xB0 && d1 >= 71 && d1 <= 78) {
                int knob_num = d1 - 70;  /* 1-8 */
                /* Use ui_slot from shadow UI navigation, fall back to track button selection */
                int slot = (shadow_control && shadow_control->ui_slot < SHADOW_CHAIN_INSTANCES)
                           ? shadow_control->ui_slot : shadow_selected_slot;
                if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) slot = 0;

                /* Debug: log knob CC received */
                {
                    char dbg[128];
                    snprintf(dbg, sizeof(dbg), "Shift+Knob: CC=%d knob=%d d2=%d slot=%d active=%d v2=%d set_param=%d",
                             d1, knob_num, d2, slot,
                             shadow_chain_slots[slot].active,
                             shadow_plugin_v2 ? 1 : 0,
                             (shadow_plugin_v2 && shadow_plugin_v2->set_param) ? 1 : 0);
                    shadow_log(dbg);
                }

                /* Adjust parameter if slot is active */
                if (shadow_chain_slots[slot].active && shadow_plugin_v2 && shadow_plugin_v2->set_param) {
                    /* Decode relative encoder value to delta (1 = CW, 127 = CCW) */
                    int delta = 0;
                    if (d2 >= 1 && d2 <= 63) delta = d2;      /* Clockwise: 1-63 */
                    else if (d2 >= 65 && d2 <= 127) delta = d2 - 128;  /* Counter-clockwise: -63 to -1 */

                    if (delta != 0) {
                        /* Adjust parameter via knob_N_adjust */
                        char key[32];
                        char val[16];
                        snprintf(key, sizeof(key), "knob_%d_adjust", knob_num);
                        snprintf(val, sizeof(val), "%d", delta);
                        shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance, key, val);
                    }
                }

                /* Always show overlay (shows "Unmapped" for unmapped knobs) */
                shift_knob_update_overlay(slot, knob_num, d2);

                /* Block CC from reaching Move when shift held */
                src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
            }
        }
    }

    /* Clear overlay when Shift is released */
    if (!shiftHeld && shift_knob_overlay_active) {
        /* Don't immediately clear - let timeout handle it for smooth experience */
    }

    /* === POST-IOCTL: FORWARD MIDI TO SHADOW UI AND HANDLE CAPTURE RULES ===
     * Shadow mailbox sync already filtered MIDI_IN for Move.
     * Here we scan the UNFILTERED hardware buffer to:
     * 1. Forward relevant events to shadow_ui_midi_shm
     * 2. Handle capture rules (route captured events to DSP) */
#if !SHADOW_DISABLE_POST_IOCTL_MIDI
    if (shadow_display_mode && shadow_control && hardware_mmap_addr) {
        uint8_t *src = hardware_mmap_addr + MIDI_IN_OFFSET;  /* Scan unfiltered hardware buffer */
        int overtake_mode = shadow_control->overtake_mode;

        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            uint8_t cin = src[j] & 0x0F;
            uint8_t cable = (src[j] >> 4) & 0x0F;
            /* In overtake mode, allow sysex (CIN 0x04-0x07) and normal messages (0x08-0x0E) */
            if (overtake_mode) {
                if (cin < 0x04 || cin > 0x0E) continue;
            } else {
                if (cin < 0x08 || cin > 0x0E) continue;
                if (cable != 0x00) continue;  /* Only internal cable 0 (Move hardware) */
            }

            uint8_t status = src[j + 1];
            uint8_t type = status & 0xF0;
            uint8_t d1 = src[j + 2];
            uint8_t d2 = src[j + 3];

            /* In overtake mode, forward events to shadow UI.
             * overtake_mode=1 (menu): only forward UI events (jog, click, back)
             * overtake_mode=2 (module): forward ALL events (all cables) */
            if (overtake_mode && shadow_ui_midi_shm) {
                /* In menu mode (1), only forward essential UI events */
                if (overtake_mode == 1) {
                    int is_ui_event = (type == 0xB0 &&
                                      (d1 == 14 || d1 == 3 || d1 == 51 ||  /* jog, click, back */
                                       (d1 >= 40 && d1 <= 43)));           /* track buttons */
                    if (!is_ui_event) continue;  /* Skip non-UI events in menu mode */
                }

                for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                    if (shadow_ui_midi_shm[slot] == 0) {
                        shadow_ui_midi_shm[slot] = src[j];
                        shadow_ui_midi_shm[slot + 1] = status;
                        shadow_ui_midi_shm[slot + 2] = d1;
                        shadow_ui_midi_shm[slot + 3] = d2;
                        shadow_control->midi_ready++;
                        break;
                    }
                }
                continue;  /* Skip normal processing in overtake mode */
            }

            /* Handle CC events */
            if (type == 0xB0) {
                /* CCs to forward to shadow UI:
                 * - CC 14 (jog wheel), CC 3 (jog click), CC 51 (back)
                 * - CC 40-43 (track buttons)
                 * - CC 71-78 (knobs) */
                int forward_to_shadow = (d1 == 14 || d1 == 3 || d1 == 51 ||
                                         (d1 >= 40 && d1 <= 43) || (d1 >= 71 && d1 <= 78));

                if (forward_to_shadow && shadow_ui_midi_shm) {
                    for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                        if (shadow_ui_midi_shm[slot] == 0) {
                            shadow_ui_midi_shm[slot] = 0x0B;
                            shadow_ui_midi_shm[slot + 1] = status;
                            shadow_ui_midi_shm[slot + 2] = d1;
                            shadow_ui_midi_shm[slot + 3] = d2;
                            shadow_control->midi_ready++;
                            break;
                        }
                    }
                }

                /* Check capture rules for CCs (beyond the hardcoded blocks) */
                /* Skip knobs - they're handled by shadow UI, not routed to DSP */
                int is_knob_cc = (d1 >= 71 && d1 <= 78);
                {
                    const shadow_capture_rules_t *capture = shadow_get_focused_capture();
                    if (capture && capture_has_cc(capture, d1) && !is_knob_cc) {
                        /* Route captured CC to focused slot's DSP */
                        int slot = shadow_control ? shadow_control->ui_slot : 0;
                        if (slot >= 0 && slot < SHADOW_CHAIN_INSTANCES &&
                            shadow_chain_slots[slot].active &&
                            shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                            uint8_t msg[3] = { status, d1, d2 };
                            shadow_plugin_v2->on_midi(shadow_chain_slots[slot].instance, msg, 3,
                                                      MOVE_MIDI_SOURCE_INTERNAL);
                        }
                    }
                }
                continue;
            }

            /* Handle note events */
            if (type == 0x90 || type == 0x80) {
                /* Forward track notes (40-43) to shadow UI for slot switching */
                if (d1 >= 40 && d1 <= 43 && shadow_ui_midi_shm) {
                    for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                        if (shadow_ui_midi_shm[slot] == 0) {
                            shadow_ui_midi_shm[slot] = (type == 0x90) ? 0x09 : 0x08;
                            shadow_ui_midi_shm[slot + 1] = status;
                            shadow_ui_midi_shm[slot + 2] = d1;
                            shadow_ui_midi_shm[slot + 3] = d2;
                            shadow_control->midi_ready++;
                            break;
                        }
                    }
                }

                /* Forward knob touch notes (0-7) to shadow UI for peek-at-value */
                if (d1 <= 7 && shadow_ui_midi_shm) {
                    for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                        if (shadow_ui_midi_shm[slot] == 0) {
                            shadow_ui_midi_shm[slot] = (type == 0x90) ? 0x09 : 0x08;
                            shadow_ui_midi_shm[slot + 1] = status;
                            shadow_ui_midi_shm[slot + 2] = d1;
                            shadow_ui_midi_shm[slot + 3] = d2;
                            shadow_control->midi_ready++;
                            break;
                        }
                    }
                }

                /* Check capture rules for focused slot.
                 * Never route knob touch notes (0-9) to DSP even if in capture rules. */
                {
                    const shadow_capture_rules_t *capture = shadow_get_focused_capture();
                    if (capture && d1 >= 10 && capture_has_note(capture, d1)) {
                        /* Route captured note to focused slot's DSP */
                        int slot = shadow_control ? shadow_control->ui_slot : 0;
                        if (slot >= 0 && slot < SHADOW_CHAIN_INSTANCES &&
                            shadow_chain_slots[slot].active &&
                            shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                            uint8_t msg[3] = { status, d1, d2 };
                            shadow_plugin_v2->on_midi(shadow_chain_slots[slot].instance, msg, 3,
                                                      MOVE_MIDI_SOURCE_INTERNAL);
                        }
                    }
                }
                continue;
            }
        }

    }
#endif /* !SHADOW_DISABLE_POST_IOCTL_MIDI */

    /* === POST-IOCTL: INJECT KNOB RELEASE EVENTS ===
     * When toggling shadow mode, inject note-off events for knob touches
     * so Move doesn't think knobs are still being held.
     * This MUST happen AFTER filtering to avoid being zeroed out. */
#if !SHADOW_DISABLE_POST_IOCTL_MIDI
    if (shadow_inject_knob_release && global_mmap_addr) {
        shadow_inject_knob_release = 0;
        uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;
        /* Find empty slots and inject note-offs for knobs 0, 7, 8 (Knob1, Knob8, Volume) */
        const uint8_t knob_notes[] = { 0, 7, 8 };  /* Knob 1, Knob 8, Volume */
        int injected = 0;
        for (int j = 0; j < MIDI_BUFFER_SIZE && injected < 3; j += 4) {
            if (src[j] == 0 && src[j+1] == 0 && src[j+2] == 0 && src[j+3] == 0) {
                /* Empty slot - inject note-off */
                src[j] = 0x08;  /* CIN = Note Off, Cable 0 */
                src[j + 1] = 0x80;  /* Note Off, channel 0 */
                src[j + 2] = knob_notes[injected];  /* Note number */
                src[j + 3] = 0x00;  /* Velocity 0 */
                injected++;
            }
        }
    }
#endif /* !SHADOW_DISABLE_POST_IOCTL_MIDI */

#if SHADOW_INPROCESS_POC
    /* === POST-IOCTL: DEFERRED DSP RENDERING (SLOW, ~300µs) ===
     * Render DSP for the NEXT frame. This happens AFTER the ioctl returns,
     * so Move gets to process pad events before we do heavy DSP work.
     * The rendered audio will be mixed in pre-ioctl of the next frame.
     */
    {
        static uint64_t render_time_sum = 0;
        static int render_time_count = 0;
        static uint64_t render_time_max = 0;

        struct timespec render_start, render_end;
        clock_gettime(CLOCK_MONOTONIC, &render_start);

        shadow_inprocess_render_to_buffer();  /* Slow: actual DSP rendering */

        clock_gettime(CLOCK_MONOTONIC, &render_end);
        uint64_t render_us = (render_end.tv_sec - render_start.tv_sec) * 1000000 +
                              (render_end.tv_nsec - render_start.tv_nsec) / 1000;
        render_time_sum += render_us;
        render_time_count++;
        if (render_us > render_time_max) render_time_max = render_us;

        /* Log DSP render timing every 1000 blocks (~23 seconds) */
        if (render_time_count >= 1000) {
#if SHADOW_TIMING_LOG
            uint64_t avg = render_time_sum / render_time_count;
            FILE *f = fopen("/tmp/dsp_timing.log", "a");
            if (f) {
                fprintf(f, "Post-ioctl DSP render: avg=%llu us, max=%llu us\n",
                        (unsigned long long)avg, (unsigned long long)render_time_max);
                fclose(f);
            }
#endif
            render_time_sum = 0;
            render_time_count = 0;
            render_time_max = 0;
        }
    }
#endif

do_timing:
    /* === COMPREHENSIVE IOCTL TIMING CALCULATIONS === */
    clock_gettime(CLOCK_MONOTONIC, &ioctl_end);

    uint64_t pre_us = (pre_end.tv_sec - ioctl_start.tv_sec) * 1000000 +
                      (pre_end.tv_nsec - ioctl_start.tv_nsec) / 1000;
    uint64_t ioctl_us = (post_start.tv_sec - pre_end.tv_sec) * 1000000 +
                        (post_start.tv_nsec - pre_end.tv_nsec) / 1000;
    uint64_t post_us = (ioctl_end.tv_sec - post_start.tv_sec) * 1000000 +
                       (ioctl_end.tv_nsec - post_start.tv_nsec) / 1000;
    uint64_t total_us = (ioctl_end.tv_sec - ioctl_start.tv_sec) * 1000000 +
                        (ioctl_end.tv_nsec - ioctl_start.tv_nsec) / 1000;

    total_sum += total_us;
    pre_sum += pre_us;
    ioctl_sum += ioctl_us;
    post_sum += post_us;
    timing_count++;

    if (total_us > total_max) total_max = total_us;
    if (pre_us > pre_max) pre_max = pre_us;
    if (ioctl_us > ioctl_max) ioctl_max = ioctl_us;
    if (post_us > post_max) post_max = post_us;

#if SHADOW_TIMING_LOG
    /* Warn immediately if total hook time >2ms */
    if (total_us > 2000) {
        static int hook_overrun_count = 0;
        hook_overrun_count++;
        if (hook_overrun_count <= 10 || hook_overrun_count % 100 == 0) {
            FILE *f = fopen("/tmp/ioctl_timing.log", "a");
            if (f) {
                fprintf(f, "WARNING: Hook overrun #%d: total=%llu us (pre=%llu, ioctl=%llu, post=%llu)\n",
                        hook_overrun_count, (unsigned long long)total_us,
                        (unsigned long long)pre_us, (unsigned long long)ioctl_us,
                        (unsigned long long)post_us);
                fclose(f);
            }
        }
    }
#endif

    /* Log every 1000 blocks (~23 seconds) */
    if (timing_count >= 1000) {
#if SHADOW_TIMING_LOG
        FILE *f = fopen("/tmp/ioctl_timing.log", "a");
        if (f) {
            fprintf(f, "Ioctl timing (1000 blocks): total avg=%llu max=%llu | pre avg=%llu max=%llu | ioctl avg=%llu max=%llu | post avg=%llu max=%llu\n",
                    (unsigned long long)(total_sum / timing_count), (unsigned long long)total_max,
                    (unsigned long long)(pre_sum / timing_count), (unsigned long long)pre_max,
                    (unsigned long long)(ioctl_sum / timing_count), (unsigned long long)ioctl_max,
                    (unsigned long long)(post_sum / timing_count), (unsigned long long)post_max);
            fclose(f);
        }
#endif
        total_sum = pre_sum = ioctl_sum = post_sum = 0;
        total_max = pre_max = ioctl_max = post_max = 0;
        timing_count = 0;
    }

    /* Log granular pre-ioctl timing every 1000 blocks */
    granular_count++;
    if (granular_count >= 1000) {
#if SHADOW_TIMING_LOG
        FILE *f = fopen("/tmp/ioctl_timing.log", "a");
        if (f) {
            fprintf(f, "Granular: midi_mon avg=%llu max=%llu | fwd_midi avg=%llu max=%llu | "
                       "mix_audio avg=%llu max=%llu | ui_req avg=%llu max=%llu | "
                       "param_req avg=%llu max=%llu | proc_midi avg=%llu max=%llu | "
                       "inproc_mix avg=%llu max=%llu | display avg=%llu max=%llu\n",
                    (unsigned long long)(midi_mon_sum / granular_count), (unsigned long long)midi_mon_max,
                    (unsigned long long)(fwd_midi_sum / granular_count), (unsigned long long)fwd_midi_max,
                    (unsigned long long)(mix_audio_sum / granular_count), (unsigned long long)mix_audio_max,
                    (unsigned long long)(ui_req_sum / granular_count), (unsigned long long)ui_req_max,
                    (unsigned long long)(param_req_sum / granular_count), (unsigned long long)param_req_max,
                    (unsigned long long)(proc_midi_sum / granular_count), (unsigned long long)proc_midi_max,
                    (unsigned long long)(inproc_mix_sum / granular_count), (unsigned long long)inproc_mix_max,
                    (unsigned long long)(display_sum / granular_count), (unsigned long long)display_max);
            fclose(f);
        }
#endif
        midi_mon_sum = midi_mon_max = fwd_midi_sum = fwd_midi_max = 0;
        mix_audio_sum = mix_audio_max = ui_req_sum = ui_req_max = 0;
        param_req_sum = param_req_max = proc_midi_sum = proc_midi_max = 0;
        inproc_mix_sum = inproc_mix_max = display_sum = display_max = 0;
        granular_count = 0;
    }

    /* Record frame time for overrun detection in next iteration */
    last_frame_total_us = total_us;

    return result;
}
