#define _GNU_SOURCE

#ifndef ENABLE_SCREEN_READER
#define ENABLE_SCREEN_READER 1
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <math.h>
#include <linux/spi/spidev.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#if ENABLE_SCREEN_READER
#include <dbus/dbus.h>
#include <systemd/sd-bus.h>
#endif

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
#include "host/shadow_constants.h"
#include "host/unified_log.h"
#include "host/tts_engine.h"
#include "host/link_audio.h"

/* Debug flags - set to 1 to enable various debug logging */
#define SHADOW_DEBUG 0           /* Master debug flag for mailbox/MIDI debug */
#define SHADOW_TRACE_DEBUG 0     /* SPI/MIDI trace logging */
#define SHADOW_TIMING_LOG 0      /* ioctl/DSP timing logs to /tmp */

unsigned char *global_mmap_addr = NULL;  /* Points to shadow_mailbox (what Move sees) */
unsigned char *hardware_mmap_addr = NULL; /* Points to real hardware mailbox */
static unsigned char shadow_mailbox[4096] __attribute__((aligned(64))); /* Shadow buffer for Move */

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
#define CC_SAMPLE 87
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
static shadow_screenreader_t *shadow_screenreader_shm = NULL;  /* Forward declaration for D-Bus handler */
static shadow_overlay_state_t *shadow_overlay_shm = NULL;     /* Overlay state for JS rendering */

/* Display mode save/restore for overlay forcing */
/* display_overlay in shadow_control_t replaces the old display_mode forcing */

/* Forward declaration - defined after sampler variables */
static void shadow_overlay_sync(void);
static volatile float shadow_master_volume;  /* Defined later */

/* Feature flags from config/features.json */
static bool shadow_ui_enabled = true;      /* Shadow UI enabled by default */
static bool standalone_enabled = true;     /* Standalone mode enabled by default */
static bool display_mirror_enabled = false; /* Display mirror off by default */

/* Link Audio interception and publishing state */
static link_audio_state_t link_audio;
static uint32_t la_prev_intercepted = 0;  /* previous packets_intercepted (stale tracking) */
static uint32_t la_stale_frames = 0;      /* blocks since last new packet */
static int16_t shadow_slot_capture[SHADOW_CHAIN_INSTANCES][FRAMES_PER_BLOCK * 2];

static void launch_shadow_ui(void);
static void launch_link_subscriber(void);
static void start_link_sub_monitor(void);
static void link_sub_reset_state(void);
static void load_feature_config(void);

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
#define SLOT_STATE_DIR "/data/UserData/move-anything/slot_state"
#define SET_STATE_DIR "/data/UserData/move-anything/set_state"
#define ACTIVE_SET_PATH "/data/UserData/move-anything/active_set.txt"
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
static void (*shadow_chain_set_inject_audio)(void *instance, int16_t *buf, int frames) = NULL;
static void (*shadow_chain_set_external_fx_mode)(void *instance, int mode) = NULL;
static void (*shadow_chain_process_fx)(void *instance, int16_t *buf, int frames) = NULL;
static host_api_v1_t shadow_host_api;
static int shadow_inprocess_ready = 0;

/* Overtake DSP state - loaded when an overtake module has a dsp.so */
static void *overtake_dsp_handle = NULL;           /* dlopen handle */
static plugin_api_v2_t *overtake_dsp_gen = NULL;   /* V2 generator plugin */
static void *overtake_dsp_gen_inst = NULL;          /* Generator instance */
static audio_fx_api_v2_t *overtake_dsp_fx = NULL;  /* V2 FX plugin */
static void *overtake_dsp_fx_inst = NULL;           /* FX instance */
static host_api_v1_t overtake_host_api;             /* Host API provided to plugin */

/* Forward declarations for overtake DSP */
static void shadow_overtake_dsp_load(const char *path);
static void shadow_overtake_dsp_unload(void);

/* Startup mod wheel reset countdown - resets mod wheel after Move finishes its startup MIDI burst */
#define STARTUP_MODWHEEL_RESET_FRAMES 20  /* ~0.6 seconds at 128 frames/block */
static int shadow_startup_modwheel_countdown = 0;

/* Deferred DSP rendering buffer - rendered post-ioctl, mixed pre-ioctl next frame.
 * Used for overtake DSP and as fallback when chain_process_fx is unavailable. */
static int16_t shadow_deferred_dsp_buffer[FRAMES_PER_BLOCK * 2];
static int shadow_deferred_dsp_valid = 0;

/* Per-slot raw synth output from render_to_buffer (no FX applied).
 * FX is processed in mix_from_buffer using same-frame Link Audio data. */
static int16_t shadow_slot_deferred[SHADOW_CHAIN_INSTANCES][FRAMES_PER_BLOCK * 2];
static int shadow_slot_deferred_valid[SHADOW_CHAIN_INSTANCES];

/* Per-slot idle detection: skip render_block when output has been silent.
 * Wakes on MIDI dispatch with one-frame latency (2.9ms, inaudible). */
#define DSP_IDLE_THRESHOLD 344       /* ~1 second of silence before sleeping */
#define DSP_SILENCE_LEVEL 4          /* abs(sample) below this = silence */
static int shadow_slot_silence_frames[SHADOW_CHAIN_INSTANCES];
static int shadow_slot_idle[SHADOW_CHAIN_INSTANCES];
/* Phase 2: track FX output silence to skip FX processing too.
 * FX keeps running while reverb/delay tails decay (synth idle, FX active).
 * Once FX output is also silent, skip FX entirely. */
static int shadow_slot_fx_silence_frames[SHADOW_CHAIN_INSTANCES];
static int shadow_slot_fx_idle[SHADOW_CHAIN_INSTANCES];

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
    float pre_mute_volume;  /* saved volume before mute (0 = not muted) */
    int muted;              /* 1 = muted by Move track mute sync */
    int forward_channel;    /* -2 = passthrough, -1 = auto, 0-15 = forward MIDI to this channel */
    char patch_name[64];
    shadow_capture_rules_t capture;  /* MIDI controls this slot captures when focused */
} shadow_chain_slot_t;

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
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);  /* Optional MIDI handler (via dlsym) */
} master_fx_slot_t;

static master_fx_slot_t shadow_master_fx_slots[MASTER_FX_SLOTS];

/* Legacy single-slot pointers for backward compatibility during transition */
#define shadow_master_fx_handle (shadow_master_fx_slots[0].handle)
#define shadow_master_fx (shadow_master_fx_slots[0].api)
#define shadow_master_fx_instance (shadow_master_fx_slots[0].instance)
#define shadow_master_fx_module (shadow_master_fx_slots[0].module_path)
#define shadow_master_fx_capture (shadow_master_fx_slots[0].capture)

static int shadow_master_fx_chain_active(void) {
    for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
        master_fx_slot_t *s = &shadow_master_fx_slots[fx];
        if (s->instance && s->api && s->api->process_block) {
            return 1;
        }
    }
    return 0;
}

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

/* Mute button hold state: 1 while CC 88 is held, 0 when released */
static volatile int shadow_mute_held = 0;

/* Set detection via Settings.json polling + xattr matching */
static float sampler_set_tempo = 0.0f;              /* 0 = not yet detected */
static char sampler_current_set_name[128] = "";      /* current set name */
static char sampler_current_set_uuid[64] = "";       /* UUID from Sets/<UUID>/<Name>/ path */
static int sampler_last_song_index = -1;             /* last seen currentSongIndex */
static int sampler_pending_song_index = -1;          /* unresolved currentSongIndex without UUID dir yet */
static uint32_t sampler_pending_set_seq = 0;         /* synthetic pending-set UUID sequence */
static float sampler_read_set_tempo(const char *set_name);  /* forward decl */
static void shadow_ui_state_update_slot(int slot);          /* forward decl */
static int shadow_read_set_mute_states(const char *set_name, int muted_out[4]);  /* forward decl */
static void shadow_handle_set_loaded(const char *set_name, const char *uuid);  /* forward decl */
static void shadow_poll_current_set(void);  /* forward decl */
static int shim_run_command(const char *const argv[]);  /* forward decl */
static int shadow_chain_parse_channel(int ch);  /* forward decl */
static void shadow_ui_state_refresh(void);  /* forward decl */

/* Apply mute/unmute to a shadow slot */
static void shadow_apply_mute(int slot, int is_muted) {
    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) return;
    if (is_muted && !shadow_chain_slots[slot].muted) {
        shadow_chain_slots[slot].pre_mute_volume = shadow_chain_slots[slot].volume;
        shadow_chain_slots[slot].volume = 0.0f;
        shadow_chain_slots[slot].muted = 1;
        shadow_ui_state_update_slot(slot);
        char msg[128];
        snprintf(msg, sizeof(msg), "Mute sync: slot %d muted (saved vol=%.3f)",
                 slot, shadow_chain_slots[slot].pre_mute_volume);
        shadow_log(msg);
    } else if (!is_muted && shadow_chain_slots[slot].muted) {
        shadow_chain_slots[slot].volume = shadow_chain_slots[slot].pre_mute_volume;
        shadow_chain_slots[slot].muted = 0;
        shadow_ui_state_update_slot(slot);
        char msg[128];
        snprintf(msg, sizeof(msg), "Mute sync: slot %d unmuted (restored vol=%.3f)",
                 slot, shadow_chain_slots[slot].volume);
        shadow_log(msg);
    }
}


#if ENABLE_SCREEN_READER
/* D-Bus connection for monitoring */
static DBusConnection *shadow_dbus_conn = NULL;
static pthread_t shadow_dbus_thread;
static volatile int shadow_dbus_running = 0;

/* Move's D-Bus socket FD (ORIGINAL, for send() hook to recognize) */
static int move_dbus_socket_fd = -1;
static sd_bus *move_sdbus_conn = NULL;
static pthread_mutex_t move_dbus_conn_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Shadow buffer for pending screen reader announcements */
#define MAX_PENDING_ANNOUNCEMENTS 4
#define MAX_ANNOUNCEMENT_LEN 8192
static char pending_announcements[MAX_PENDING_ANNOUNCEMENTS][MAX_ANNOUNCEMENT_LEN];
static int pending_announcement_count = 0;
static pthread_mutex_t pending_announcements_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Track Move's D-Bus serial number for coordinated message injection */
static uint32_t move_dbus_serial = 0;
static pthread_mutex_t move_dbus_serial_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/* Priority announcement flag - blocks D-Bus messages while toggle announcement plays */
static bool tts_priority_announcement_active = false;
static uint64_t tts_priority_announcement_time_ms = 0;
#define TTS_PRIORITY_BLOCK_MS 1000  /* Block D-Bus for 1 second after priority announcement */

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

/* Native Move sampler source tracking (from stock D-Bus announcements) */
typedef enum {
    NATIVE_SAMPLER_SOURCE_UNKNOWN = 0,
    NATIVE_SAMPLER_SOURCE_RESAMPLING,
    NATIVE_SAMPLER_SOURCE_LINE_IN,
    NATIVE_SAMPLER_SOURCE_MIC_IN,
    NATIVE_SAMPLER_SOURCE_USB_C_IN
} native_sampler_source_t;
static volatile native_sampler_source_t native_sampler_source = NATIVE_SAMPLER_SOURCE_UNKNOWN;
/* Sticky source fallback for transient UNKNOWN states (e.g. route/re-init changes). */
static volatile native_sampler_source_t native_sampler_source_last_known = NATIVE_SAMPLER_SOURCE_UNKNOWN;

typedef enum {
    NATIVE_RESAMPLE_BRIDGE_OFF = 0,
    NATIVE_RESAMPLE_BRIDGE_MIX,
    NATIVE_RESAMPLE_BRIDGE_OVERWRITE
} native_resample_bridge_mode_t;
static volatile native_resample_bridge_mode_t native_resample_bridge_mode = NATIVE_RESAMPLE_BRIDGE_OFF;

/* Link Audio routing: when enabled, per-track Link Audio streams are routed through
 * shadow slot FX chains (zero-rebuild path).  Off by default — user enables in MFX settings. */
static volatile int link_audio_routing_enabled = 0;

/* Snapshot of final mixed output (AUDIO_OUT) at pre-master tap for native resample bridge. */
static int16_t native_total_mix_snapshot[FRAMES_PER_BLOCK * 2];
static volatile int native_total_mix_snapshot_valid = 0;
/* Split component buffers for bridge compensation (no-MFX case).
 * move_component: Move's audio from AUDIO_OUT before ME mixing.
 * me_component: ME's deferred buffer at full gain (slot-vol only, no master-vol). */
static int16_t native_bridge_move_component[FRAMES_PER_BLOCK * 2];
static int16_t native_bridge_me_component[FRAMES_PER_BLOCK * 2];
static float native_bridge_capture_mv = 1.0f;
static volatile int native_bridge_split_valid = 0;
/* Overwrite makeup diagnostics (helps trace master-volume compensation behavior). */
static volatile float native_bridge_makeup_desired_gain = 1.0f;
static volatile float native_bridge_makeup_applied_gain = 1.0f;
static volatile int native_bridge_makeup_limited = 0;

static int shadow_read_global_volume_from_settings(float *linear_out, float *db_out);

typedef struct {
    float rms_l;
    float rms_r;
    float rms_mid;
    float rms_side;
    float rms_low_l;
    float rms_low_r;
} native_audio_metrics_t;

static const char *native_sampler_source_name(native_sampler_source_t src)
{
    switch (src) {
        case NATIVE_SAMPLER_SOURCE_RESAMPLING: return "resampling";
        case NATIVE_SAMPLER_SOURCE_LINE_IN: return "line-in";
        case NATIVE_SAMPLER_SOURCE_MIC_IN: return "mic-in";
        case NATIVE_SAMPLER_SOURCE_USB_C_IN: return "usb-c-in";
        case NATIVE_SAMPLER_SOURCE_UNKNOWN:
        default: return "unknown";
    }
}

static const char *native_resample_bridge_mode_name(native_resample_bridge_mode_t mode)
{
    switch (mode) {
        case NATIVE_RESAMPLE_BRIDGE_OFF: return "off";
        case NATIVE_RESAMPLE_BRIDGE_OVERWRITE: return "overwrite";
        case NATIVE_RESAMPLE_BRIDGE_MIX:
        default: return "mix";
    }
}

static int native_resample_diag_is_enabled(void)
{
    static int cached = 0;
    static int check_counter = 0;
    static int last_logged = -1;

    if (check_counter++ % 200 == 0) {
        cached = (access("/data/UserData/move-anything/native_resample_diag_on", F_OK) == 0);
        if (cached != last_logged) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Native bridge diag: %s",
                     cached ? "enabled" : "disabled");
            shadow_log(msg);
            last_logged = cached;
        }
    }
    return cached;
}

static void native_compute_audio_metrics(const int16_t *buf, native_audio_metrics_t *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    if (!buf) return;

    double sum_l = 0.0;
    double sum_r = 0.0;
    double sum_mid = 0.0;
    double sum_side = 0.0;
    double sum_low_l = 0.0;
    double sum_low_r = 0.0;
    float lp_l = 0.0f;
    float lp_r = 0.0f;
    const float alpha = 0.028f;  /* ~200 Hz one-pole lowpass at 44.1 kHz */

    for (int i = 0; i < FRAMES_PER_BLOCK; i++) {
        float l = (float)buf[i * 2] / 32768.0f;
        float r = (float)buf[i * 2 + 1] / 32768.0f;
        float mid = 0.5f * (l + r);
        float side = 0.5f * (l - r);

        sum_l += (double)l * (double)l;
        sum_r += (double)r * (double)r;
        sum_mid += (double)mid * (double)mid;
        sum_side += (double)side * (double)side;

        lp_l += alpha * (l - lp_l);
        lp_r += alpha * (r - lp_r);
        sum_low_l += (double)lp_l * (double)lp_l;
        sum_low_r += (double)lp_r * (double)lp_r;
    }

    const float inv_n = 1.0f / (float)FRAMES_PER_BLOCK;
    m->rms_l = sqrtf((float)sum_l * inv_n);
    m->rms_r = sqrtf((float)sum_r * inv_n);
    m->rms_mid = sqrtf((float)sum_mid * inv_n);
    m->rms_side = sqrtf((float)sum_side * inv_n);
    m->rms_low_l = sqrtf((float)sum_low_l * inv_n);
    m->rms_low_r = sqrtf((float)sum_low_r * inv_n);
}

static native_resample_bridge_mode_t native_resample_bridge_mode_from_text(const char *text)
{
    if (!text || !text[0]) return NATIVE_RESAMPLE_BRIDGE_OFF;

    char lower[64];
    str_to_lower(lower, sizeof(lower), text);

    if (strcmp(lower, "0") == 0 || strcmp(lower, "off") == 0) {
        return NATIVE_RESAMPLE_BRIDGE_OFF;
    }
    if (strcmp(lower, "2") == 0 ||
        strcmp(lower, "overwrite") == 0 ||
        strcmp(lower, "replace") == 0) {
        return NATIVE_RESAMPLE_BRIDGE_OVERWRITE;
    }
    if (strcmp(lower, "1") == 0 || strcmp(lower, "mix") == 0) {
        return NATIVE_RESAMPLE_BRIDGE_MIX;
    }

    return NATIVE_RESAMPLE_BRIDGE_OFF;
}

static void native_resample_bridge_load_mode_from_shadow_config(void)
{
    const char *config_path = "/data/UserData/move-anything/shadow_config.json";
    FILE *f = fopen(config_path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8192) {
        fclose(f);
        return;
    }

    char *json = malloc((size_t)size + 1);
    if (!json) {
        fclose(f);
        return;
    }

    size_t nread = fread(json, 1, (size_t)size, f);
    fclose(f);
    json[nread] = '\0';

    char *mode_key = strstr(json, "\"resample_bridge_mode\"");
    if (mode_key) {
        char *colon = strchr(mode_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t' || *colon == '"') colon++;
            char token[32];
            size_t idx = 0;
            while (*colon && idx + 1 < sizeof(token)) {
                char c = *colon;
                if (c == '"' || c == ',' || c == '}' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
                    break;
                token[idx++] = c;
                colon++;
            }
            token[idx] = '\0';
            if (token[0]) {
                native_resample_bridge_mode_t new_mode = native_resample_bridge_mode_from_text(token);
                native_resample_bridge_mode = new_mode;
                char msg[128];
                snprintf(msg, sizeof(msg), "Native resample bridge mode: %s (from config)",
                         native_resample_bridge_mode_name(new_mode));
                shadow_log(msg);
            }
        }
    }

    /* Load Link Audio routing setting */
    char *la_key = strstr(json, "\"link_audio_routing\"");
    if (la_key) {
        char *colon = strchr(la_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "true", 4) == 0 || *colon == '1') {
                link_audio_routing_enabled = 1;
            } else {
                link_audio_routing_enabled = 0;
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "Link Audio routing: %s (from config)",
                     link_audio_routing_enabled ? "ON" : "OFF");
            shadow_log(msg);
        }
    }

    free(json);
}

static native_sampler_source_t native_sampler_source_from_text(const char *text)
{
    if (!text || !text[0]) return NATIVE_SAMPLER_SOURCE_UNKNOWN;

    char lower[256];
    str_to_lower(lower, sizeof(lower), text);

    if (strstr(lower, "resampl")) return NATIVE_SAMPLER_SOURCE_RESAMPLING;
    if (strstr(lower, "line in") || strstr(lower, "line-in") || strstr(lower, "linein"))
        return NATIVE_SAMPLER_SOURCE_LINE_IN;
    if (strstr(lower, "usb-c") || strstr(lower, "usb c") || strstr(lower, "usbc"))
        return NATIVE_SAMPLER_SOURCE_USB_C_IN;
    if (strstr(lower, "mic") || strstr(lower, "microphone"))
        return NATIVE_SAMPLER_SOURCE_MIC_IN;

    return NATIVE_SAMPLER_SOURCE_UNKNOWN;
}

static void native_sampler_update_from_dbus_text(const char *text)
{
    native_sampler_source_t parsed = native_sampler_source_from_text(text);
    if (parsed == NATIVE_SAMPLER_SOURCE_UNKNOWN) return;

    if (parsed != native_sampler_source) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Native sampler source: %s (from \"%s\")",
                 native_sampler_source_name(parsed), text);
        shadow_log(msg);
        native_sampler_source = parsed;
        native_sampler_source_last_known = parsed;
    }
}

static int16_t clamp_i16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Overwrite path with component-based compensation.
 *
 * When Master FX is OFF and split buffers are valid, we can compensate for
 * Move's internal master-volume attenuation on the native audio component.
 *
 * Problem: Move's audio in AUDIO_OUT is already attenuated by Move's master
 * volume. When captured and played back as a pad, Move attenuates it AGAIN.
 * Result: pad = captured × Move_vol = Move_audio × Move_vol² (double atten).
 *
 * Fix: undo Move's volume on the native component before writing to bridge:
 *   dst = clamp(move_component / mv + me_component)
 * On playback Move applies its vol once: (move/mv + me) × Move_vol
 *   ≈ move + me × Move_vol ≈ move + me × mv = live output.
 *
 * When Master FX is ON, the signals are nonlinearly mixed and can't be
 * decomposed — fall back to unity copy from the post-FX snapshot. */
static void native_resample_bridge_apply_overwrite_makeup(const int16_t *src,
                                                          int16_t *dst,
                                                          size_t samples)
{
    if (!src || !dst || samples == 0) return;

    float mv = native_bridge_capture_mv;
    if (mv < 0.001f) {
        /* Volume essentially muted — can't compensate meaningfully */
        memcpy(dst, src, samples * sizeof(int16_t));
        native_bridge_makeup_desired_gain = 0.0f;
        native_bridge_makeup_applied_gain = 1.0f;
        native_bridge_makeup_limited = 0;
        return;
    }

    float inv_mv = 1.0f / mv;
    /* Cap makeup gain to prevent extreme boosting at very low volumes.
     * 20× = +26dB. */
    float max_makeup = 20.0f;

    if (!shadow_master_fx_chain_active() && native_bridge_split_valid) {
        /* Component compensation for no-MFX case */
        float native_gain = (inv_mv < max_makeup) ? inv_mv : max_makeup;
        int limiter_hit = 0;

        for (size_t i = 0; i < samples; i++) {
            float move_scaled = (float)native_bridge_move_component[i] * native_gain;
            float me = (float)native_bridge_me_component[i];
            float sum = move_scaled + me;
            if (sum > 32767.0f) { sum = 32767.0f; limiter_hit = 1; }
            if (sum < -32768.0f) { sum = -32768.0f; limiter_hit = 1; }
            dst[i] = (int16_t)lroundf(sum);
        }

        native_bridge_makeup_desired_gain = inv_mv;
        native_bridge_makeup_applied_gain = native_gain;
        native_bridge_makeup_limited = limiter_hit;
    } else if (shadow_master_fx_chain_active()) {
        /* MFX-active case: snapshot is at unity level (MFX processes at unity,
         * mv applied afterward).  Pass through as-is — Move applies master
         * volume on the resampling path, landing at the correct live level. */
        memcpy(dst, src, samples * sizeof(int16_t));
        native_bridge_makeup_desired_gain = 1.0f;
        native_bridge_makeup_applied_gain = 1.0f;
        native_bridge_makeup_limited = 0;
    } else {
        /* Split not valid and no MFX path available — unity copy */
        memcpy(dst, src, samples * sizeof(int16_t));
        native_bridge_makeup_desired_gain = 1.0f;
        native_bridge_makeup_applied_gain = 1.0f;
        native_bridge_makeup_limited = 0;
    }
}

static void native_capture_total_mix_snapshot_from_buffer(const int16_t *src)
{
    if (!src) return;
    memcpy(native_total_mix_snapshot, src, AUDIO_BUFFER_SIZE);

    __sync_synchronize();
    native_total_mix_snapshot_valid = 1;
}

/* Source gating policy for native bridge.
 * - Replace mode: always allow. User explicitly chose full input replacement.
 * - Mix mode: block explicit Mic In / USB-C In announcements to reduce feedback risk.
 * - Unknown source fails open to avoid getting stuck when announcements are missing. */
static int native_resample_bridge_source_allows_apply(native_resample_bridge_mode_t mode)
{
    if (mode == NATIVE_RESAMPLE_BRIDGE_OVERWRITE) return 1;

    native_sampler_source_t src = native_sampler_source;

    if (src == NATIVE_SAMPLER_SOURCE_MIC_IN) return 0;
    if (src == NATIVE_SAMPLER_SOURCE_USB_C_IN) return 0;
    return 1;
}

static void native_resample_diag_log_skip(native_resample_bridge_mode_t mode, const char *reason)
{
    static int skip_counter = 0;
    if (!native_resample_diag_is_enabled()) return;
    if (skip_counter++ % 200 != 0) return;

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Native bridge diag: skip reason=%s mode=%s src=%s last=%s",
             reason ? reason : "unknown",
             native_resample_bridge_mode_name(mode),
             native_sampler_source_name(native_sampler_source),
             native_sampler_source_name(native_sampler_source_last_known));
    shadow_log(msg);
}

static void native_resample_diag_log_apply(native_resample_bridge_mode_t mode,
                                           const int16_t *src,
                                           const int16_t *dst)
{
    static int apply_counter = 0;
    if (!native_resample_diag_is_enabled()) return;
    if (apply_counter++ % 200 != 0) return;

    native_audio_metrics_t src_m;
    native_audio_metrics_t dst_m;
    native_compute_audio_metrics(src, &src_m);
    native_compute_audio_metrics(dst, &dst_m);

    int overwrite_diff = -1;
    if (mode == NATIVE_RESAMPLE_BRIDGE_OVERWRITE && src && dst) {
        overwrite_diff = 0;
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            if (src[i] != dst[i]) overwrite_diff++;
        }
    }

    float src_side_ratio = src_m.rms_side / (src_m.rms_mid + 1e-9f);
    float dst_side_ratio = dst_m.rms_side / (dst_m.rms_mid + 1e-9f);

    char msg[512];
    snprintf(msg, sizeof(msg),
             "Native bridge diag: apply mode=%s src=%s last=%s mv=%.3f split=%d mfx=%d makeup=(%.2fx->%.2fx lim=%d) tap=post-fx-premaster src_rms=(%.4f,%.4f) dst_rms=(%.4f,%.4f) src_low=(%.4f,%.4f) dst_low=(%.4f,%.4f) side_ratio=(%.4f->%.4f) overwrite_diff=%d",
             native_resample_bridge_mode_name(mode),
             native_sampler_source_name(native_sampler_source),
             native_sampler_source_name(native_sampler_source_last_known),
             shadow_master_volume,
             native_bridge_split_valid,
             shadow_master_fx_chain_active(),
             native_bridge_makeup_desired_gain,
             native_bridge_makeup_applied_gain,
             native_bridge_makeup_limited,
             src_m.rms_l, src_m.rms_r,
             dst_m.rms_l, dst_m.rms_r,
             src_m.rms_low_l, src_m.rms_low_r,
             dst_m.rms_low_l, dst_m.rms_low_r,
             src_side_ratio, dst_side_ratio,
             overwrite_diff);
    shadow_log(msg);
}

static void native_resample_bridge_apply(void)
{
    if (!global_mmap_addr || !native_total_mix_snapshot_valid) return;

    native_resample_bridge_mode_t mode = native_resample_bridge_mode;
    if (mode == NATIVE_RESAMPLE_BRIDGE_OFF) {
        native_resample_diag_log_skip(mode, "mode_off");
        return;
    }

    if (!native_resample_bridge_source_allows_apply(mode)) {
        native_resample_diag_log_skip(mode, "source_blocked");
        return;
    }

    int16_t *dst = (int16_t *)(global_mmap_addr + AUDIO_IN_OFFSET);
    if (mode == NATIVE_RESAMPLE_BRIDGE_OVERWRITE) {
        int16_t compensated_snapshot[FRAMES_PER_BLOCK * 2];
        native_resample_bridge_apply_overwrite_makeup(
            native_total_mix_snapshot,
            compensated_snapshot,
            FRAMES_PER_BLOCK * 2
        );
        memcpy(dst, compensated_snapshot, AUDIO_BUFFER_SIZE);
        native_resample_diag_log_apply(mode, native_total_mix_snapshot, dst);
        return;
    }

    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t mixed = (int32_t)dst[i] + (int32_t)native_total_mix_snapshot[i];
        dst[i] = clamp_i16(mixed);
    }
    native_resample_diag_log_apply(mode, native_total_mix_snapshot, dst);
}

#if ENABLE_SCREEN_READER
/* Inject pending screen reader announcements immediately */
static void shadow_inject_pending_announcements(void)
{
    pthread_mutex_lock(&pending_announcements_mutex);
    int has_pending = (pending_announcement_count > 0);
    pthread_mutex_unlock(&pending_announcements_mutex);

    if (!has_pending) return;

    pthread_mutex_lock(&move_dbus_conn_mutex);
    int fd = move_dbus_socket_fd;
    pthread_mutex_unlock(&move_dbus_conn_mutex);

    if (fd < 0) return;

    pthread_mutex_lock(&pending_announcements_mutex);
    for (int i = 0; i < pending_announcement_count; i++) {
        /* Create D-Bus signal message */
        DBusMessage *msg = dbus_message_new_signal(
            "/com/ableton/move/screenreader",
            "com.ableton.move.ScreenReader",
            "text"
        );

        if (msg) {
            const char *announce_text = pending_announcements[i];
            if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &announce_text, DBUS_TYPE_INVALID)) {
                /* Get next serial number */
                pthread_mutex_lock(&move_dbus_serial_mutex);
                move_dbus_serial++;
                uint32_t our_serial = move_dbus_serial;
                pthread_mutex_unlock(&move_dbus_serial_mutex);

                /* Set the serial number */
                dbus_message_set_serial(msg, our_serial);

                /* Serialize and write directly to Move's FD */
                char *marshalled = NULL;
                int msg_len = 0;
                if (dbus_message_marshal(msg, &marshalled, &msg_len)) {
                    ssize_t written = write(fd, marshalled, msg_len);

                    if (written > 0) {
                        char logbuf[512];
                        snprintf(logbuf, sizeof(logbuf),
                                "Screen reader: \"%s\" (injected %zd bytes to FD %d, serial=%u)",
                                announce_text, written, fd, our_serial);
                        shadow_log(logbuf);
                    } else {
                        char logbuf[256];
                        snprintf(logbuf, sizeof(logbuf),
                                "Screen reader: Failed to inject \"%s\" (errno=%d)",
                                announce_text, errno);
                        shadow_log(logbuf);
                    }

                    free(marshalled);
                }
            }
            dbus_message_unref(msg);
        }
    }
    pending_announcement_count = 0;  /* Clear after injecting */
    pthread_mutex_unlock(&pending_announcements_mutex);
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

    /* If Move is asking user to confirm shutdown, dismiss shadow UI so jog wheel
     * press reaches Move's native firmware instead of being captured by us.
     * Also signal the JS UI to save all state before power-off. */
    if (shadow_control &&
        strcasecmp(text, "Press wheel to shut down") == 0) {
        shadow_log("Shutdown prompt detected — saving state and dismissing shadow UI");
        shadow_control->ui_flags |= SHADOW_UI_FLAG_SAVE_STATE;
        shadow_save_state();
        if (shadow_display_mode) {
            shadow_display_mode = 0;
            shadow_control->display_mode = 0;
        }
    }

    /* Track native Move sampler source from stock announcements. */
    native_sampler_update_from_dbus_text(text);

    /* Block D-Bus messages while priority announcement is playing */
    if (tts_priority_announcement_active) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);

        if (now_ms - tts_priority_announcement_time_ms < TTS_PRIORITY_BLOCK_MS) {
            char msg[256];
            snprintf(msg, sizeof(msg), "D-Bus text BLOCKED (priority announcement): \"%s\"", text);
            shadow_log(msg);
            return;  /* Ignore D-Bus message during priority announcement */
        } else {
            /* Blocking period expired */
            tts_priority_announcement_active = false;
        }
    }

    /* Write screen reader text to shared memory for TTS */
    if (shadow_screenreader_shm) {
        /* Only increment sequence if text has actually changed (avoid duplicate increments) */
        if (strncmp(shadow_screenreader_shm->text, text, sizeof(shadow_screenreader_shm->text) - 1) != 0) {
            strncpy(shadow_screenreader_shm->text, text, sizeof(shadow_screenreader_shm->text) - 1);
            shadow_screenreader_shm->text[sizeof(shadow_screenreader_shm->text) - 1] = '\0';
            shadow_screenreader_shm->sequence++;  /* Increment to signal new message */
        }
    }

    /* Set detection handled by Settings.json polling (shadow_poll_current_set) */

    /* Check if it's a track volume message */
    if (strncmp(text, "Track Volume ", 13) == 0) {
        float volume = shadow_parse_volume_db(text);
        if (volume >= 0.0f && shadow_held_track >= 0 && shadow_held_track < SHADOW_CHAIN_INSTANCES) {
            if (!shadow_chain_slots[shadow_held_track].muted) {
                /* Update the held track's slot volume (skip if muted) */
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


    /* Auto-correct mute state from D-Bus screen reader text.
     * Move announces "<Instrument> muted" / "<Instrument> unmuted" on any
     * mute state change. Apply to the selected slot so we stay in sync even
     * when Move mutes/unmutes independently of our Mute+Track shortcut. */
    {
        int text_len = strlen(text);
        int ends_with_unmuted = (text_len >= 8 && strcmp(text + text_len - 7, "unmuted") == 0
                                 && text[text_len - 8] == ' ');
        int ends_with_muted = !ends_with_unmuted &&
                              (text_len >= 6 && strcmp(text + text_len - 5, "muted") == 0
                               && text[text_len - 6] == ' ');

        if (ends_with_muted || ends_with_unmuted) {
            shadow_apply_mute(shadow_selected_slot, ends_with_muted);
        }
    }

    /* After receiving any screen reader message from Move, inject our pending announcements */
    shadow_inject_pending_announcements();
}

/* Send screen reader announcement via D-Bus signal */
/* Hook dbus_connection_send to capture Move's connection when it sends screen reader signals */
/* Hook sd-bus functions to capture Move's connection */

/* Hook sdbus-cpp factory function by mangled name */
/* _ZN5sdbus25createSystemBusConnectionEv = sdbus::createSystemBusConnection() */

typedef void* (*sdbus_create_fn)(void);

/* Hook connect() to capture Move's D-Bus socket FD */
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    static int (*real_connect)(int, const struct sockaddr *, socklen_t) = NULL;
    if (!real_connect) {
        real_connect = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "connect");
        shadow_log("D-Bus: connect() hook initialized");
    }
    
    int result = real_connect(sockfd, addr, addrlen);
    
    if (result == 0 && addr && addr->sa_family == AF_UNIX) {
        struct sockaddr_un *un_addr = (struct sockaddr_un *)addr;
        
        /* Check if this is the D-Bus system bus socket */
        if (strstr(un_addr->sun_path, "dbus") && strstr(un_addr->sun_path, "system")) {
            pthread_mutex_lock(&move_dbus_conn_mutex);
            if (move_dbus_socket_fd == -1) {
                /* This is Move's D-Bus FD - we'll intercept writes to it */
                move_dbus_socket_fd = sockfd;
                char logbuf[256];
                snprintf(logbuf, sizeof(logbuf),
                        "D-Bus: *** INTERCEPTING Move's socket FD %d (path=%s) ***",
                        sockfd, un_addr->sun_path);
                shadow_log(logbuf);
            }
            pthread_mutex_unlock(&move_dbus_conn_mutex);
        }
    }
    
    return result;
}

/* Parse D-Bus message serial number from raw bytes (little-endian) */
static uint32_t parse_dbus_serial(const uint8_t *buf, size_t len)
{
    /* D-Bus native wire format:
     * [0] = endianness ('l' for little-endian)
     * [1] = message type
     * [2] = flags
     * [3] = protocol version (usually 1)
     * [4-7] = body length (uint32)
     * [8-11] = serial number (uint32) */

    if (len < 12) return 0;
    if (buf[0] != 'l') return 0;  /* Only handle little-endian for now */

    uint32_t serial;
    memcpy(&serial, buf + 8, sizeof(serial));
    return serial;
}

/* Hook send() to intercept Move's D-Bus messages and inject ours */
ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    static ssize_t (*real_send)(int, const void *, size_t, int) = NULL;
    if (!real_send) {
        real_send = (ssize_t (*)(int, const void *, size_t, int))dlsym(RTLD_NEXT, "send");
    }

    /* Check if this is a send to Move's D-Bus socket */
    pthread_mutex_lock(&move_dbus_conn_mutex);
    int is_move_dbus = (sockfd == move_dbus_socket_fd && move_dbus_socket_fd >= 0);
    pthread_mutex_unlock(&move_dbus_conn_mutex);

    if (is_move_dbus) {
        /* Parse and track Move's serial number */
        uint32_t serial = parse_dbus_serial((const uint8_t*)buf, len);
        if (serial > 0) {
            pthread_mutex_lock(&move_dbus_serial_mutex);
            if (serial > move_dbus_serial) {
                move_dbus_serial = serial;
            }
            pthread_mutex_unlock(&move_dbus_serial_mutex);
        }

        /* Forward Move's message first */
        ssize_t result = real_send(sockfd, buf, len, flags);

        /* Check if we have pending announcements to inject */
        pthread_mutex_lock(&pending_announcements_mutex);
        int has_pending = (pending_announcement_count > 0);
        pthread_mutex_unlock(&pending_announcements_mutex);

        if (has_pending && result > 0) {
            /* Inject our screen reader messages with coordinated serials */
            pthread_mutex_lock(&pending_announcements_mutex);
            for (int i = 0; i < pending_announcement_count; i++) {
                /* Create D-Bus signal message */
                DBusMessage *msg = dbus_message_new_signal(
                    "/com/ableton/move/screenreader",
                    "com.ableton.move.ScreenReader",
                    "text"
                );

                if (msg) {
                    const char *text = pending_announcements[i];
                    if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID)) {
                        /* Get next serial number */
                        pthread_mutex_lock(&move_dbus_serial_mutex);
                        move_dbus_serial++;
                        uint32_t our_serial = move_dbus_serial;
                        pthread_mutex_unlock(&move_dbus_serial_mutex);

                        /* Set the serial number */
                        dbus_message_set_serial(msg, our_serial);

                        /* Serialize and send */
                        char *marshalled = NULL;
                        int msg_len = 0;
                        if (dbus_message_marshal(msg, &marshalled, &msg_len)) {
                            ssize_t written = real_send(sockfd, marshalled, msg_len, flags);

                            if (written > 0) {
                                char logbuf[512];
                                snprintf(logbuf, sizeof(logbuf),
                                        "Screen reader: \"%s\" (injected %zd bytes, serial=%u)",
                                        text, written, our_serial);
                                shadow_log(logbuf);
                            } else {
                                char logbuf[256];
                                snprintf(logbuf, sizeof(logbuf),
                                        "Screen reader: Failed to inject \"%s\" (errno=%d)",
                                        text, errno);
                                shadow_log(logbuf);
                            }

                            free(marshalled);
                        }
                    }
                    dbus_message_unref(msg);
                }
            }
            pending_announcement_count = 0;  /* Clear after injecting */
            pthread_mutex_unlock(&pending_announcements_mutex);
        }

        return result;
    }

    /* Not Move's D-Bus socket, pass through */
    return real_send(sockfd, buf, len, flags);
}

#endif /* ENABLE_SCREEN_READER — Link Audio is independent of screen reader */

/* ============================================================================
 * LINK AUDIO INTERCEPTION AND PUBLISHING
 * ============================================================================
 * Hooks sendto() to intercept Move's per-track Link Audio packets, provides
 * a self-subscriber to trigger audio transmission without Live, and publishes
 * shadow slot audio as additional Link Audio channels visible in Live.
 * ============================================================================ */

/* Forward declarations for link audio functions */
static void link_audio_parse_session(const uint8_t *pkt, size_t len,
                                     int sockfd, const struct sockaddr *dest,
                                     socklen_t addrlen);
static void link_audio_intercept_audio(const uint8_t *pkt);
static void *link_audio_publisher_thread(void *arg);
static void link_audio_start_publisher(void);

/* Read big-endian uint32 from buffer */
static inline uint32_t link_audio_read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Read big-endian uint16 from buffer */
static inline uint16_t link_audio_read_u16_be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

/* Write big-endian uint32 to buffer */
static inline void link_audio_write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8)  & 0xFF;
    p[3] = v & 0xFF;
}

/* Write big-endian uint16 to buffer */
static inline void link_audio_write_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

/* Write big-endian uint64 to buffer */
static inline void link_audio_write_u64_be(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = v & 0xFF;
        v >>= 8;
    }
}

/* Swap int16 from big-endian to native (little-endian on ARM) */
static inline int16_t link_audio_swap_i16(int16_t be_val)
{
    uint16_t u = (uint16_t)be_val;
    return (int16_t)(((u >> 8) & 0xFF) | ((u & 0xFF) << 8));
}

/* sendto() hook — intercepts Link Audio packets from Move */
static ssize_t (*real_sendto)(int, const void *, size_t, int,
                              const struct sockaddr *, socklen_t) = NULL;

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen)
{
    if (!real_sendto) {
        real_sendto = (ssize_t (*)(int, const void *, size_t, int,
                       const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "sendto");
    }

    /* Check for Link Audio packets when feature is enabled */
    if (link_audio.enabled && len >= 12) {
        const uint8_t *p = (const uint8_t *)buf;
        if (memcmp(p, LINK_AUDIO_MAGIC, LINK_AUDIO_MAGIC_LEN) == 0 &&
            p[7] == LINK_AUDIO_VERSION) {
            uint8_t msg_type = p[8];

            if (msg_type == LINK_AUDIO_MSG_AUDIO && len == LINK_AUDIO_PACKET_SIZE) {
                link_audio_intercept_audio(p);
            } else if (msg_type == LINK_AUDIO_MSG_SESSION) {
                link_audio_parse_session(p, len, sockfd, dest_addr, addrlen);
            }
        }
    }

    return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}

/* Parse TLV session announcement (msg_type=1) to discover channels */
static void link_audio_parse_session(const uint8_t *pkt, size_t len,
                                     int sockfd, const struct sockaddr *dest,
                                     socklen_t addrlen)
{
    if (len < 20) return;

    /* Copy Move's PeerID from offset 12 */
    memcpy(link_audio.move_peer_id, pkt + 12, 8);

    /* Capture network info for self-subscriber (first time only).
     * We're on the audio thread here, so the socket fd is valid for getsockname. */
    if (!link_audio.addr_captured && dest && dest->sa_family == AF_INET6) {
        link_audio.move_socket_fd = sockfd;
        memcpy(&link_audio.move_addr, dest, sizeof(struct sockaddr_in6));
        link_audio.move_addrlen = addrlen;

        /* Capture Move's own local address via getsockname (valid on audio thread).
         * The local port from getsockname IS Move's listening port — do NOT
         * overwrite it with the destination port (that's the peer's port). */
        socklen_t local_len = sizeof(link_audio.move_local_addr);
        if (getsockname(sockfd, (struct sockaddr *)&link_audio.move_local_addr,
                        &local_len) == 0) {
            /* Keep the port from getsockname — it's Move's bound/listening port */
        } else {
            /* Fallback: copy dest addr (better than nothing) */
            memcpy(&link_audio.move_local_addr, dest, sizeof(struct sockaddr_in6));
        }

        link_audio.addr_captured = 1;

        /* If session was already parsed (standalone subscriber beat Live),
         * start the publisher now that we have Live's address */
        if (link_audio.session_parsed && !link_audio.publisher_running) {
            link_audio_start_publisher();
        }

        /* Write Move's chnnlsv endpoint to file for standalone link-subscriber */
        {
            char local_str_ep[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &link_audio.move_local_addr.sin6_addr,
                      local_str_ep, sizeof(local_str_ep));
            FILE *ep = fopen("/data/UserData/move-anything/link-audio-endpoint", "w");
            if (ep) {
                fprintf(ep, "%s %d %u\n",
                        local_str_ep,
                        ntohs(link_audio.move_local_addr.sin6_port),
                        link_audio.move_local_addr.sin6_scope_id);
                fclose(ep);
            }
        }

        char dest_str[INET6_ADDRSTRLEN], local_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &link_audio.move_addr.sin6_addr,
                  dest_str, sizeof(dest_str));
        inet_ntop(AF_INET6, &link_audio.move_local_addr.sin6_addr,
                  local_str, sizeof(local_str));
        char logbuf[512];
        snprintf(logbuf, sizeof(logbuf),
                 "Link Audio: captured dest=%s:%d, local(Move)=%s:%d scope=%d",
                 dest_str, ntohs(link_audio.move_addr.sin6_port),
                 local_str, ntohs(link_audio.move_local_addr.sin6_port),
                 link_audio.move_local_addr.sin6_scope_id);
        shadow_log(logbuf);
    }

    /* Parse TLV entries starting at offset 20 */
    size_t pos = 20;
    while (pos + 8 <= len) {
        /* TLV: 4-byte tag + 4-byte length */
        const uint8_t *tag = pkt + pos;
        uint32_t tlen = link_audio_read_u32_be(pkt + pos + 4);
        pos += 8;

        if (pos + tlen > len) break;  /* malformed */

        if (memcmp(tag, "sess", 4) == 0 && tlen == 8) {
            /* Session ID */
            memcpy(link_audio.session_id, pkt + pos, 8);

        } else if (memcmp(tag, "auca", 4) == 0 && tlen >= 4) {
            /* Audio channel announcements */
            const uint8_t *auca = pkt + pos;
            size_t auca_end = tlen;
            uint32_t num_channels = link_audio_read_u32_be(auca);
            size_t auca_pos = 4;

            int count = 0;
            for (uint32_t c = 0; c < num_channels && auca_pos + 4 <= auca_end; c++) {
                uint32_t name_len = link_audio_read_u32_be(auca + auca_pos);
                auca_pos += 4;
                if (auca_pos + name_len + 8 > auca_end) break;

                if (count < LINK_AUDIO_MOVE_CHANNELS) {
                    link_audio_channel_t *ch = &link_audio.channels[count];
                    /* Copy name */
                    int nlen = name_len < 31 ? name_len : 31;
                    memcpy(ch->name, auca + auca_pos, nlen);
                    ch->name[nlen] = '\0';
                    auca_pos += name_len;
                    /* Copy channel ID */
                    memcpy(ch->channel_id, auca + auca_pos, 8);
                    auca_pos += 8;
                    ch->active = 1;
                    count++;
                } else {
                    auca_pos += name_len + 8;
                }
            }
            link_audio.move_channel_count = count;

        }

        pos += tlen;
    }

    /* Mark session as parsed and start threads.
     * For standalone subscriber (no Live), addr_captured may be false
     * since audio flows via IPv4 loopback. Still mark as parsed. */
    if (!link_audio.session_parsed &&
        link_audio.move_channel_count > 0) {
        link_audio.session_parsed = 1;
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf),
                 "Link Audio: session parsed, %d channels discovered",
                 link_audio.move_channel_count);
        shadow_log(logbuf);
        for (int i = 0; i < link_audio.move_channel_count; i++) {
            char ch_log[128];
            snprintf(ch_log, sizeof(ch_log), "Link Audio:   [%d] \"%s\"",
                     i, link_audio.channels[i].name);
            shadow_log(ch_log);
        }

        /* Start publisher for shadow audio only when Live's address is known */
        if (link_audio.addr_captured) {
            link_audio_start_publisher();
        }
    }
}

/* Intercept audio data packet (msg_type=6, runs on audio thread — must be fast) */
static void link_audio_intercept_audio(const uint8_t *pkt)
{
    /* Extract ChannelID at offset 20 */
    const uint8_t *channel_id = pkt + 20;

    /* Find matching channel */
    int idx = -1;
    for (int i = 0; i < link_audio.move_channel_count; i++) {
        if (memcmp(link_audio.channels[i].channel_id, channel_id, 8) == 0) {
            idx = i;
            break;
        }
    }

    /* Auto-discover channels from audio packets when not yet known via session.
     * The standalone link-subscriber triggers Move to stream audio, but session
     * announcements (msg_type=1) may not flow through sendto() on loopback. */
    if (idx < 0 && link_audio.move_channel_count < LINK_AUDIO_MOVE_CHANNELS) {
        idx = link_audio.move_channel_count;
        link_audio_channel_t *ch = &link_audio.channels[idx];
        memcpy(ch->channel_id, channel_id, 8);
        snprintf(ch->name, sizeof(ch->name), "ch%d", idx);
        ch->active = 1;
        ch->write_pos = 0;
        ch->read_pos = 0;
        ch->peak = 0;
        ch->pkt_count = 0;
        link_audio.move_channel_count = idx + 1;

        /* Also capture Move's PeerID from offset 12 */
        memcpy(link_audio.move_peer_id, pkt + 12, 8);

        char logbuf[128];
        snprintf(logbuf, sizeof(logbuf),
                 "Link Audio: auto-discovered channel %d (id %02x%02x%02x%02x%02x%02x%02x%02x)",
                 idx, channel_id[0], channel_id[1], channel_id[2], channel_id[3],
                 channel_id[4], channel_id[5], channel_id[6], channel_id[7]);
        shadow_log(logbuf);
    }

    if (idx < 0) return;  /* no room for more channels */

    link_audio_channel_t *ch = &link_audio.channels[idx];

    /* Byte-swap 125 stereo frames from BE int16 → LE int16, write to ring */
    const int16_t *src = (const int16_t *)(pkt + LINK_AUDIO_HEADER_SIZE);
    uint32_t wp = ch->write_pos;
    uint32_t rp = ch->read_pos;
    int samples_to_write = LINK_AUDIO_FRAMES_PER_PACKET * 2;

    /* Overflow check: drop packet if ring is too full */
    if ((wp - rp) + (uint32_t)samples_to_write > LINK_AUDIO_RING_SAMPLES) {
        link_audio.overruns++;
        return;
    }

    int peak = ch->peak;

    for (int i = 0; i < samples_to_write; i++) {
        int16_t sample = link_audio_swap_i16(src[i]);
        ch->ring[wp & LINK_AUDIO_RING_MASK] = sample;
        wp++;
        int abs_s = (sample < 0) ? -(int)sample : (int)sample;
        if (abs_s > peak) peak = abs_s;
    }

    /* Memory barrier before publishing write_pos */
    __sync_synchronize();
    ch->write_pos = wp;
    ch->peak = (int16_t)(peak > 32767 ? 32767 : peak);
    ch->pkt_count++;

    /* Update sequence from packet (offset 44, u32 BE) */
    ch->sequence = link_audio_read_u32_be(pkt + 44);

    link_audio.packets_intercepted++;
}

/* Read from a Move channel's ring buffer (called from consumer thread) */
static int link_audio_read_channel(int idx, int16_t *out, int frames)
{
    if (idx < 0 || idx >= link_audio.move_channel_count) return 0;

    link_audio_channel_t *ch = &link_audio.channels[idx];
    int samples = frames * 2;  /* stereo */

    __sync_synchronize();
    uint32_t rp = ch->read_pos;
    uint32_t wp = ch->write_pos;
    uint32_t avail = wp - rp;

    if (avail < (uint32_t)samples) {
        /* Underrun — zero-fill */
        memset(out, 0, samples * sizeof(int16_t));
        link_audio.underruns++;
        return 0;
    }

    /* If the writer lapped the reader (e.g. slot was inactive), skip ahead
     * to the most recent data.  Reading stale ring positions produces audio
     * that doesn't match the current mailbox frame, breaking inject/subtract
     * cancellation.  Keep one frame of margin to avoid racing the writer. */
    if (avail > (uint32_t)samples * 4) {
        rp = wp - (uint32_t)samples;
    }

    for (int i = 0; i < samples; i++) {
        out[i] = ch->ring[rp & LINK_AUDIO_RING_MASK];
        rp++;
    }

    __sync_synchronize();
    ch->read_pos = rp;
    return 1;
}

/* ---- Shadow audio publisher ---- */

static void link_audio_start_publisher(void)
{
    /* Publisher disabled on main: needs Link SDK integration (see link-audio-publish branch) */
    return;
}

/* Build a session announcement packet for the shadow publisher */
static int link_audio_build_session_announcement(uint8_t *pkt, int max_len)
{
    int pos = 0;

    /* Common header (12 bytes) */
    memcpy(pkt + pos, LINK_AUDIO_MAGIC, LINK_AUDIO_MAGIC_LEN);
    pos += LINK_AUDIO_MAGIC_LEN;
    pkt[pos++] = LINK_AUDIO_VERSION;
    pkt[pos++] = LINK_AUDIO_MSG_SESSION;  /* msg_type = 1 */
    pkt[pos++] = 0;  /* flags */
    pkt[pos++] = 0;  /* reserved hi */
    pkt[pos++] = 0;  /* reserved lo */

    /* PeerID (8 bytes) */
    memcpy(pkt + pos, link_audio.publisher_peer_id, 8);
    pos += 8;

    /* TLV: "sess" - session ID */
    memcpy(pkt + pos, "sess", 4); pos += 4;
    link_audio_write_u32_be(pkt + pos, 8); pos += 4;
    memcpy(pkt + pos, link_audio.publisher_session_id, 8); pos += 8;

    /* TLV: "__pi" - peer info (name = "ME") */
    const char *peer_name = "ME";
    uint32_t name_len = (uint32_t)strlen(peer_name);
    memcpy(pkt + pos, "__pi", 4); pos += 4;
    link_audio_write_u32_be(pkt + pos, 4 + name_len); pos += 4;
    link_audio_write_u32_be(pkt + pos, name_len); pos += 4;
    memcpy(pkt + pos, peer_name, name_len); pos += name_len;

    /* TLV: "auca" - audio channel announcements */
    /* Count active shadow channels */
    int active_count = 0;
    for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
        if (shadow_chain_slots[i].active) active_count++;
    }

    /* Calculate auca payload size */
    uint32_t auca_size = 4;  /* num_channels u32 */
    for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
        if (!shadow_chain_slots[i].active) continue;
        auca_size += 4 + (uint32_t)strlen(link_audio.pub_channels[i].name) + 8;
    }

    memcpy(pkt + pos, "auca", 4); pos += 4;
    link_audio_write_u32_be(pkt + pos, auca_size); pos += 4;
    link_audio_write_u32_be(pkt + pos, active_count); pos += 4;

    for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
        if (!shadow_chain_slots[i].active) continue;
        uint32_t ch_name_len = (uint32_t)strlen(link_audio.pub_channels[i].name);
        link_audio_write_u32_be(pkt + pos, ch_name_len); pos += 4;
        memcpy(pkt + pos, link_audio.pub_channels[i].name, ch_name_len);
        pos += ch_name_len;
        memcpy(pkt + pos, link_audio.pub_channels[i].channel_id, 8);
        pos += 8;
    }

    /* TLV: "__ht" - heartbeat */
    memcpy(pkt + pos, "__ht", 4); pos += 4;
    link_audio_write_u32_be(pkt + pos, 8); pos += 4;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ts = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    link_audio_write_u64_be(pkt + pos, ts); pos += 8;

    return pos;
}

/* Build a 574-byte audio data packet */
static void link_audio_build_audio_packet(uint8_t *pkt,
                                          const uint8_t *peer_id,
                                          const uint8_t *channel_id,
                                          uint32_t sequence,
                                          const int16_t *samples_le,
                                          int num_frames)
{
    memset(pkt, 0, LINK_AUDIO_PACKET_SIZE);

    /* Common header */
    memcpy(pkt, LINK_AUDIO_MAGIC, LINK_AUDIO_MAGIC_LEN);
    pkt[7] = LINK_AUDIO_VERSION;
    pkt[8] = LINK_AUDIO_MSG_AUDIO;

    /* Identity block */
    memcpy(pkt + 12, peer_id, 8);      /* PeerID */
    memcpy(pkt + 20, channel_id, 8);   /* ChannelID */
    memcpy(pkt + 28, peer_id, 8);      /* SourcePeerID */

    /* Audio metadata */
    link_audio_write_u32_be(pkt + 36, 1);           /* stream version */
    /* pkt + 40: reserved = 0 */
    link_audio_write_u32_be(pkt + 44, sequence);     /* sequence number */
    link_audio_write_u16_be(pkt + 48, (uint16_t)num_frames);
    /* pkt + 50: padding = 0 */
    /* Timestamp at offset 52 */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ts = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    link_audio_write_u64_be(pkt + 52, ts);
    link_audio_write_u32_be(pkt + 60, 6);            /* audio descriptor */
    pkt[64] = 0xd5; pkt[65] = 0x11; pkt[66] = 0x01; /* constant */
    link_audio_write_u32_be(pkt + 67, 44100);        /* sample rate */
    pkt[71] = 2;                                      /* stereo */
    link_audio_write_u16_be(pkt + 72, LINK_AUDIO_PAYLOAD_SIZE);

    /* Audio payload: convert LE int16 → BE int16 */
    int16_t *dst = (int16_t *)(pkt + LINK_AUDIO_HEADER_SIZE);
    for (int i = 0; i < num_frames * 2; i++) {
        dst[i] = link_audio_swap_i16(samples_le[i]);
    }
}

/* Publisher thread: sends shadow audio to Live as Link Audio */
static void *link_audio_publisher_thread(void *arg)
{
    (void)arg;

    /* We need to know where to send session announcements.
     * Use the same destination as Move's session announcements. */
    struct sockaddr_in6 dest_addr;
    memcpy(&dest_addr, &link_audio.move_addr, sizeof(dest_addr));

    uint8_t session_pkt[512];
    uint8_t audio_pkt[LINK_AUDIO_PACKET_SIZE];
    uint8_t recv_buf[128];

    uint32_t session_counter = 0;
    uint32_t tick_counter = 0;

    /* Per-channel output accumulator for 128→125 frame conversion */
    int16_t accum[LINK_AUDIO_SHADOW_CHANNELS][LINK_AUDIO_PUB_RING_SAMPLES];
    uint32_t accum_wp[LINK_AUDIO_SHADOW_CHANNELS];
    uint32_t accum_rp[LINK_AUDIO_SHADOW_CHANNELS];
    memset(accum_wp, 0, sizeof(accum_wp));
    memset(accum_rp, 0, sizeof(accum_rp));
    memset(accum, 0, sizeof(accum));

    while (link_audio.publisher_running && link_audio.enabled) {
        /* Wait for ioctl tick signal (set by ioctl hook at ~344 Hz) */
        while (!link_audio.publisher_tick && link_audio.publisher_running) {
            struct timespec ts = {0, 500000L};  /* 0.5ms poll */
            nanosleep(&ts, NULL);
        }
        link_audio.publisher_tick = 0;
        tick_counter++;

        /* Send session announcement every ~1 second (~344 ticks) */
        if (tick_counter % 344 == 0) {
            int pkt_len = link_audio_build_session_announcement(session_pkt,
                                                                sizeof(session_pkt));
            real_sendto(link_audio.publisher_socket_fd, session_pkt, pkt_len, 0,
                        (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            session_counter++;
        }

        /* Check for incoming ChannelRequests (non-blocking) */
        struct sockaddr_in6 from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(link_audio.publisher_socket_fd, recv_buf,
                             sizeof(recv_buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from_addr, &from_len);
        if (n >= 36 && memcmp(recv_buf, LINK_AUDIO_MAGIC, LINK_AUDIO_MAGIC_LEN) == 0 &&
            recv_buf[8] == LINK_AUDIO_MSG_REQUEST) {
            /* Match ChannelID at offset 20 to our channels */
            for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
                if (memcmp(recv_buf + 20,
                           link_audio.pub_channels[i].channel_id, 8) == 0) {
                    link_audio.pub_channels[i].subscribed = 1;
                    /* Store subscriber address for sending audio */
                    memcpy(&dest_addr, &from_addr, sizeof(dest_addr));
                }
            }
        }

        /* Feed captured slot audio into accumulators */
        for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
            if (!shadow_chain_slots[i].active) continue;

            uint32_t wp = accum_wp[i];
            for (int s = 0; s < FRAMES_PER_BLOCK * 2; s++) {
                accum[i][wp & LINK_AUDIO_PUB_RING_MASK] = shadow_slot_capture[i][s];
                wp++;
            }
            accum_wp[i] = wp;
        }

        /* Drain 125-frame packets from accumulators for subscribed channels */
        for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
            if (!link_audio.pub_channels[i].subscribed) continue;
            if (!shadow_chain_slots[i].active) continue;

            uint32_t avail = accum_wp[i] - accum_rp[i];
            while (avail >= LINK_AUDIO_FRAMES_PER_PACKET * 2) {
                /* Read 125 frames from accumulator */
                int16_t out_frames[LINK_AUDIO_FRAMES_PER_PACKET * 2];
                uint32_t rp = accum_rp[i];
                for (int s = 0; s < LINK_AUDIO_FRAMES_PER_PACKET * 2; s++) {
                    out_frames[s] = accum[i][rp & LINK_AUDIO_PUB_RING_MASK];
                    rp++;
                }
                accum_rp[i] = rp;

                /* Build and send audio packet */
                link_audio_build_audio_packet(audio_pkt,
                                              link_audio.publisher_peer_id,
                                              link_audio.pub_channels[i].channel_id,
                                              link_audio.pub_channels[i].sequence++,
                                              out_frames,
                                              LINK_AUDIO_FRAMES_PER_PACKET);
                real_sendto(link_audio.publisher_socket_fd, audio_pkt,
                            LINK_AUDIO_PACKET_SIZE, 0,
                            (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                link_audio.packets_published++;

                avail = accum_wp[i] - accum_rp[i];
            }
        }
    }

    close(link_audio.publisher_socket_fd);
    link_audio.publisher_socket_fd = -1;
    shadow_log("Link Audio: publisher thread exited");
    return NULL;
}

#if ENABLE_SCREEN_READER /* Resume screen reader / D-Bus hooks */

int sd_bus_default_system(sd_bus **ret)
{
    static int (*real_default)(sd_bus**) = NULL;
    if (!real_default) {
        real_default = (int (*)(sd_bus**))dlsym(RTLD_NEXT, "sd_bus_default_system");
    }
    
    int result = real_default(ret);
    
    if (result >= 0 && ret && *ret) {
        pthread_mutex_lock(&move_dbus_conn_mutex);
        if (!move_sdbus_conn) {
            move_sdbus_conn = sd_bus_ref(*ret);
            const char *unique_name = NULL;
            sd_bus_get_unique_name(*ret, &unique_name);
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf), 
                    "D-Bus: *** CAPTURED sd-bus connection via sd_bus_default_system (sender=%s) ***",
                    unique_name ? unique_name : "?");
            shadow_log(logbuf);
        }
        pthread_mutex_unlock(&move_dbus_conn_mutex);
    }
    
    return result;
}

int sd_bus_start(sd_bus *bus)
{
    static int (*real_start)(sd_bus*) = NULL;
    if (!real_start) {
        real_start = (int (*)(sd_bus*))dlsym(RTLD_NEXT, "sd_bus_start");
    }
    
    int result = real_start(bus);
    
    if (result >= 0 && bus) {
        pthread_mutex_lock(&move_dbus_conn_mutex);
        if (!move_sdbus_conn) {
            move_sdbus_conn = sd_bus_ref(bus);
            const char *unique_name = NULL;
            sd_bus_get_unique_name(bus, &unique_name);
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf), 
                    "D-Bus: *** CAPTURED sd-bus connection via sd_bus_start (sender=%s) ***",
                    unique_name ? unique_name : "?");
            shadow_log(logbuf);
        }
        pthread_mutex_unlock(&move_dbus_conn_mutex);
    }
    
    return result;
}

/* Hook sendmsg to capture Move's D-Bus socket FD at the lowest level */
/* Removed - sd-bus hooks are used instead */

/* Queue a screen reader announcement to be injected via send() hook */
static void send_screenreader_announcement(const char *text)
{
    if (!text || !text[0]) return;

    pthread_mutex_lock(&move_dbus_conn_mutex);
    int sock_fd = move_dbus_socket_fd;
    pthread_mutex_unlock(&move_dbus_conn_mutex);

    if (sock_fd < 0) {
        /* Haven't captured Move's FD yet */
        return;
    }

    /* Add to pending queue */
    pthread_mutex_lock(&pending_announcements_mutex);
    if (pending_announcement_count < MAX_PENDING_ANNOUNCEMENTS) {
        size_t text_len = strlen(text);
        if (text_len >= MAX_ANNOUNCEMENT_LEN) {
            text_len = MAX_ANNOUNCEMENT_LEN - 1;
        }
        memcpy(pending_announcements[pending_announcement_count], text, text_len);
        pending_announcements[pending_announcement_count][text_len] = '\0';
        pending_announcement_count++;

        char logbuf[512];
        snprintf(logbuf, sizeof(logbuf),
                "Screen reader: Queued \"%s\" (pending=%d)",
                text, pending_announcement_count);
        shadow_log(logbuf);
    } else {
        shadow_log("Screen reader: Queue full, dropping announcement");
    }
    pthread_mutex_unlock(&pending_announcements_mutex);

    /* Flush immediately so announcements aren't delayed until next D-Bus activity */
    shadow_inject_pending_announcements();
}

/* D-Bus filter function to receive signals */
static DBusHandlerResult shadow_dbus_filter(DBusConnection *conn, DBusMessage *msg, void *data)
{
    (void)conn;
    (void)data;

    /* Log ALL D-Bus signals for discovery (temporary) */
    {
        const char *iface = dbus_message_get_interface(msg);
        const char *member = dbus_message_get_member(msg);
        const char *path = dbus_message_get_path(msg);
        const char *sender = dbus_message_get_sender(msg);
        int msg_type = dbus_message_get_type(msg);

        /* Log WebServiceAuthentication method calls (challenge/PIN flow) */
        if (msg_type == DBUS_MESSAGE_TYPE_METHOD_CALL &&
            iface && strcmp(iface, "com.ableton.move.WebServiceAuthentication") == 0) {
            char arg_preview[128] = "";
            DBusMessageIter iter;
            if (dbus_message_iter_init(msg, &iter) &&
                dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
                const char *s = NULL;
                dbus_message_iter_get_basic(&iter, &s);
                if (s) snprintf(arg_preview, sizeof(arg_preview), " arg0=\"%.60s\"", s);
            }
            char logbuf[512];
            snprintf(logbuf, sizeof(logbuf), "D-Bus AUTH: %s.%s path=%s sender=%s%s",
                     iface, member ? member : "?", path ? path : "?",
                     sender ? sender : "?", arg_preview);
            shadow_log(logbuf);
        }

        if (msg_type == DBUS_MESSAGE_TYPE_SIGNAL) {
            /* Extract first string arg if present */
            char arg_preview[128] = "";
            DBusMessageIter iter;
            if (dbus_message_iter_init(msg, &iter)) {
                int atype = dbus_message_iter_get_arg_type(&iter);
                if (atype == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&iter, &s);
                    if (s) snprintf(arg_preview, sizeof(arg_preview), " arg0=\"%.100s\"", s);
                } else if (atype == DBUS_TYPE_INT32) {
                    int32_t v;
                    dbus_message_iter_get_basic(&iter, &v);
                    snprintf(arg_preview, sizeof(arg_preview), " arg0=%d", v);
                } else if (atype == DBUS_TYPE_UINT32) {
                    uint32_t v;
                    dbus_message_iter_get_basic(&iter, &v);
                    snprintf(arg_preview, sizeof(arg_preview), " arg0=%u", v);
                } else if (atype == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t v;
                    dbus_message_iter_get_basic(&iter, &v);
                    snprintf(arg_preview, sizeof(arg_preview), " arg0=%s", v ? "true" : "false");
                }
            }

            char logbuf[512];
            snprintf(logbuf, sizeof(logbuf), "D-Bus signal: %s.%s path=%s sender=%s%s",
                     iface ? iface : "?", member ? member : "?",
                     path ? path : "?", sender ? sender : "?", arg_preview);
            shadow_log(logbuf);

            /* Track serial numbers from Move's messages */
            if (sender && strstr(sender, ":1.")) {
                uint32_t serial = dbus_message_get_serial(msg);
                if (serial > 0) {
                    pthread_mutex_lock(&move_dbus_serial_mutex);
                    if (serial > move_dbus_serial) {
                        move_dbus_serial = serial;
                    }
                    pthread_mutex_unlock(&move_dbus_serial_mutex);
                }
            }
        }
    }

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

    /* Scan existing FDs to find Move's D-Bus socket */
    shadow_log("D-Bus: Scanning file descriptors for Move's D-Bus socket...");
    for (int fd = 3; fd < 256; fd++) {
        struct sockaddr_un addr;
        socklen_t addr_len = sizeof(addr);

        if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
            if (addr.sun_family == AF_UNIX &&
                strstr(addr.sun_path, "dbus") &&
                strstr(addr.sun_path, "system")) {

                /* Get our own FD for comparison */
                int our_fd = -1;
                dbus_connection_get_unix_fd(shadow_dbus_conn, &our_fd);

                if (fd != our_fd) {
                    /* This is Move's FD! Store ORIGINAL for send() hook to recognize */
                    pthread_mutex_lock(&move_dbus_conn_mutex);
                    move_dbus_socket_fd = fd;
                    pthread_mutex_unlock(&move_dbus_conn_mutex);

                    char logbuf[256];
                    snprintf(logbuf, sizeof(logbuf),
                            "D-Bus: *** FOUND Move's D-Bus socket FD %d (path=%s) ***",
                            fd, addr.sun_path);
                    shadow_log(logbuf);

                    snprintf(logbuf, sizeof(logbuf),
                            "D-Bus: Will intercept writes to FD %d via send() hook", fd);
                    shadow_log(logbuf);

                    break;
                }
            }
        }
    }

    /* Subscribe to ALL signals for discovery (tempo detection, etc.)
     * NOTE: We explicitly DON'T subscribe to com.ableton.move.ScreenReader
     * because stock Move's web server treats that as a competing client
     * and shows "single window" error. We only SEND to that interface. */
    const char *rule_all = "type='signal'";
    dbus_bus_add_match(shadow_dbus_conn, rule_all, &err);
    dbus_connection_flush(shadow_dbus_conn);

    /* Also try to eavesdrop on WebServiceAuthentication method calls (PIN flow) */
    if (!dbus_error_is_set(&err)) {
        const char *rule_auth = "type='method_call',interface='com.ableton.move.WebServiceAuthentication'";
        dbus_bus_add_match(shadow_dbus_conn, rule_auth, &err);
        if (dbus_error_is_set(&err)) {
            shadow_log("D-Bus: Auth eavesdrop match failed (expected - may need display-based PIN detection)");
            dbus_error_free(&err);
        } else {
            shadow_log("D-Bus: Auth eavesdrop match added - will monitor setSecret calls");
            dbus_connection_flush(shadow_dbus_conn);
        }
    }

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

    /* Send test announcements via the new shadow buffer architecture.
     * These are queued and injected via send() hook with coordinated serial numbers. */
    send_screenreader_announcement("Move Anything Screen Reader Test");
    sleep(1);
    send_screenreader_announcement("Screen Reader Active");

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
#else
/* Screen reader disabled at build time: keep hooks as pass-through no-ops. */
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    static int (*real_connect)(int, const struct sockaddr *, socklen_t) = NULL;
    if (!real_connect) {
        real_connect = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "connect");
    }
    if (!real_connect) return -1;
    return real_connect(sockfd, addr, addrlen);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    static ssize_t (*real_send)(int, const void *, size_t, int) = NULL;
    if (!real_send) {
        real_send = (ssize_t (*)(int, const void *, size_t, int))dlsym(RTLD_NEXT, "send");
    }
    if (!real_send) return -1;
    return real_send(sockfd, buf, len, flags);
}

static void send_screenreader_announcement(const char *text)
{
    (void)text;
}

static void shadow_inject_pending_announcements(void)
{
}

static void shadow_dbus_start(void)
{
}

static void shadow_dbus_stop(void)
{
}
#endif

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
/* Is jog encoder currently being touched? (note 9) */
static volatile int shadow_jog_touched = 0;
/* Is shift button currently held? (CC 49) - global for cross-function access */
static volatile int shadow_shift_held = 0;
/* Suppress plain volume-touch hide until touch is fully released after
 * Shift+Vol shortcut launches, avoiding a brief native volume flash. */
static volatile int shadow_block_plain_volume_hide_until_release = 0;

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

/* Overlay knobs activation mode (from shadow_control->overlay_knobs_mode) */
#define OVERLAY_KNOBS_SHIFT     0
#define OVERLAY_KNOBS_JOG_TOUCH 1
#define OVERLAY_KNOBS_OFF       2

#define SHIFT_KNOB_OVERLAY_FRAMES 60  /* ~1 second at 60fps */

/* ==========================================================================
 * Shadow Sampler - Record final mixed audio output to WAV
 * ========================================================================== */

/* Sampler state machine */
typedef enum {
    SAMPLER_IDLE = 0,
    SAMPLER_ARMED,
    SAMPLER_RECORDING
} sampler_state_t;

static sampler_state_t sampler_state = SAMPLER_IDLE;

/* Duration options (bars); 0 = unlimited (until stopped) */
static const int sampler_duration_options[] = {0, 1, 2, 4, 8, 16};
#define SAMPLER_DURATION_COUNT 6
static int sampler_duration_index = 3;  /* Default: 4 bars */

/* MIDI clock counting */
static int sampler_clock_count = 0;
static int sampler_target_pulses = 0;
static int sampler_bars_completed = 0;

/* Fallback timing (when no MIDI clock) */
static int sampler_fallback_blocks = 0;
static int sampler_fallback_target = 0;
static int sampler_clock_received = 0;

/* Tempo detection: MIDI clock BPM measurement */
static struct timespec sampler_clock_last_beat = {0, 0};
static int sampler_clock_beat_ticks = 0;     /* ticks since last beat boundary */
static float sampler_measured_bpm = 0.0f;    /* Current BPM from MIDI clock */
static float sampler_last_known_bpm = 0.0f;  /* Persists across recordings */
static int sampler_clock_active = 0;         /* Non-zero if clock ticks seen recently */
static int sampler_clock_stale_frames = 0;   /* Frames since last tick, for staleness */
#define SAMPLER_CLOCK_STALE_THRESHOLD 200    /* ~3.5 sec at 57Hz = assume clock stopped */

/* Tempo detection: settings file */
#define SAMPLER_SETTINGS_PATH "/data/UserData/move-anything/settings.txt"
static int sampler_settings_tempo = 0;       /* 0 = not yet read */

/* Tempo detection: current Set's Song.abl (declared early for D-Bus handler access) */
#define SAMPLER_SETS_DIR "/data/UserData/UserLibrary/Sets"

/* Ensure a directory exists, creating it if needed (like mkdir -p) */
static void shadow_ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) {
        const char *mkdir_argv[] = { "mkdir", "-p", dir, NULL };
        shim_run_command(mkdir_argv);
    }
}

/* Copy a single file from src_path to dst_path. Returns 1 on success. */
static int shadow_copy_file(const char *src_path, const char *dst_path) {
    FILE *sf = fopen(src_path, "r");
    if (!sf) return 0;
    fseek(sf, 0, SEEK_END);
    long sz = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(sf); return 0; }
    char *buf = malloc(sz);
    if (!buf) { fclose(sf); return 0; }
    size_t nr = fread(buf, 1, sz, sf);
    fclose(sf);
    if (nr == 0) { free(buf); return 0; }
    FILE *df = fopen(dst_path, "w");
    if (!df) { free(buf); return 0; }
    size_t nw = fwrite(buf, 1, nr, df);
    fclose(df);
    free(buf);
    if (nw != nr) { unlink(dst_path); return 0; }
    return 1;
}

/* Batch migration: copy default slot_state/ to set_state/<UUID>/ for ALL
 * existing sets. Called once at boot if .migrated marker doesn't exist.
 * This ensures all existing sets inherit the user's current slot config
 * when per-set state is first introduced. */
static void shadow_batch_migrate_sets(void) {
    char migrated_path[256];
    snprintf(migrated_path, sizeof(migrated_path), SET_STATE_DIR "/.migrated");
    struct stat mst;
    if (stat(migrated_path, &mst) == 0) return;  /* Already migrated */

    shadow_log("Batch migration: seeding per-set state for all existing sets");
    shadow_ensure_dir(SET_STATE_DIR);

    DIR *sets_dir = opendir(SAMPLER_SETS_DIR);
    if (!sets_dir) {
        shadow_log("Batch migration: cannot open Sets dir, writing .migrated anyway");
        goto write_marker;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(sets_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Each entry under Sets/ is a UUID directory */
        const char *uuid = entry->d_name;
        char set_dir[512];
        snprintf(set_dir, sizeof(set_dir), SET_STATE_DIR "/%s", uuid);

        /* Skip if already has state files */
        char test_path[768];
        snprintf(test_path, sizeof(test_path), "%s/slot_0.json", set_dir);
        struct stat tst;
        if (stat(test_path, &tst) == 0) continue;

        shadow_ensure_dir(set_dir);

        /* Copy slot state files from default dir */
        for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
            char src[512], dst[512];
            snprintf(src, sizeof(src), SLOT_STATE_DIR "/slot_%d.json", i);
            snprintf(dst, sizeof(dst), "%s/slot_%d.json", set_dir, i);
            shadow_copy_file(src, dst);

            snprintf(src, sizeof(src), SLOT_STATE_DIR "/master_fx_%d.json", i);
            snprintf(dst, sizeof(dst), "%s/master_fx_%d.json", set_dir, i);
            shadow_copy_file(src, dst);
        }

        /* Also copy shadow_chain_config.json if it exists */
        {
            char src[512], dst[512];
            snprintf(src, sizeof(src), "%s", SHADOW_CHAIN_CONFIG_PATH);
            snprintf(dst, sizeof(dst), "%s/shadow_chain_config.json", set_dir);
            shadow_copy_file(src, dst);
        }

        count++;
    }
    closedir(sets_dir);

    char m[128];
    snprintf(m, sizeof(m), "Batch migration: seeded %d sets from default slot_state", count);
    shadow_log(m);

write_marker:
    {
        FILE *mf = fopen(migrated_path, "w");
        if (mf) {
            fputs("1\n", mf);
            fclose(mf);
        }
    }
}

/* Save shadow_chain_config.json to a specific directory.
 * Writes current slot volumes, channels, forward channels, names. */
static void shadow_save_config_to_dir(const char *dir) {
    shadow_ensure_dir(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/shadow_chain_config.json", dir);

    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\n  \"slots\": [\n");
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        int display_ch = shadow_chain_slots[i].channel < 0
            ? 0 : shadow_chain_slots[i].channel + 1;
        int display_fwd = shadow_chain_slots[i].forward_channel >= 0
            ? shadow_chain_slots[i].forward_channel + 1
            : shadow_chain_slots[i].forward_channel;
        fprintf(f, "    {\"name\": \"%s\", \"channel\": %d, \"volume\": %.3f, \"forward_channel\": %d}%s\n",
                shadow_chain_slots[i].patch_name, display_ch,
                shadow_chain_slots[i].volume, display_fwd,
                i < SHADOW_CHAIN_INSTANCES - 1 ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

/* Load shadow_chain_config.json from a specific directory into shadow_chain_slots[].
 * Returns 1 if file was loaded, 0 if not found or error. */
static int shadow_load_config_from_dir(const char *dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/shadow_chain_config.json", dir);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4096) { fclose(f); return 0; }

    char *json = malloc(size + 1);
    if (!json) { fclose(f); return 0; }
    size_t nread = fread(json, 1, size, f);
    json[nread] = '\0';
    fclose(f);

    /* Parse slots - same logic as shadow_chain_load_config */
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
                if (ch >= 0 && ch <= 16)
                    shadow_chain_slots[i].channel = shadow_chain_parse_channel(ch);
            }
            cursor = chan_pos + 8;
        } else {
            cursor = name_pos + 6;
        }
        char *vol_pos = strstr(name_pos, "\"volume\"");
        if (vol_pos) {
            char *vol_colon = strchr(vol_pos, ':');
            if (vol_colon) {
                float vol = atof(vol_colon + 1);
                if (vol >= 0.0f && vol <= 1.0f)
                    shadow_chain_slots[i].volume = vol;
            }
        }
        char *fwd_pos = strstr(name_pos, "\"forward_channel\"");
        if (fwd_pos) {
            char *fwd_colon = strchr(fwd_pos, ':');
            if (fwd_colon) {
                int ch = atoi(fwd_colon + 1);
                if (ch >= -2 && ch <= 16)
                    shadow_chain_slots[i].forward_channel = (ch > 0) ? ch - 1 : ch;
            }
        }
    }
    free(json);
    shadow_ui_state_refresh();
    return 1;
}

/* Find Song.abl size for a given UUID by scanning its subdirectory.
 * Returns file size, or -1 if not found. */
static long shadow_get_song_abl_size(const char *uuid) {
    char uuid_path[512];
    snprintf(uuid_path, sizeof(uuid_path), "%s/%s", SAMPLER_SETS_DIR, uuid);
    DIR *d = opendir(uuid_path);
    if (!d) return -1;
    struct dirent *sub;
    long result = -1;
    while ((sub = readdir(d)) != NULL) {
        if (sub->d_name[0] == '.') continue;
        char song_path[768];
        snprintf(song_path, sizeof(song_path), "%s/%s/Song.abl", uuid_path, sub->d_name);
        struct stat st;
        if (stat(song_path, &st) == 0 && S_ISREG(st.st_mode)) {
            result = (long)st.st_size;
            break;
        }
    }
    closedir(d);
    return result;
}

/* Returns non-zero if set name indicates user asked for duplication.
 * This avoids false-positive copy detection for ordinary "new set" creation. */
static int shadow_set_name_looks_like_copy(const char *set_name) {
    if (!set_name || !set_name[0]) return 0;
    if (strcasestr(set_name, "copy")) return 1;
    if (strcasestr(set_name, "duplicate")) return 1;
    return 0;
}

/* Detect if a new set is a copy of an existing tracked set.
 * Compares Song.abl file sizes between the new set and all sets
 * that have per-set state directories.
 * Returns 1 and fills copy_source_uuid if a likely source is found. */
static int shadow_detect_copy_source(const char *set_name, const char *new_uuid,
                                     char *copy_source_uuid, int buf_len) {
    copy_source_uuid[0] = '\0';
    if (!shadow_set_name_looks_like_copy(set_name)) {
        return 0;
    }

    /* Get new set's Song.abl size */
    long new_size = shadow_get_song_abl_size(new_uuid);
    if (new_size <= 0) return 0;

    /* Scan set_state/ for existing tracked sets */
    DIR *state_dir = opendir(SET_STATE_DIR);
    if (!state_dir) return 0;

    int match_count = 0;
    char best_uuid[64] = "";
    struct dirent *entry;
    while ((entry = readdir(state_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (strcmp(entry->d_name, new_uuid) == 0) continue;  /* Skip self */

        /* Check if this tracked set's Song.abl matches */
        long existing_size = shadow_get_song_abl_size(entry->d_name);
        if (existing_size == new_size) {
            snprintf(best_uuid, sizeof(best_uuid), "%s", entry->d_name);
            match_count++;
        }
    }
    closedir(state_dir);

    if (match_count == 1) {
        snprintf(copy_source_uuid, buf_len, "%s", best_uuid);
        return 1;
    }

    return 0;
}

/* Handle a Set being loaded — called from Settings.json poll.
 * set_name: human-readable name (e.g. "My Song")
 * uuid: UUID directory name from Sets/<UUID>/<Name>/ path */
static void shadow_handle_set_loaded(const char *set_name, const char *uuid) {
    if (!set_name || !set_name[0]) return;

    /* Avoid re-triggering for the same set */
    if (strcmp(sampler_current_set_name, set_name) == 0 &&
        (uuid == NULL || strcmp(sampler_current_set_uuid, uuid) == 0)) {
        return;
    }

    /* Save outgoing set's config before switching */
    if (uuid && sampler_current_set_uuid[0]) {
        char outgoing_dir[512];
        snprintf(outgoing_dir, sizeof(outgoing_dir), SET_STATE_DIR "/%s",
                 sampler_current_set_uuid);
        shadow_save_config_to_dir(outgoing_dir);
        char m[256];
        snprintf(m, sizeof(m), "Set switch: saved config to %s", outgoing_dir);
        shadow_log(m);
    }

    snprintf(sampler_current_set_name, sizeof(sampler_current_set_name), "%s", set_name);
    if (uuid) {
        snprintf(sampler_current_set_uuid, sizeof(sampler_current_set_uuid), "%s", uuid);
    }

    /* Write active set UUID + name to file for shadow UI and boot persistence.
     * Format: line 1 = UUID, line 2 = set name */
    if (uuid && uuid[0]) {
        FILE *af = fopen(ACTIVE_SET_PATH, "w");
        if (af) {
            fputs(uuid, af);
            fputc('\n', af);
            fputs(set_name ? set_name : "", af);
            fclose(af);
        }
        /* Ensure per-set state directory exists */
        char incoming_dir[512];
        snprintf(incoming_dir, sizeof(incoming_dir), SET_STATE_DIR "/%s", uuid);
        shadow_ensure_dir(incoming_dir);

        /* Detect if this is a copied set — write source UUID for JS copy-on-first-use */
        char copy_source_path[512];
        snprintf(copy_source_path, sizeof(copy_source_path), "%s/copy_source.txt", incoming_dir);
        {
            /* Only detect copy for sets that don't already have state */
            char test_path[512];
            snprintf(test_path, sizeof(test_path), "%s/slot_0.json", incoming_dir);
            struct stat tst;
            if (stat(test_path, &tst) != 0) {
                /* No existing state — check if this is a copy */
                char source_uuid[64];
                if (shadow_detect_copy_source(set_name, uuid, source_uuid, sizeof(source_uuid))) {
                    /* Write copy source UUID so JS can copy from the right dir */
                    FILE *csf = fopen(copy_source_path, "w");
                    if (csf) {
                        fputs(source_uuid, csf);
                        fclose(csf);
                    }
                    /* Also copy the source's chain config to the new dir */
                    {
                        char source_dir[512];
                        snprintf(source_dir, sizeof(source_dir), SET_STATE_DIR "/%s", source_uuid);
                        char src_cfg[512], dst_cfg[512];
                        snprintf(src_cfg, sizeof(src_cfg), "%s/shadow_chain_config.json", source_dir);
                        snprintf(dst_cfg, sizeof(dst_cfg), "%s/shadow_chain_config.json", incoming_dir);
                        shadow_copy_file(src_cfg, dst_cfg);
                    }
                    char m[256];
                    snprintf(m, sizeof(m), "Set copy detected: source=%s -> new=%s", source_uuid, uuid);
                    shadow_log(m);
                }
            }
        }

        /* Load incoming set's config (volumes, channels) */
        shadow_load_config_from_dir(incoming_dir);
    }

    /* Signal shadow UI to save outgoing state and reload from new set dir */
    if (shadow_control) {
        shadow_control->ui_flags |= SHADOW_UI_FLAG_SET_CHANGED;
    }

    sampler_set_tempo = sampler_read_set_tempo(set_name);
    char msg[256];
    snprintf(msg, sizeof(msg), "Set detected: \"%s\" uuid=%s tempo=%.1f",
             set_name, uuid ? uuid : "?", sampler_set_tempo);
    shadow_log(msg);

    /* Read initial mute states from Song.abl */
    int muted[4];
    int n = shadow_read_set_mute_states(set_name, muted);
    for (int i = 0; i < n && i < SHADOW_CHAIN_INSTANCES; i++) {
        if (muted[i] && !shadow_chain_slots[i].muted) {
            shadow_chain_slots[i].pre_mute_volume = shadow_chain_slots[i].volume;
            shadow_chain_slots[i].volume = 0.0f;
            shadow_chain_slots[i].muted = 1;
            shadow_ui_state_update_slot(i);
            char m[64];
            snprintf(m, sizeof(m), "Set load: slot %d muted (saved vol=%.3f)",
                     i, shadow_chain_slots[i].pre_mute_volume);
            shadow_log(m);
        } else if (!muted[i] && shadow_chain_slots[i].muted) {
            shadow_chain_slots[i].volume = shadow_chain_slots[i].pre_mute_volume;
            shadow_chain_slots[i].muted = 0;
            shadow_ui_state_update_slot(i);
            char m[64];
            snprintf(m, sizeof(m), "Set load: slot %d unmuted (restored vol=%.3f)",
                     i, shadow_chain_slots[i].volume);
            shadow_log(m);
        }
    }
}

/* Poll Settings.json for currentSongIndex changes, then match via xattr.
 * Called periodically from ioctl tick (~every 5 seconds). */
static void shadow_poll_current_set(void)
{
    static const char settings_path[] = "/data/UserData/settings/Settings.json";

    /* Read currentSongIndex from Settings.json */
    FILE *f = fopen(settings_path, "r");
    if (!f) return;

    int song_index = -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "\"currentSongIndex\":");
        if (p) {
            p += 19;  /* skip past "currentSongIndex": */
            while (*p == ' ') p++;
            song_index = atoi(p);
            break;
        }
    }
    fclose(f);

    if (song_index < 0) return;

    /* Normal path: react when index changes.
     * Pending path: keep retrying the same unresolved index until a UUID appears. */
    if (song_index == sampler_last_song_index &&
        song_index != sampler_pending_song_index) {
        return;
    }

    int song_index_changed = (song_index != sampler_last_song_index);
    if (song_index_changed) {
        sampler_last_song_index = song_index;
    }

    /* Scan Sets directories for matching user.song-index xattr */
    DIR *sets_dir = opendir(SAMPLER_SETS_DIR);
    if (!sets_dir) return;

    int matched = 0;
    struct dirent *entry;
    while ((entry = readdir(sets_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char uuid_path[512];
        snprintf(uuid_path, sizeof(uuid_path), "%s/%s", SAMPLER_SETS_DIR, entry->d_name);

        /* Read user.song-index xattr from UUID directory */
        char xattr_val[32] = "";
        ssize_t xlen = getxattr(uuid_path, "user.song-index", xattr_val, sizeof(xattr_val) - 1);
        if (xlen <= 0) continue;
        xattr_val[xlen] = '\0';

        int idx = atoi(xattr_val);
        if (idx != song_index) continue;

        /* Found matching UUID dir — get set name from subdirectory */
        DIR *uuid_dir = opendir(uuid_path);
        if (!uuid_dir) continue;

        int handled = 0;
        struct dirent *sub;
        while ((sub = readdir(uuid_dir)) != NULL) {
            if (sub->d_name[0] == '.') continue;
            /* This subdirectory name is the set name */
            shadow_handle_set_loaded(sub->d_name, entry->d_name);
            handled = 1;
            break;
        }
        closedir(uuid_dir);
        if (handled) {
            matched = 1;
            break;
        }
    }
    closedir(sets_dir);

    if (matched) {
        sampler_pending_song_index = -1;
        return;
    }

    /* currentSongIndex changed, but the Sets/<UUID>/ folder is not materialized yet.
     * Present an immediate blank working state in a synthetic pending namespace. */
    if (song_index_changed || song_index != sampler_pending_song_index) {
        sampler_pending_set_seq++;
        if (sampler_pending_set_seq == 0) sampler_pending_set_seq = 1;
    }
    sampler_pending_song_index = song_index;

    char pending_name[128];
    char pending_uuid[64];
    snprintf(pending_name, sizeof(pending_name), "New Set %d", song_index + 1);
    snprintf(pending_uuid, sizeof(pending_uuid), "__pending-%d-%u",
             song_index, (unsigned)sampler_pending_set_seq);
    shadow_handle_set_loaded(pending_name, pending_uuid);
}

/* Tempo source tracking for display */
typedef enum {
    TEMPO_SOURCE_DEFAULT = 0,
    TEMPO_SOURCE_SETTINGS,
    TEMPO_SOURCE_SET,
    TEMPO_SOURCE_LAST_CLOCK,
    TEMPO_SOURCE_CLOCK
} tempo_source_t;

/* Overlay state */
static int sampler_overlay_active = 0;
static int sampler_overlay_timeout = 0;
#define SAMPLER_OVERLAY_DONE_FRAMES 90  /* ~1.5 seconds for "saved" message */

/* Source selection */
typedef enum {
    SAMPLER_SOURCE_RESAMPLE = 0,
    SAMPLER_SOURCE_MOVE_INPUT
} sampler_source_t;
static sampler_source_t sampler_source = SAMPLER_SOURCE_RESAMPLE;

/* Menu cursor for full-screen UI */
typedef enum {
    SAMPLER_MENU_SOURCE = 0,
    SAMPLER_MENU_DURATION,
    SAMPLER_MENU_COUNT
} sampler_menu_item_t;
static int sampler_menu_cursor = SAMPLER_MENU_SOURCE;

/* VU meter */
static int16_t sampler_vu_peak = 0;
static int sampler_vu_hold_frames = 0;
#define SAMPLER_VU_HOLD_DURATION 8      /* ~140ms hold at 57Hz */
#define SAMPLER_VU_DECAY_RATE 1500      /* Raw amplitude decay per frame */

/* Full-screen mode flag */
static int sampler_fullscreen_active = 0;

/* WAV file header structure (matches chain_host.c) */
typedef struct {
    char riff_id[4];
    uint32_t file_size;
    char wave_id[4];
    char fmt_id[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_id[4];
    uint32_t data_size;
} sampler_wav_header_t;

/* Ring buffer for threaded recording */
#define SAMPLER_SAMPLE_RATE 44100
#define SAMPLER_NUM_CHANNELS 2
#define SAMPLER_BITS_PER_SAMPLE 16
#define SAMPLER_RING_BUFFER_SECONDS 2
#define SAMPLER_RING_BUFFER_SAMPLES (SAMPLER_SAMPLE_RATE * SAMPLER_RING_BUFFER_SECONDS)
#define SAMPLER_RING_BUFFER_SIZE (SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS * sizeof(int16_t))
#define SAMPLER_RECORDINGS_DIR "/data/UserData/UserLibrary/Samples/Move Everything"

/* Execute a command safely using fork/execvp instead of system() */
static int shim_run_command(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(STDOUT_FILENO, STDERR_FILENO);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static FILE *sampler_wav_file = NULL;
static uint32_t sampler_samples_written = 0;
static char sampler_current_recording[256] = "";
static int16_t *sampler_ring_buffer = NULL;
static size_t sampler_ring_write_pos = 0;
static size_t sampler_ring_read_pos = 0;
static pthread_t sampler_writer_thread;
static pthread_mutex_t sampler_ring_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sampler_ring_cond = PTHREAD_COND_INITIALIZER;
static volatile int sampler_writer_running = 0;
static volatile int sampler_writer_should_exit = 0;

/* ==========================================================================
 * Skipback Buffer - always-rolling 30-second capture, dump via Shift+Capture
 * ========================================================================== */
#define SKIPBACK_SECONDS 30
#define SKIPBACK_SAMPLES (SAMPLER_SAMPLE_RATE * SKIPBACK_SECONDS)
#define SKIPBACK_BUFFER_SIZE (SKIPBACK_SAMPLES * SAMPLER_NUM_CHANNELS * sizeof(int16_t))
#define SKIPBACK_DIR "/data/UserData/UserLibrary/Samples/Move Everything/Skipback"

static int16_t *skipback_buffer = NULL;
static volatile size_t skipback_write_pos = 0;  /* In samples (L+R pairs count as 2) */
static volatile int skipback_buffer_full = 0;   /* Has wrapped at least once */
static int skipback_saving = 0;                  /* Writer thread active (accessed atomically) */
static pthread_t skipback_writer_thread;
static volatile int skipback_overlay_timeout = 0;  /* Frames remaining for "saved" overlay */
#define SKIPBACK_OVERLAY_FRAMES 171  /* ~3 seconds at ~57Hz */

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
    uint8_t okm = shadow_control ? shadow_control->overlay_knobs_mode : OVERLAY_KNOBS_SHIFT;
    if (okm == OVERLAY_KNOBS_OFF) return;
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

    /* Screen reader: announce param and value */
    {
        char sr_buf[192];
        snprintf(sr_buf, sizeof(sr_buf), "%s, %s", shift_knob_overlay_param, shift_knob_overlay_value);
        send_screenreader_announcement(sr_buf);
    }

    shadow_overlay_sync();
}

/* ==========================================================================
 * Shadow Sampler - WAV, ring buffer, recording, audio capture, MIDI clock
 * ========================================================================== */

static void sampler_write_wav_header(FILE *f, uint32_t data_size) {
    sampler_wav_header_t header;
    memcpy(header.riff_id, "RIFF", 4);
    header.file_size = 36 + data_size;
    memcpy(header.wave_id, "WAVE", 4);
    memcpy(header.fmt_id, "fmt ", 4);
    header.fmt_size = 16;
    header.audio_format = 1;  /* PCM */
    header.num_channels = SAMPLER_NUM_CHANNELS;
    header.sample_rate = SAMPLER_SAMPLE_RATE;
    header.byte_rate = SAMPLER_SAMPLE_RATE * SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
    header.block_align = SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
    header.bits_per_sample = SAMPLER_BITS_PER_SAMPLE;
    memcpy(header.data_id, "data", 4);
    header.data_size = data_size;
    fseek(f, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, f);
}

static size_t sampler_ring_available_write(void) {
    size_t write_pos = __atomic_load_n(&sampler_ring_write_pos, __ATOMIC_ACQUIRE);
    size_t read_pos = __atomic_load_n(&sampler_ring_read_pos, __ATOMIC_ACQUIRE);
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;
    if (write_pos >= read_pos)
        return buffer_samples - (write_pos - read_pos) - 1;
    else
        return read_pos - write_pos - 1;
}

static size_t sampler_ring_available_read(void) {
    size_t write_pos = __atomic_load_n(&sampler_ring_write_pos, __ATOMIC_ACQUIRE);
    size_t read_pos = __atomic_load_n(&sampler_ring_read_pos, __ATOMIC_ACQUIRE);
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;
    if (write_pos >= read_pos)
        return write_pos - read_pos;
    else
        return buffer_samples - (read_pos - write_pos);
}

static void *sampler_writer_thread_func(void *arg) {
    (void)arg;
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;
    size_t write_chunk = SAMPLER_SAMPLE_RATE * SAMPLER_NUM_CHANNELS / 4;  /* ~250ms */

    while (1) {
        pthread_mutex_lock(&sampler_ring_mutex);
        while (sampler_ring_available_read() < write_chunk && !sampler_writer_should_exit) {
            pthread_cond_wait(&sampler_ring_cond, &sampler_ring_mutex);
        }
        int should_exit = sampler_writer_should_exit;
        pthread_mutex_unlock(&sampler_ring_mutex);

        size_t available = sampler_ring_available_read();
        while (available > 0 && sampler_wav_file) {
            size_t read_pos = __atomic_load_n(&sampler_ring_read_pos, __ATOMIC_ACQUIRE);
            size_t to_end = buffer_samples - read_pos;
            size_t to_write = (available < to_end) ? available : to_end;
            fwrite(&sampler_ring_buffer[read_pos], sizeof(int16_t), to_write, sampler_wav_file);
            sampler_samples_written += to_write / SAMPLER_NUM_CHANNELS;
            __atomic_store_n(&sampler_ring_read_pos, (read_pos + to_write) % buffer_samples, __ATOMIC_RELEASE);
            available = sampler_ring_available_read();
        }

        if (should_exit) break;
    }
    return NULL;
}

/* Read tempo from the current Set's Song.abl file.
 * Scans Sets/<uuid>/<SetName>/Song.abl for all UUIDs.
 * Picks the most recently modified match if there are duplicates.
 * Does a simple string search for "tempo": in the JSON. */
static float sampler_read_set_tempo(const char *set_name) {
    if (!set_name || !set_name[0]) return 0.0f;

    DIR *sets_dir = opendir(SAMPLER_SETS_DIR);
    if (!sets_dir) return 0.0f;

    char best_path[512] = "";
    time_t best_mtime = 0;
    struct dirent *entry;

    while ((entry = readdir(sets_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Build path: Sets/<uuid>/<set_name>/Song.abl */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s/%s/Song.abl",
                 SAMPLER_SETS_DIR, entry->d_name, set_name);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (st.st_mtime > best_mtime) {
                best_mtime = st.st_mtime;
                snprintf(best_path, sizeof(best_path), "%s", path);
            }
        }
    }
    closedir(sets_dir);

    if (best_path[0] == '\0') return 0.0f;

    /* Read file and search for "tempo": <value> */
    FILE *f = fopen(best_path, "r");
    if (!f) return 0.0f;

    float tempo = 0.0f;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "\"tempo\":");
        if (p) {
            p += 8;  /* skip past "tempo": */
            while (*p == ' ') p++;
            tempo = strtof(p, NULL);
            if (tempo >= 20.0f && tempo <= 999.0f) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Set tempo: %.1f BPM from %s", tempo, best_path);
                shadow_log(msg);
                break;
            }
            tempo = 0.0f;
        }
    }
    fclose(f);
    return tempo;
}

/* Read track mute states from Song.abl for the given set name.
 * Track-level "speakerOn" fields are at exactly 8-space indent.
 * Results stored in muted_out[0..3]: 1=muted, 0=not muted.
 * Returns number of tracks found (0 on failure). */
static int shadow_read_set_mute_states(const char *set_name, int muted_out[4]) {
    memset(muted_out, 0, 4 * sizeof(int));
    if (!set_name || !set_name[0]) return 0;

    DIR *sets_dir = opendir(SAMPLER_SETS_DIR);
    if (!sets_dir) return 0;

    char best_path[512] = "";
    time_t best_mtime = 0;
    struct dirent *entry;

    while ((entry = readdir(sets_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s/%s/Song.abl",
                 SAMPLER_SETS_DIR, entry->d_name, set_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (st.st_mtime > best_mtime) {
                best_mtime = st.st_mtime;
                snprintf(best_path, sizeof(best_path), "%s", path);
            }
        }
    }
    closedir(sets_dir);

    if (best_path[0] == '\0') return 0;

    FILE *f = fopen(best_path, "r");
    if (!f) return 0;

    /* Parse by tracking brace/bracket depth.
     * Track-level structure: "tracks": [ { ... "mixer": { "speakerOn": X } } ]
     * Track-level mixer is at brace_depth=3 (root=1, track=2, mixer=3).
     * Deeper mixers (drum cells, device chains) are at depth 5+. */
    int track_count = 0;
    int brace_depth = 0;
    int in_tracks = 0;       /* 1 after seeing "tracks" key */
    int bracket_depth = 0;   /* track [] nesting after "tracks" */
    char line[512];
    while (fgets(line, sizeof(line), f) && track_count < 4) {
        /* Count braces and brackets on this line */
        for (char *p = line; *p; p++) {
            if (*p == '{') brace_depth++;
            else if (*p == '}') brace_depth--;
            else if (*p == '[') bracket_depth++;
            else if (*p == ']') bracket_depth--;
        }

        /* Detect "tracks" key to know we're in the tracks array */
        if (!in_tracks && strstr(line, "\"tracks\"")) {
            in_tracks = 1;
        }

        /* Track-level speakerOn: inside tracks array, at brace depth 3 */
        if (in_tracks && brace_depth == 3 && strstr(line, "\"speakerOn\"")) {
            muted_out[track_count] = strstr(line, "false") ? 1 : 0;
            track_count++;
        }
    }
    fclose(f);

    if (track_count > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Set mute states from %s: [%d,%d,%d,%d]",
                 set_name, muted_out[0], muted_out[1], muted_out[2], muted_out[3]);
        shadow_log(msg);
    }
    return track_count;
}

/* Read tempo_bpm from Move Anything settings file */
static int sampler_read_settings_tempo(void) {
    FILE *f = fopen(SAMPLER_SETTINGS_PATH, "r");
    if (!f) return 0;

    char line[256];
    int bpm = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        if (strcmp(line, "tempo_bpm") == 0) {
            bpm = atoi(eq + 1);
            if (bpm < 20) bpm = 20;
            if (bpm > 300) bpm = 300;
            break;
        }
    }
    fclose(f);
    return bpm;
}

/* Get best available BPM using fallback chain:
 * 1. MIDI clock (if actively ticking)
 * 2. Last measured clock BPM (from previous session)
 * 3. Current Set's tempo (from Song.abl)
 * 4. Settings file tempo_bpm
 * 5. Default 120 BPM */
static float sampler_get_bpm(tempo_source_t *source) {
    /* 1. Active MIDI clock */
    if (sampler_clock_active && sampler_measured_bpm >= 20.0f) {
        if (source) *source = TEMPO_SOURCE_CLOCK;
        return sampler_measured_bpm;
    }

    /* 2. Last measured clock BPM */
    if (sampler_last_known_bpm >= 20.0f) {
        if (source) *source = TEMPO_SOURCE_LAST_CLOCK;
        return sampler_last_known_bpm;
    }

    /* 3. Current Set's tempo */
    if (sampler_set_tempo >= 20.0f) {
        if (source) *source = TEMPO_SOURCE_SET;
        return sampler_set_tempo;
    }

    /* 4. Settings file tempo */
    if (sampler_settings_tempo == 0) {
        sampler_settings_tempo = sampler_read_settings_tempo();
        if (sampler_settings_tempo == 0) sampler_settings_tempo = -1;  /* mark as tried */
    }
    if (sampler_settings_tempo > 0) {
        if (source) *source = TEMPO_SOURCE_SETTINGS;
        return (float)sampler_settings_tempo;
    }

    /* 5. Default */
    if (source) *source = TEMPO_SOURCE_DEFAULT;
    return 120.0f;
}

/* Build a screen reader string describing current sampler menu item */
static void sampler_announce_menu_item(void) {
    char sr_buf[128];
    if (sampler_menu_cursor == SAMPLER_MENU_SOURCE) {
        snprintf(sr_buf, sizeof(sr_buf), "Source, %s",
                 sampler_source == SAMPLER_SOURCE_RESAMPLE ? "Resample" : "Move Input");
    } else if (sampler_menu_cursor == SAMPLER_MENU_DURATION) {
        int bars = sampler_duration_options[sampler_duration_index];
        if (bars == 0)
            snprintf(sr_buf, sizeof(sr_buf), "Duration, Until stop");
        else
            snprintf(sr_buf, sizeof(sr_buf), "Duration, %d bar%s", bars, bars > 1 ? "s" : "");
    } else {
        return;
    }
    send_screenreader_announcement(sr_buf);
}

static void sampler_start_recording(void) {
    if (sampler_writer_running) return;

    /* Create recordings directory (skip fork if it already exists to avoid audio glitch) */
    {
        struct stat st;
        if (stat(SAMPLER_RECORDINGS_DIR, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", SAMPLER_RECORDINGS_DIR, NULL };
            shim_run_command(mkdir_argv);
        }
    }

    /* Generate filename with timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm_info = localtime_r(&now, &tm_buf);
    snprintf(sampler_current_recording, sizeof(sampler_current_recording),
             SAMPLER_RECORDINGS_DIR "/sample_%04d%02d%02d_%02d%02d%02d.wav",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    /* Allocate ring buffer */
    sampler_ring_buffer = malloc(SAMPLER_RING_BUFFER_SIZE);
    if (!sampler_ring_buffer) {
        shadow_log("Sampler: failed to allocate ring buffer");
        send_screenreader_announcement("Recording failed");
        return;
    }

    /* Open file */
    sampler_wav_file = fopen(sampler_current_recording, "wb");
    if (!sampler_wav_file) {
        shadow_log("Sampler: failed to open WAV file");
        send_screenreader_announcement("Recording failed");
        free(sampler_ring_buffer);
        sampler_ring_buffer = NULL;
        return;
    }

    /* Initialize state */
    sampler_samples_written = 0;
    __atomic_store_n(&sampler_ring_write_pos, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&sampler_ring_read_pos, 0, __ATOMIC_RELEASE);
    sampler_writer_should_exit = 0;
    sampler_clock_count = 0;
    sampler_bars_completed = 0;
    sampler_clock_received = 0;
    sampler_fallback_blocks = 0;

    /* Calculate target: bars * 4 beats * 24 PPQN (0 = unlimited) */
    int bars = sampler_duration_options[sampler_duration_index];
    if (bars > 0) {
        sampler_target_pulses = bars * 4 * 24;
        /* Fallback: use best available BPM for timing when no MIDI clock.
         * seconds = bars * 4 beats / (BPM / 60)
         * blocks = seconds * 44100 / 128 */
        tempo_source_t src;
        float bpm = sampler_get_bpm(&src);
        float seconds = bars * 4.0f * 60.0f / bpm;
        sampler_fallback_target = (int)(seconds * 44100.0f / 128.0f);
        {
            char msg[128];
            const char *src_names[] = {"default", "settings", "set", "last clock", "clock"};
            snprintf(msg, sizeof(msg), "Sampler: using %.1f BPM (%s) for fallback timing",
                     bpm, src_names[src]);
            shadow_log(msg);
        }
    } else {
        sampler_target_pulses = 0;  /* No auto-stop */
        sampler_fallback_target = 0;
    }

    /* Write placeholder header */
    sampler_write_wav_header(sampler_wav_file, 0);

    /* Start writer thread */
    if (pthread_create(&sampler_writer_thread, NULL, sampler_writer_thread_func, NULL) != 0) {
        shadow_log("Sampler: failed to create writer thread");
        send_screenreader_announcement("Recording failed");
        fclose(sampler_wav_file);
        sampler_wav_file = NULL;
        free(sampler_ring_buffer);
        sampler_ring_buffer = NULL;
        return;
    }

    sampler_writer_running = 1;
    sampler_state = SAMPLER_RECORDING;
    sampler_overlay_active = 1;
    sampler_overlay_timeout = 0;  /* Stay active while recording */
    shadow_overlay_sync();

    char msg[256];
    if (bars > 0)
        snprintf(msg, sizeof(msg), "Sampler: recording started (%d bars) -> %s",
                 bars, sampler_current_recording);
    else
        snprintf(msg, sizeof(msg), "Sampler: recording started (until stopped) -> %s",
                 sampler_current_recording);
    shadow_log(msg);
}

static void sampler_stop_recording(void) {
    if (!sampler_writer_running) return;

    shadow_log("Sampler: stopping recording");

    /* Signal writer thread to exit */
    pthread_mutex_lock(&sampler_ring_mutex);
    sampler_writer_should_exit = 1;
    pthread_cond_signal(&sampler_ring_cond);
    pthread_mutex_unlock(&sampler_ring_mutex);

    pthread_join(sampler_writer_thread, NULL);
    sampler_writer_running = 0;

    /* Update WAV header with final size */
    if (sampler_wav_file) {
        uint32_t data_size = sampler_samples_written * SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
        sampler_write_wav_header(sampler_wav_file, data_size);
        fclose(sampler_wav_file);
        sampler_wav_file = NULL;
    }

    /* Free ring buffer */
    if (sampler_ring_buffer) {
        free(sampler_ring_buffer);
        sampler_ring_buffer = NULL;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Sampler: saved %s (%u samples, %.1f sec)",
             sampler_current_recording, sampler_samples_written,
             (float)sampler_samples_written / SAMPLER_SAMPLE_RATE);
    shadow_log(msg);

    sampler_current_recording[0] = '\0';
    sampler_state = SAMPLER_IDLE;

    send_screenreader_announcement("Sample saved");

    /* Keep fullscreen active for "saved" message, then timeout */
    sampler_overlay_active = 1;
    sampler_overlay_timeout = SAMPLER_OVERLAY_DONE_FRAMES;
    shadow_overlay_sync();
}

static void sampler_capture_audio(void) {
    if (sampler_state != SAMPLER_RECORDING || !sampler_ring_buffer) return;

    /* Select audio source based on sampler_source setting */
    int16_t *audio = NULL;
    if (sampler_source == SAMPLER_SOURCE_RESAMPLE && global_mmap_addr) {
        audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    } else if (sampler_source == SAMPLER_SOURCE_MOVE_INPUT && hardware_mmap_addr) {
        audio = (int16_t *)(hardware_mmap_addr + AUDIO_IN_OFFSET);
    }
    if (!audio) return;

    size_t samples_to_write = FRAMES_PER_BLOCK * SAMPLER_NUM_CHANNELS;
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;

    /* Write to ring buffer if space available (drop if full, never block) */
    if (sampler_ring_available_write() >= samples_to_write) {
        size_t write_pos = __atomic_load_n(&sampler_ring_write_pos, __ATOMIC_ACQUIRE);
        for (size_t i = 0; i < samples_to_write; i++) {
            sampler_ring_buffer[write_pos] = audio[i];
            write_pos = (write_pos + 1) % buffer_samples;
        }
        __atomic_store_n(&sampler_ring_write_pos, write_pos, __ATOMIC_RELEASE);

        /* Signal writer thread */
        pthread_mutex_lock(&sampler_ring_mutex);
        pthread_cond_signal(&sampler_ring_cond);
        pthread_mutex_unlock(&sampler_ring_mutex);
    }

    /* Fallback timeout: count blocks if no MIDI clock received (skip if unlimited) */
    if (!sampler_clock_received && sampler_fallback_target > 0) {
        sampler_fallback_blocks++;
        if (sampler_fallback_blocks >= sampler_fallback_target) {
            shadow_log("Sampler: fallback timeout reached (no MIDI clock)");
            sampler_stop_recording();
        }
    }
}

static void sampler_on_clock(uint8_t status) {
    if (status == 0xF8) {
        /* MIDI Clock tick - always measure BPM regardless of sampler state */
        sampler_clock_active = 1;
        sampler_clock_stale_frames = 0;
        sampler_clock_beat_ticks++;

        /* Measure BPM every 24 ticks (one beat) */
        if (sampler_clock_beat_ticks >= 24) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (sampler_clock_last_beat.tv_sec > 0) {
                double elapsed = (now.tv_sec - sampler_clock_last_beat.tv_sec)
                               + (now.tv_nsec - sampler_clock_last_beat.tv_nsec) / 1e9;
                if (elapsed > 0.1 && elapsed < 10.0) {  /* Sanity: 6-600 BPM */
                    sampler_measured_bpm = 60.0f / (float)elapsed;
                    sampler_last_known_bpm = sampler_measured_bpm;
                }
            }
            sampler_clock_last_beat = now;
            sampler_clock_beat_ticks = 0;
        }

        /* Recording-specific: count pulses for auto-stop */
        if (sampler_state == SAMPLER_RECORDING) {
            sampler_clock_received = 1;
            sampler_clock_count++;

            /* Calculate bars completed (24 PPQN * 4 beats = 96 pulses per bar) */
            sampler_bars_completed = sampler_clock_count / 96;

            /* Auto-stop when target reached (skip if unlimited mode) */
            if (sampler_target_pulses > 0 && sampler_clock_count >= sampler_target_pulses) {
                shadow_log("Sampler: target duration reached via MIDI clock");
                sampler_stop_recording();
            }
        }
    } else if (status == 0xFA) {
        /* MIDI Start - trigger recording if armed */
        if (sampler_state == SAMPLER_ARMED) {
            shadow_log("Sampler: triggered by MIDI Start");
            sampler_start_recording();
        }
    }
    else if (status == 0xFC) {
        /* MIDI Stop - stop recording regardless of mode */
        if (sampler_state == SAMPLER_RECORDING) {
            shadow_log("Sampler: stopped by MIDI Stop");
            sampler_stop_recording();
        }
    }
}

/* Skipback: allocate buffer on first use */
static void skipback_init(void) {
    if (skipback_buffer) return;
    skipback_buffer = (int16_t *)calloc(SKIPBACK_SAMPLES * SAMPLER_NUM_CHANNELS, sizeof(int16_t));
    if (skipback_buffer) {
        skipback_write_pos = 0;
        skipback_buffer_full = 0;
        shadow_log("Skipback: allocated 30s rolling buffer");
    } else {
        shadow_log("Skipback: failed to allocate buffer");
    }
}

/* Skipback: capture one audio block into rolling buffer */
static void skipback_capture(int16_t *audio) {
    if (!skipback_buffer || !audio || __atomic_load_n(&skipback_saving, __ATOMIC_ACQUIRE)) return;

    size_t total_samples = SKIPBACK_SAMPLES * SAMPLER_NUM_CHANNELS;
    size_t block_samples = FRAMES_PER_BLOCK * SAMPLER_NUM_CHANNELS;
    size_t wp = skipback_write_pos;

    for (size_t i = 0; i < block_samples; i++) {
        skipback_buffer[wp] = audio[i];
        wp = (wp + 1) % total_samples;
    }

    if (!skipback_buffer_full && wp < skipback_write_pos)
        skipback_buffer_full = 1;
    skipback_write_pos = wp;
}

/* Skipback: writer thread - dumps ring buffer to WAV */
static void *skipback_writer_func(void *arg) {
    (void)arg;

    /* Create directory (skip fork if it already exists to avoid audio glitch) */
    {
        struct stat st;
        if (stat(SKIPBACK_DIR, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", SKIPBACK_DIR, NULL };
            shim_run_command(mkdir_argv);
        }
    }

    /* Generate filename */
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm_info = localtime_r(&now, &tm_buf);
    char path[256];
    snprintf(path, sizeof(path),
             SKIPBACK_DIR "/skipback_%04d%02d%02d_%02d%02d%02d.wav",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    FILE *f = fopen(path, "wb");
    if (!f) {
        shadow_log("Skipback: failed to open WAV file");
        send_screenreader_announcement("Skipback failed");
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return NULL;
    }

    /* Determine how much data to write */
    size_t total_samples = SKIPBACK_SAMPLES * SAMPLER_NUM_CHANNELS;
    size_t wp = skipback_write_pos;
    size_t data_samples;
    size_t start_pos;

    if (skipback_buffer_full) {
        /* Buffer has wrapped: write from wp (oldest) to wp (newest) */
        data_samples = total_samples;
        start_pos = wp;
    } else {
        /* Buffer hasn't wrapped: write from 0 to wp */
        data_samples = wp;
        start_pos = 0;
    }

    if (data_samples == 0) {
        shadow_log("Skipback: no audio captured yet");
        send_screenreader_announcement("No audio captured yet");
        fclose(f);
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return NULL;
    }

    /* Write WAV header */
    uint32_t data_bytes = (uint32_t)(data_samples * sizeof(int16_t));
    sampler_wav_header_t hdr;
    memcpy(hdr.riff_id, "RIFF", 4);
    hdr.file_size = 36 + data_bytes;
    memcpy(hdr.wave_id, "WAVE", 4);
    memcpy(hdr.fmt_id, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_format = 1;
    hdr.num_channels = SAMPLER_NUM_CHANNELS;
    hdr.sample_rate = SAMPLER_SAMPLE_RATE;
    hdr.byte_rate = SAMPLER_SAMPLE_RATE * SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
    hdr.block_align = SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
    hdr.bits_per_sample = SAMPLER_BITS_PER_SAMPLE;
    memcpy(hdr.data_id, "data", 4);
    hdr.data_size = data_bytes;
    fwrite(&hdr, sizeof(hdr), 1, f);

    /* Write audio data from ring buffer */
    size_t pos = start_pos;
    size_t remaining = data_samples;
    while (remaining > 0) {
        size_t chunk = remaining;
        if (pos + chunk > total_samples)
            chunk = total_samples - pos;
        fwrite(skipback_buffer + pos, sizeof(int16_t), chunk, f);
        pos = (pos + chunk) % total_samples;
        remaining -= chunk;
    }

    fclose(f);

    uint32_t frames = (uint32_t)(data_samples / SAMPLER_NUM_CHANNELS);
    char msg[256];
    snprintf(msg, sizeof(msg), "Skipback: saved %s (%.1f sec)",
             path, (float)frames / SAMPLER_SAMPLE_RATE);
    shadow_log(msg);

    skipback_overlay_timeout = SKIPBACK_OVERLAY_FRAMES;
    shadow_overlay_sync();
    send_screenreader_announcement("Skipback saved");
    __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
    return NULL;
}

/* Skipback: trigger save from main thread */
static void skipback_trigger_save(void) {
    if (__atomic_load_n(&skipback_saving, __ATOMIC_ACQUIRE)) {
        send_screenreader_announcement("Skipback already saving");
        return;
    }
    if (!skipback_buffer) {
        send_screenreader_announcement("Skipback not available");
        return;
    }
    __atomic_store_n(&skipback_saving, 1, __ATOMIC_RELEASE);
    __sync_synchronize();

    send_screenreader_announcement("Saving skipback");

    pthread_t t;
    if (pthread_create(&t, NULL, skipback_writer_func, NULL) != 0) {
        shadow_log("Skipback: failed to create writer thread");
        send_screenreader_announcement("Skipback failed");
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return;
    }
    pthread_detach(t);
    shadow_log("Skipback: saving last 30 seconds...");
}

/* VU meter update - reads audio from selected source */
static void sampler_update_vu(void) {
    if (!sampler_fullscreen_active) return;

    int16_t *audio = NULL;
    if (sampler_source == SAMPLER_SOURCE_RESAMPLE && global_mmap_addr) {
        audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    } else if (sampler_source == SAMPLER_SOURCE_MOVE_INPUT && hardware_mmap_addr) {
        audio = (int16_t *)(hardware_mmap_addr + AUDIO_IN_OFFSET);
    }

    if (!audio) return;

    /* Scan 128 stereo frames, find peak absolute value */
    int16_t frame_peak = 0;
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int16_t val = audio[i];
        if (val < 0) val = -val;
        if (val > frame_peak) frame_peak = val;
    }

    /* Peak hold and decay */
    if (frame_peak >= sampler_vu_peak) {
        sampler_vu_peak = frame_peak;
        sampler_vu_hold_frames = SAMPLER_VU_HOLD_DURATION;
    } else if (sampler_vu_hold_frames > 0) {
        sampler_vu_hold_frames--;
    } else {
        int16_t decayed = sampler_vu_peak - SAMPLER_VU_DECAY_RATE;
        sampler_vu_peak = (decayed < 0) ? 0 : decayed;
    }
}

/* Skipback overlay - modal box over Move's display */
static void overlay_draw_skipback(uint8_t *buf) {
    int box_w = 110;
    int box_h = 20;
    int box_x = (128 - box_w) / 2;
    int box_y = (64 - box_h) / 2;

    /* Draw background and border */
    overlay_fill_rect(buf, box_x, box_y, box_w, box_h, 0);
    overlay_fill_rect(buf, box_x, box_y, box_w, 1, 1);
    overlay_fill_rect(buf, box_x, box_y + box_h - 1, box_w, 1, 1);
    overlay_fill_rect(buf, box_x, box_y, 1, box_h, 1);
    overlay_fill_rect(buf, box_x + box_w - 1, box_y, 1, box_h, 1);

    overlay_draw_string(buf, box_x + 8, box_y + 7, "Skipback saved!", 1);
}

/* Blit a rectangular region from src (shadow display) onto dst (native display).
 * Both buffers are 128x64 1bpp (1024 bytes) in SSD1306 page format:
 *   byte_index = page * 128 + x,  where page = y / 8
 *   bit = y % 8  (LSB = top pixel in page)
 * Pixels within the rect are replaced; pixels outside are untouched. */
static void overlay_blit_rect(uint8_t *dst, const uint8_t *src,
                               int rx, int ry, int rw, int rh) {
    int x_end = rx + rw;
    int y_end = ry + rh;
    if (x_end > 128) x_end = 128;
    if (y_end > 64) y_end = 64;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;

    for (int y = ry; y < y_end; y++) {
        int page = y / 8;
        int bit  = y % 8;
        uint8_t mask = (uint8_t)(1 << bit);

        for (int x = rx; x < x_end; x++) {
            int idx = page * 128 + x;
            dst[idx] = (dst[idx] & ~mask) | (src[idx] & mask);
        }
    }
}

/* Sampler overlay drawing */
static void overlay_draw_sampler(uint8_t *buf) {
    if (!sampler_fullscreen_active) return;

    /* Clear entire display for full-screen takeover */
    memset(buf, 0, 1024);

    /* 5x7 font: char width ~6px, row height ~8px, 128x64 display */
    static int recording_flash_counter = 0;

    if (sampler_state == SAMPLER_ARMED) {
        /* Row 0: Title centered */
        overlay_draw_string(buf, 10, 0, "QUANTIZED SAMPLER", 1);

        /* Row 2: Source with cursor */
        char src_str[32];
        snprintf(src_str, sizeof(src_str), "%cSource: %s",
                 sampler_menu_cursor == SAMPLER_MENU_SOURCE ? '>' : ' ',
                 sampler_source == SAMPLER_SOURCE_RESAMPLE ? "Resample" : "Move Input");
        overlay_draw_string(buf, 0, 16, src_str, 1);

        /* Row 3: Duration with cursor */
        char dur_str[32];
        int bars = sampler_duration_options[sampler_duration_index];
        if (bars == 0)
            snprintf(dur_str, sizeof(dur_str), "%cDur: Until stop",
                     sampler_menu_cursor == SAMPLER_MENU_DURATION ? '>' : ' ');
        else
            snprintf(dur_str, sizeof(dur_str), "%cDur: %d bar%s",
                     sampler_menu_cursor == SAMPLER_MENU_DURATION ? '>' : ' ',
                     bars, bars > 1 ? "s" : "");
        overlay_draw_string(buf, 0, 24, dur_str, 1);

        /* Row 6: VU meter bar (120px wide starting at x=4) */
        int vu_w = 120;
        int vu_x = 4;
        int vu_y = 48;
        int vu_h = 5;
        /* Border */
        overlay_fill_rect(buf, vu_x, vu_y, vu_w, 1, 1);
        overlay_fill_rect(buf, vu_x, vu_y + vu_h - 1, vu_w, 1, 1);
        overlay_fill_rect(buf, vu_x, vu_y, 1, vu_h, 1);
        overlay_fill_rect(buf, vu_x + vu_w - 1, vu_y, 1, vu_h, 1);
        /* Fill based on VU peak (32767 = full) */
        /* Log scale: map -48dB..0dB to 0..bar width */
        float vu_norm = 0.0f;
        if (sampler_vu_peak > 0) {
            float db = 20.0f * log10f((float)sampler_vu_peak / 32767.0f);
            vu_norm = (db + 48.0f) / 48.0f;  /* -48dB=0, 0dB=1 */
            if (vu_norm < 0.0f) vu_norm = 0.0f;
            if (vu_norm > 1.0f) vu_norm = 1.0f;
        }
        int fill_w = (int)(vu_norm * (vu_w - 2));
        if (fill_w > vu_w - 2) fill_w = vu_w - 2;
        if (fill_w > 0)
            overlay_fill_rect(buf, vu_x + 1, vu_y + 1, fill_w, vu_h - 2, 1);

        /* Row 7: Instructions */
        overlay_draw_string(buf, 0, 56, "Play/Note to record", 1);

    } else if (sampler_state == SAMPLER_RECORDING) {
        /* Row 0: Flashing title (~4Hz at ~57fps = toggle every 14 frames) */
        recording_flash_counter = (recording_flash_counter + 1) % 28;
        if ((recording_flash_counter / 14) == 0)
            overlay_draw_string(buf, 16, 0, "** RECORDING **", 1);

        /* Row 2: Source (locked, no cursor) */
        char src_str[32];
        snprintf(src_str, sizeof(src_str), " Source: %s",
                 sampler_source == SAMPLER_SOURCE_RESAMPLE ? "Resample" : "Move Input");
        overlay_draw_string(buf, 0, 16, src_str, 1);

        /* Row 3: Progress */
        int bars = sampler_duration_options[sampler_duration_index];
        char bar_str[32];
        if (bars == 0) {
            float secs = (float)sampler_samples_written / SAMPLER_SAMPLE_RATE;
            snprintf(bar_str, sizeof(bar_str), " Elapsed: %.1fs", secs);
        } else {
            int current_bar = sampler_bars_completed + 1;
            if (current_bar > bars) current_bar = bars;
            snprintf(bar_str, sizeof(bar_str), " Bar %d / %d", current_bar, bars);
        }
        overlay_draw_string(buf, 0, 24, bar_str, 1);

        /* Row 4: Progress bar (fixed duration only) */
        if (bars > 0) {
            int prog_x = 4;
            int prog_y = 32;
            int prog_w = 120;
            int prog_h = 5;
            overlay_fill_rect(buf, prog_x, prog_y, prog_w, 1, 1);
            overlay_fill_rect(buf, prog_x, prog_y + prog_h - 1, prog_w, 1, 1);
            overlay_fill_rect(buf, prog_x, prog_y, 1, prog_h, 1);
            overlay_fill_rect(buf, prog_x + prog_w - 1, prog_y, 1, prog_h, 1);
            float progress = 0.0f;
            if (sampler_clock_received && sampler_target_pulses > 0)
                progress = (float)sampler_clock_count / (float)sampler_target_pulses;
            else if (sampler_fallback_target > 0)
                progress = (float)sampler_fallback_blocks / (float)sampler_fallback_target;
            if (progress > 1.0f) progress = 1.0f;
            int fill_w = (int)((prog_w - 2) * progress);
            if (fill_w > 0)
                overlay_fill_rect(buf, prog_x + 1, prog_y + 1, fill_w, prog_h - 2, 1);
        }

        /* Row 6: VU meter */
        int vu_w = 120;
        int vu_x = 4;
        int vu_y = 48;
        int vu_h = 5;
        overlay_fill_rect(buf, vu_x, vu_y, vu_w, 1, 1);
        overlay_fill_rect(buf, vu_x, vu_y + vu_h - 1, vu_w, 1, 1);
        overlay_fill_rect(buf, vu_x, vu_y, 1, vu_h, 1);
        overlay_fill_rect(buf, vu_x + vu_w - 1, vu_y, 1, vu_h, 1);
        /* Log scale: map -48dB..0dB to 0..bar width */
        float vu_norm = 0.0f;
        if (sampler_vu_peak > 0) {
            float db = 20.0f * log10f((float)sampler_vu_peak / 32767.0f);
            vu_norm = (db + 48.0f) / 48.0f;  /* -48dB=0, 0dB=1 */
            if (vu_norm < 0.0f) vu_norm = 0.0f;
            if (vu_norm > 1.0f) vu_norm = 1.0f;
        }
        int fill_w = (int)(vu_norm * (vu_w - 2));
        if (fill_w > vu_w - 2) fill_w = vu_w - 2;
        if (fill_w > 0)
            overlay_fill_rect(buf, vu_x + 1, vu_y + 1, fill_w, vu_h - 2, 1);

        /* Row 7: Instructions */
        overlay_draw_string(buf, 0, 56, "Sample to stop", 1);

    } else {
        /* IDLE with fullscreen still showing = just finished */
        overlay_draw_string(buf, 16, 24, "Sample saved!", 1);
    }
}

/* Sync overlay state to shared memory for JS rendering */
static void shadow_overlay_sync(void) {
    if (!shadow_overlay_shm) return;

    /* Determine overlay type (priority: sampler > skipback > shift+knob) */
    if (sampler_fullscreen_active &&
        (sampler_state != SAMPLER_IDLE || sampler_overlay_timeout > 0)) {
        shadow_overlay_shm->overlay_type = SHADOW_OVERLAY_SAMPLER;
    } else if (skipback_overlay_timeout > 0) {
        shadow_overlay_shm->overlay_type = SHADOW_OVERLAY_SKIPBACK;
    } else if (shift_knob_overlay_active && shift_knob_overlay_timeout > 0) {
        shadow_overlay_shm->overlay_type = SHADOW_OVERLAY_SHIFT_KNOB;
    } else {
        shadow_overlay_shm->overlay_type = SHADOW_OVERLAY_NONE;
    }

    /* Sampler state */
    shadow_overlay_shm->sampler_state = (uint8_t)sampler_state;
    shadow_overlay_shm->sampler_source = (uint8_t)sampler_source;
    shadow_overlay_shm->sampler_cursor = (uint8_t)sampler_menu_cursor;
    shadow_overlay_shm->sampler_fullscreen = sampler_fullscreen_active ? 1 : 0;
    shadow_overlay_shm->sampler_duration_bars = (uint16_t)sampler_duration_options[sampler_duration_index];
    shadow_overlay_shm->sampler_vu_peak = sampler_vu_peak;
    shadow_overlay_shm->sampler_bars_completed = (uint16_t)sampler_bars_completed;
    shadow_overlay_shm->sampler_target_bars = (uint16_t)sampler_duration_options[sampler_duration_index];
    shadow_overlay_shm->sampler_overlay_timeout = (uint16_t)sampler_overlay_timeout;
    shadow_overlay_shm->sampler_samples_written = sampler_samples_written;
    shadow_overlay_shm->sampler_clock_count = (uint32_t)sampler_clock_count;
    shadow_overlay_shm->sampler_target_pulses = (uint32_t)sampler_target_pulses;
    shadow_overlay_shm->sampler_fallback_blocks = (uint32_t)sampler_fallback_blocks;
    shadow_overlay_shm->sampler_fallback_target = (uint32_t)sampler_fallback_target;
    shadow_overlay_shm->sampler_clock_received = sampler_clock_received ? 1 : 0;

    /* Skipback state */
    shadow_overlay_shm->skipback_active = (skipback_overlay_timeout > 0) ? 1 : 0;
    shadow_overlay_shm->skipback_overlay_timeout = (uint16_t)skipback_overlay_timeout;

    /* Shift+knob state */
    shadow_overlay_shm->shift_knob_active = (shift_knob_overlay_active && shift_knob_overlay_timeout > 0) ? 1 : 0;
    shadow_overlay_shm->shift_knob_timeout = (uint16_t)shift_knob_overlay_timeout;
    memcpy((char *)shadow_overlay_shm->shift_knob_patch, shift_knob_overlay_patch, 64);
    memcpy((char *)shadow_overlay_shm->shift_knob_param, shift_knob_overlay_param, 64);
    memcpy((char *)shadow_overlay_shm->shift_knob_value, shift_knob_overlay_value, 32);

    /* Increment sequence to notify JS of state change */
    shadow_overlay_shm->sequence++;
}

/* Load feature configuration from config/features.json */
static void load_feature_config(void)
{
    const char *config_path = "/data/UserData/move-anything/config/features.json";
    FILE *f = fopen(config_path, "r");
    if (!f) {
        /* No config file - use defaults (all enabled) */
        shadow_ui_enabled = true;
        standalone_enabled = true;
        shadow_log("Features: No config file, using defaults (all enabled)");
        return;
    }

    /* Read file */
    char config_buf[512];
    size_t len = fread(config_buf, 1, sizeof(config_buf) - 1, f);
    fclose(f);
    config_buf[len] = '\0';

    /* Parse shadow_ui_enabled */
    const char *shadow_ui_key = strstr(config_buf, "\"shadow_ui_enabled\"");
    if (shadow_ui_key) {
        const char *colon = strchr(shadow_ui_key, ':');
        if (colon) {
            /* Skip whitespace */
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "false", 5) == 0) {
                shadow_ui_enabled = false;
            } else {
                shadow_ui_enabled = true;
            }
        }
    }

    /* Parse standalone_enabled */
    const char *standalone_key = strstr(config_buf, "\"standalone_enabled\"");
    if (standalone_key) {
        const char *colon = strchr(standalone_key, ':');
        if (colon) {
            /* Skip whitespace */
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "false", 5) == 0) {
                standalone_enabled = false;
            } else {
                standalone_enabled = true;
            }
        }
    }

    /* Parse link_audio_enabled (defaults to false) */
    const char *link_audio_key = strstr(config_buf, "\"link_audio_enabled\"");
    if (link_audio_key) {
        const char *colon = strchr(link_audio_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "true", 4) == 0) {
                link_audio.enabled = 1;
            }
        }
    }

    /* Parse display_mirror_enabled (defaults to false) */
    const char *display_mirror_key = strstr(config_buf, "\"display_mirror_enabled\"");
    if (display_mirror_key) {
        const char *colon = strchr(display_mirror_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "true", 4) == 0) {
                display_mirror_enabled = true;
            }
        }
    }

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg),
             "Features: shadow_ui=%s, standalone=%s, link_audio=%s, display_mirror=%s",
             shadow_ui_enabled ? "enabled" : "disabled",
             standalone_enabled ? "enabled" : "disabled",
             link_audio.enabled ? "enabled" : "disabled",
             display_mirror_enabled ? "enabled" : "disabled");
    shadow_log(log_msg);
}

static int shadow_read_global_volume_from_settings(float *linear_out, float *db_out)
{
    FILE *f = fopen("/data/UserData/settings/Settings.json", "r");
    if (!f) return 0;

    /* Read file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 8192) {
        fclose(f);
        return 0;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return 0;
    }

    size_t nread = fread(json, 1, size, f);
    json[nread] = '\0';
    fclose(f);

    /* Find "globalVolume": X.X */
    const char *key = "\"globalVolume\":";
    char *pos = strstr(json, key);
    if (!pos) {
        free(json);
        return 0;
    }

    pos += strlen(key);
    while (*pos == ' ') pos++;

    float db = strtof(pos, NULL);
    float linear = (db <= -60.0f) ? 0.0f : powf(10.0f, db / 20.0f);
    if (linear < 0.0f) linear = 0.0f;
    if (linear > 1.0f) linear = 1.0f;

    if (linear_out) *linear_out = linear;
    if (db_out) *db_out = db;

    free(json);
    return 1;
}

/* Read initial volume from Move's Settings.json */
static void shadow_read_initial_volume(void)
{
    float linear = 1.0f;
    float db = 0.0f;
    if (!shadow_read_global_volume_from_settings(&linear, &db)) {
        shadow_log("Master volume: Settings.json not found, defaulting to 1.0");
        return;
    }

    shadow_master_volume = linear;

    char msg[64];
    snprintf(msg, sizeof(msg), "Master volume: read %.1f dB -> %.3f linear", db, shadow_master_volume);
    shadow_log(msg);
}

/* ==========================================================================
 * Shadow State Persistence - Save/load slot volumes to shadow_chain_config.json
 * ========================================================================== */

#define SHADOW_CONFIG_PATH "/data/UserData/move-anything/shadow_chain_config.json"

static void shadow_save_state(void)
{
    /* Read existing config to preserve fields written by shadow_ui.js */
    FILE *f = fopen(SHADOW_CONFIG_PATH, "r");
    char patches_buf[4096] = "";
    char master_fx[256] = "";
    char master_fx_path[256] = "";
    char master_fx_chain_buf[2048] = "";
    int overlay_knobs_mode = -1;
    int resample_bridge_mode = -1;
    int link_audio_routing_saved = -1;

    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (size > 0 && size < 16384) {
            char *json = malloc(size + 1);
            if (json) {
                size_t nread = fread(json, 1, size, f);
                json[nread] = '\0';

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

                /* Extract master_fx string (legacy single-slot) */
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

                /* Extract master_fx_chain object (written by shadow_ui.js) */
                char *mfc = strstr(json, "\"master_fx_chain\":");
                if (mfc) {
                    char *obj_start = strchr(mfc, '{');
                    if (obj_start) {
                        int depth = 1;
                        char *obj_end = obj_start + 1;
                        while (*obj_end && depth > 0) {
                            if (*obj_end == '{') depth++;
                            else if (*obj_end == '}') depth--;
                            obj_end++;
                        }
                        int len = obj_end - obj_start;
                        if (len < (int)sizeof(master_fx_chain_buf) - 1) {
                            strncpy(master_fx_chain_buf, obj_start, len);
                            master_fx_chain_buf[len] = '\0';
                        }
                    }
                }

                /* Extract overlay_knobs_mode integer */
                char *okm = strstr(json, "\"overlay_knobs_mode\":");
                if (okm) {
                    okm = strchr(okm, ':');
                    if (okm) {
                        okm++;
                        while (*okm == ' ') okm++;
                        overlay_knobs_mode = atoi(okm);
                    }
                }

                /* Extract resample_bridge_mode integer */
                char *rbm = strstr(json, "\"resample_bridge_mode\":");
                if (rbm) {
                    rbm = strchr(rbm, ':');
                    if (rbm) {
                        rbm++;
                        while (*rbm == ' ') rbm++;
                        resample_bridge_mode = atoi(rbm);
                    }
                }

                /* Extract link_audio_routing boolean */
                char *lar = strstr(json, "\"link_audio_routing\":");
                if (lar) {
                    lar = strchr(lar, ':');
                    if (lar) {
                        lar++;
                        while (*lar == ' ') lar++;
                        link_audio_routing_saved = (strncmp(lar, "true", 4) == 0) ? 1 : 0;
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
    if (master_fx_chain_buf[0]) {
        fprintf(f, "  \"master_fx_chain\": %s,\n", master_fx_chain_buf);
    }
    if (overlay_knobs_mode >= 0) {
        fprintf(f, "  \"overlay_knobs_mode\": %d,\n", overlay_knobs_mode);
    }
    if (resample_bridge_mode >= 0) {
        fprintf(f, "  \"resample_bridge_mode\": %d,\n", resample_bridge_mode);
    }
    if (link_audio_routing_saved >= 0) {
        fprintf(f, "  \"link_audio_routing\": %s,\n", link_audio_routing_saved ? "true" : "false");
    }
    fprintf(f, "  \"slot_volumes\": [%.3f, %.3f, %.3f, %.3f],\n",
            shadow_chain_slots[0].volume,
            shadow_chain_slots[1].volume,
            shadow_chain_slots[2].volume,
            shadow_chain_slots[3].volume);
    fprintf(f, "  \"slot_forward_channels\": [%d, %d, %d, %d]\n",
            shadow_chain_slots[0].forward_channel,
            shadow_chain_slots[1].forward_channel,
            shadow_chain_slots[2].forward_channel,
            shadow_chain_slots[3].forward_channel);
    fprintf(f, "}\n");
    fclose(f);

    char msg[256];
    snprintf(msg, sizeof(msg), "Saved slot volumes: [%.2f, %.2f, %.2f, %.2f] fwd: [%d, %d, %d, %d]",
             shadow_chain_slots[0].volume,
             shadow_chain_slots[1].volume,
             shadow_chain_slots[2].volume,
             shadow_chain_slots[3].volume,
             shadow_chain_slots[0].forward_channel,
             shadow_chain_slots[1].forward_channel,
             shadow_chain_slots[2].forward_channel,
             shadow_chain_slots[3].forward_channel);
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

    size_t nread = fread(json, 1, size, f);
    json[nread] = '\0';
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

    /* Parse slot_forward_channels array */
    const char *fwd_key = "\"slot_forward_channels\":";
    char *fwd_pos = strstr(json, fwd_key);
    if (fwd_pos) {
        fwd_pos = strchr(fwd_pos, '[');
        if (fwd_pos) {
            int f0, f1, f2, f3;
            if (sscanf(fwd_pos, "[%d, %d, %d, %d]", &f0, &f1, &f2, &f3) == 4) {
                shadow_chain_slots[0].forward_channel = f0;
                shadow_chain_slots[1].forward_channel = f1;
                shadow_chain_slots[2].forward_channel = f2;
                shadow_chain_slots[3].forward_channel = f3;

                char msg[128];
                snprintf(msg, sizeof(msg), "Loaded slot fwd channels: [%d, %d, %d, %d]",
                         f0, f1, f2, f3);
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
    /* Config uses 1-based MIDI channels; convert to 0-based for status nibble.
     * 0 = all channels (stored as -1 internally). */
    if (ch == 0) {
        return -1;  /* All channels */
    }
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
    /* Write to unified log */
    unified_log("shim", LOG_LEVEL_DEBUG, "%s", msg ? msg : "(null)");
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
        shadow_chain_slots[i].pre_mute_volume = 0.0f;
        shadow_chain_slots[i].muted = 0;
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
    int ch = shadow_chain_slots[slot].channel;
    shadow_ui_state->slot_channels[slot] = (ch < 0) ? 0 : (uint8_t)(ch + 1);
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

    size_t nread = fread(json, 1, size, f);
    json[nread] = '\0';
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
                if (ch >= 0 && ch <= 16) {
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

        /* Parse forward_channel (-2 = passthrough, -1 = auto, 1-16 = channel) */
        char *fwd_pos = strstr(name_pos, "\"forward_channel\"");
        if (fwd_pos) {
            char *fwd_colon = strchr(fwd_pos, ':');
            if (fwd_colon) {
                int ch = atoi(fwd_colon + 1);
                if (ch >= -2 && ch <= 16) {
                    shadow_chain_slots[i].forward_channel = (ch > 0) ? ch - 1 : ch;
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
    s->on_midi = NULL;
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
static int shadow_master_fx_slot_load_with_config(int slot, const char *dsp_path, const char *config_json);

static int shadow_master_fx_slot_load(int slot, const char *dsp_path) {
    return shadow_master_fx_slot_load_with_config(slot, dsp_path, NULL);
}

static int shadow_master_fx_slot_load_with_config(int slot, const char *dsp_path, const char *config_json) {
    if (slot < 0 || slot >= MASTER_FX_SLOTS) return -1;
    master_fx_slot_t *s = &shadow_master_fx_slots[slot];

    if (!dsp_path || !dsp_path[0]) {
        shadow_master_fx_slot_unload(slot);
        return 0;  /* Empty = disable this slot */
    }

    /* Already loaded? (skip check if config_json provided - need fresh instance) */
    if (!config_json && strcmp(s->module_path, dsp_path) == 0 && s->instance) {
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

    s->instance = s->api->create_instance(module_dir, config_json);
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
                size_t nread = fread(json, 1, size, f);
                json[nread] = '\0';
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

    /* Check for optional MIDI handler (e.g. ducker) */
    {
        typedef void (*fx_on_midi_fn)(void *, const uint8_t *, int, int);
        s->on_midi = (fx_on_midi_fn)dlsym(s->handle, "move_audio_fx_on_midi");
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

/* Forward MIDI to master FX slots that have on_midi (e.g. ducker) */
static void shadow_master_fx_forward_midi(const uint8_t *msg, int len, int source) {
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        master_fx_slot_t *s = &shadow_master_fx_slots[i];
        if (s->on_midi && s->instance) {
            s->on_midi(s->instance, msg, len, source);
        }
    }
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

    /* Look up optional chain exports for Link Audio routing + same-frame FX */
    shadow_chain_set_inject_audio = (void (*)(void *, int16_t *, int))
        dlsym(shadow_dsp_handle, "chain_set_inject_audio");
    shadow_chain_set_external_fx_mode = (void (*)(void *, int))
        dlsym(shadow_dsp_handle, "chain_set_external_fx_mode");
    shadow_chain_process_fx = (void (*)(void *, int16_t *, int))
        dlsym(shadow_dsp_handle, "chain_process_fx");

    unified_log("shim", LOG_LEVEL_INFO, "chain dlsym: inject=%p ext_fx_mode=%p process_fx=%p same_frame=%d",
            (void*)shadow_chain_set_inject_audio,
            (void*)shadow_chain_set_external_fx_mode,
            (void*)shadow_chain_process_fx,
            (shadow_chain_set_external_fx_mode && shadow_chain_process_fx) ? 1 : 0);

    /* Run batch migration if this is the first boot with per-set state support.
     * Copies default slot_state/ to set_state/<UUID>/ for all existing sets. */
    shadow_batch_migrate_sets();

    /* Determine boot state directory: per-set if active_set.txt exists, else default */
    char boot_state_dir[512];
    snprintf(boot_state_dir, sizeof(boot_state_dir), "%s", SLOT_STATE_DIR);
    {
        FILE *asf = fopen(ACTIVE_SET_PATH, "r");
        if (asf) {
            char boot_uuid[128] = "";
            if (fgets(boot_uuid, sizeof(boot_uuid), asf)) {
                /* Trim whitespace */
                char *end = boot_uuid + strlen(boot_uuid) - 1;
                while (end > boot_uuid && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
                if (boot_uuid[0]) {
                    char set_dir[512];
                    snprintf(set_dir, sizeof(set_dir), SET_STATE_DIR "/%s", boot_uuid);
                    /* Only adopt per-set dir if it has actual state files */
                    char test_slot[768];
                    snprintf(test_slot, sizeof(test_slot), "%s/slot_0.json", set_dir);
                    char test_cfg[768];
                    snprintf(test_cfg, sizeof(test_cfg), "%s/shadow_chain_config.json", set_dir);
                    struct stat st;
                    if (stat(test_slot, &st) == 0 || stat(test_cfg, &st) == 0) {
                        snprintf(boot_state_dir, sizeof(boot_state_dir), "%s", set_dir);
                        /* Store UUID + name so set-change detection doesn't re-trigger */
                        snprintf(sampler_current_set_uuid, sizeof(sampler_current_set_uuid),
                                 "%s", boot_uuid);
                        /* Read set name from line 2 */
                        char boot_name[128] = "";
                        if (fgets(boot_name, sizeof(boot_name), asf)) {
                            char *ne = boot_name + strlen(boot_name) - 1;
                            while (ne > boot_name && (*ne == '\n' || *ne == '\r' || *ne == ' ')) *ne-- = '\0';
                            if (boot_name[0]) {
                                snprintf(sampler_current_set_name, sizeof(sampler_current_set_name),
                                         "%s", boot_name);
                            }
                        }
                        char m[256];
                        snprintf(m, sizeof(m), "Boot: using per-set state dir %s", set_dir);
                        shadow_log(m);
                    }
                }
            }
            fclose(asf);
        }
    }

    shadow_chain_load_config();
    /* If per-set config was loaded, it overrides the global config values */
    if (strcmp(boot_state_dir, SLOT_STATE_DIR) != 0) {
        shadow_load_config_from_dir(boot_state_dir);
    }

    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        shadow_chain_slots[i].instance = shadow_plugin_v2->create_instance(
            SHADOW_CHAIN_MODULE_DIR, NULL);
        if (!shadow_chain_slots[i].instance) {
            continue;
        }

        /* Check for autosave file first (preserves unsaved work across reboots) */
        char autosave_path[256];
        snprintf(autosave_path, sizeof(autosave_path),
                 "%s/slot_%d.json", boot_state_dir, i);
        FILE *af = fopen(autosave_path, "r");
        if (af) {
            /* Check if autosave has content (not just "{}") */
            fseek(af, 0, SEEK_END);
            long asize = ftell(af);
            fclose(af);
            if (asize > 10) {  /* More than just "{}\n" */
                shadow_plugin_v2->set_param(
                    shadow_chain_slots[i].instance, "load_file", autosave_path);
                shadow_chain_slots[i].active = 1;
                shadow_chain_slots[i].patch_index = -1;
                /* Query channel settings from loaded autosave */
                if (shadow_chain_slots[i].forward_channel == -1 && shadow_plugin_v2->get_param) {
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
                if (shadow_plugin_v2->get_param) {
                    char ch_buf[16];
                    int len;
                    len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                        "patch:receive_channel", ch_buf, sizeof(ch_buf));
                    if (len > 0) {
                        ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
                        int recv_ch = atoi(ch_buf);
                        if (recv_ch != 0) {
                            shadow_chain_slots[i].channel = (recv_ch >= 1 && recv_ch <= 16) ? recv_ch - 1 : -1;
                        }
                    }
                    len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                        "patch:forward_channel", ch_buf, sizeof(ch_buf));
                    if (len > 0) {
                        ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
                        int fwd_ch = atoi(ch_buf);
                        if (fwd_ch != 0) {
                            shadow_chain_slots[i].forward_channel = (fwd_ch > 0) ? fwd_ch - 1 : fwd_ch;
                        }
                    }
                }
                {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Shadow inprocess: slot %d loaded from autosave", i);
                    shadow_log(msg);
                }
                continue;
            }
        }

        /* Fall back to name-based lookup from config */
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
            /* Query synth's default forward channel after patch load.
             * Only apply if slot is still at Auto (-1); preserve explicit user settings. */
            if (shadow_chain_slots[i].forward_channel == -1 && shadow_plugin_v2->get_param) {
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
            /* Apply channel settings saved in the patch preset (overrides config/defaults).
             * 0 = not saved in preset, keep config values. */
            if (shadow_plugin_v2->get_param) {
                char ch_buf[16];
                int len;
                len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                    "patch:receive_channel", ch_buf, sizeof(ch_buf));
                if (len > 0) {
                    ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
                    int recv_ch = atoi(ch_buf);
                    if (recv_ch != 0) {
                        shadow_chain_slots[i].channel = (recv_ch >= 1 && recv_ch <= 16) ? recv_ch - 1 : -1;
                    }
                }
                len = shadow_plugin_v2->get_param(shadow_chain_slots[i].instance,
                    "patch:forward_channel", ch_buf, sizeof(ch_buf));
                if (len > 0) {
                    ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
                    int fwd_ch = atoi(ch_buf);
                    if (fwd_ch != 0) {
                        shadow_chain_slots[i].forward_channel = (fwd_ch > 0) ? fwd_ch - 1 : fwd_ch;
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

    /* Load master FX slots from state files (written by shadow_ui.js autosave) */
    for (int mfx = 0; mfx < MASTER_FX_SLOTS; mfx++) {
        char mfx_path[256];
        snprintf(mfx_path, sizeof(mfx_path), "%s/master_fx_%d.json", boot_state_dir, mfx);
        FILE *mf = fopen(mfx_path, "r");
        if (!mf) continue;

        fseek(mf, 0, SEEK_END);
        long msize = ftell(mf);
        fseek(mf, 0, SEEK_SET);

        if (msize <= 10) {  /* Empty marker "{}\n" */
            fclose(mf);
            continue;
        }

        char *mjson = malloc(msize + 1);
        if (!mjson) { fclose(mf); continue; }
        size_t mnread = fread(mjson, 1, msize, mf);
        mjson[mnread] = '\0';
        fclose(mf);

        /* Extract module_path */
        char dsp_path[256] = "";
        {
            char *mp = strstr(mjson, "\"module_path\":");
            if (mp) {
                mp = strchr(mp, ':');
                if (mp) {
                    mp++;
                    while (*mp == ' ' || *mp == '"') mp++;
                    char *end = mp;
                    while (*end && *end != '"') end++;
                    int len = end - mp;
                    if (len > 0 && len < (int)sizeof(dsp_path) - 1) {
                        strncpy(dsp_path, mp, len);
                        dsp_path[len] = '\0';
                    }
                }
            }
        }

        if (!dsp_path[0]) {
            free(mjson);
            continue;
        }

        /* Extract plugin_id from params BEFORE loading module.
         * Pass it as config_json to create_instance so the CLAP host
         * starts with the correct sub-plugin immediately (no default→switch). */
        char config_json_buf[512] = "";
        char *params_start = strstr(mjson, "\"params\":");
        if (params_start) {
            char *pid_key = strstr(params_start, "\"plugin_id\"");
            if (pid_key) {
                char *pc = strchr(pid_key + 11, ':');
                if (pc) {
                    pc++;
                    while (*pc == ' ') pc++;
                    if (*pc == '"') {
                        pc++;
                        char *pe = strchr(pc, '"');
                        if (pe) {
                            int plen = pe - pc;
                            if (plen > 0 && plen < 256) {
                                char pid_val[256];
                                strncpy(pid_val, pc, plen);
                                pid_val[plen] = '\0';
                                snprintf(config_json_buf, sizeof(config_json_buf),
                                         "{\"plugin_id\":\"%s\"}", pid_val);
                            }
                        }
                    }
                }
            }
        }

        /* Load the module (with plugin_id config if available) */
        int load_result = shadow_master_fx_slot_load_with_config(mfx, dsp_path,
            config_json_buf[0] ? config_json_buf : NULL);
        if (load_result != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "MFX boot: slot %d failed to load %s", mfx, dsp_path);
            shadow_log(msg);
            free(mjson);
            continue;
        }

        master_fx_slot_t *s = &shadow_master_fx_slots[mfx];

        /* Restore state if available */
        char *state_start = strstr(mjson, "\"state\":");
        if (state_start && s->api && s->instance && s->api->set_param) {
            char *obj_start = strchr(state_start, '{');
            if (obj_start) {
                int depth = 1;
                char *obj_end = obj_start + 1;
                while (*obj_end && depth > 0) {
                    if (*obj_end == '{') depth++;
                    else if (*obj_end == '}') depth--;
                    obj_end++;
                }
                int slen = obj_end - obj_start;
                char *state_buf = malloc(slen + 1);
                if (state_buf) {
                    memcpy(state_buf, obj_start, slen);
                    state_buf[slen] = '\0';
                    s->api->set_param(s->instance, "state", state_buf);
                    free(state_buf);
                }
            }
        } else if (params_start && s->api && s->instance && s->api->set_param) {
            /* Fall back to individual params */
            char *obj_start = strchr(params_start, '{');
            if (obj_start) {
                int depth = 1;
                char *obj_end = obj_start + 1;
                while (*obj_end && depth > 0) {
                    if (*obj_end == '{') depth++;
                    else if (*obj_end == '}') depth--;
                    obj_end++;
                }
                /* Parse individual key:value pairs from the params object */
                char *p = obj_start + 1;
                while (p < obj_end - 1) {
                    /* Find key */
                    char *kstart = strchr(p, '"');
                    if (!kstart || kstart >= obj_end) break;
                    kstart++;
                    char *kend = strchr(kstart, '"');
                    if (!kend || kend >= obj_end) break;

                    char param_key[128];
                    int klen = kend - kstart;
                    if (klen >= (int)sizeof(param_key)) { p = kend + 1; continue; }
                    strncpy(param_key, kstart, klen);
                    param_key[klen] = '\0';

                    /* Find value (after colon) */
                    char *colon = strchr(kend, ':');
                    if (!colon || colon >= obj_end) break;
                    colon++;
                    while (*colon == ' ') colon++;

                    char param_val[256];
                    if (*colon == '"') {
                        /* String value */
                        colon++;
                        char *vend = strchr(colon, '"');
                        if (!vend || vend >= obj_end) break;
                        int vlen = vend - colon;
                        if (vlen >= (int)sizeof(param_val)) { p = vend + 1; continue; }
                        strncpy(param_val, colon, vlen);
                        param_val[vlen] = '\0';
                        p = vend + 1;
                    } else {
                        /* Numeric value */
                        char *vend = colon;
                        while (*vend && *vend != ',' && *vend != '}' && *vend != '\n') vend++;
                        int vlen = vend - colon;
                        if (vlen >= (int)sizeof(param_val)) { p = vend; continue; }
                        strncpy(param_val, colon, vlen);
                        param_val[vlen] = '\0';
                        /* Trim trailing whitespace */
                        while (vlen > 0 && (param_val[vlen-1] == ' ' || param_val[vlen-1] == '\r')) {
                            param_val[--vlen] = '\0';
                        }
                        p = vend;
                    }

                    /* Skip plugin_id - already applied via config_json */
                    if (strcmp(param_key, "plugin_id") != 0) {
                        s->api->set_param(s->instance, param_key, param_val);
                    }
                }
            }
        }

        {
            char msg[256];
            snprintf(msg, sizeof(msg), "MFX boot: slot %d loaded %s%s",
                     mfx, s->module_id,
                     state_start ? " (with state)" : (strstr(mjson, "\"params\":") ? " (with params)" : ""));
            shadow_log(msg);
        }
        free(mjson);
    }

    shadow_ui_state_refresh();

    /* Pre-create directories so mkdir fork() doesn't glitch audio later */
    {
        struct stat st;
        if (stat(SAMPLER_RECORDINGS_DIR, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", SAMPLER_RECORDINGS_DIR, NULL };
            shim_run_command(mkdir_argv);
        }
        if (stat(SKIPBACK_DIR, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", SKIPBACK_DIR, NULL };
            shim_run_command(mkdir_argv);
        }
        if (stat(SLOT_STATE_DIR, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", SLOT_STATE_DIR, NULL };
            shim_run_command(mkdir_argv);
        }
        if (stat(SET_STATE_DIR, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", SET_STATE_DIR, NULL };
            shim_run_command(mkdir_argv);
        }
    }

    shadow_inprocess_ready = 1;
    /* Start countdown for delayed mod wheel reset after Move's startup MIDI settles */
    shadow_startup_modwheel_countdown = STARTUP_MODWHEEL_RESET_FRAMES;
    if (shadow_control) {
        /* Allow display hotkey when running in-process DSP. */
        shadow_control->shadow_ready = 1;
    }
    /* Launch shadow UI only if enabled */
    if (shadow_ui_enabled) {
        launch_shadow_ui();
    }
    shadow_log("Shadow inprocess: chain loaded");
    return 0;
}

static int shadow_chain_slot_for_channel(int ch) {
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        if (shadow_chain_slots[i].channel != ch && shadow_chain_slots[i].channel != -1) continue;
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
    if (fwd_ch == -2) {
        /* Passthrough: preserve original MIDI channel */
        return status;
    }
    if (fwd_ch >= 0 && fwd_ch <= 15) {
        /* Specific forward channel */
        return (status & 0xF0) | (uint8_t)fwd_ch;
    }
    /* Auto (-1): use the receive channel, but if recv=All (-1), passthrough */
    if (shadow_chain_slots[slot].channel < 0) {
        return status;  /* Recv=All + Fwd=Auto → passthrough */
    }
    return (status & 0xF0) | (uint8_t)shadow_chain_slots[slot].channel;
}

/* Dispatch MIDI to all matching slots (supports recv=All broadcasting) */
static void shadow_chain_dispatch_midi_to_slots(const uint8_t *pkt, int log_on, int *midi_log_count) {
    uint8_t status_usb = pkt[1];
    uint8_t type = status_usb & 0xF0;
    uint8_t midi_ch = status_usb & 0x0F;
    uint8_t note = pkt[2];
    int dispatched = 0;

    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        /* Check channel match: slot receives this channel, or slot is set to All (-1) */
        if (shadow_chain_slots[i].channel != (int)midi_ch && shadow_chain_slots[i].channel != -1)
            continue;

        /* Lazy activation check */
        if (!shadow_chain_slots[i].active) {
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
                    }
                }
            }
            if (!shadow_chain_slots[i].active) continue;
        }

        /* Wake slot from idle on any MIDI dispatch */
        if (shadow_slot_idle[i] || shadow_slot_fx_idle[i]) {
            shadow_slot_idle[i] = 0;
            shadow_slot_silence_frames[i] = 0;
            shadow_slot_fx_idle[i] = 0;
            shadow_slot_fx_silence_frames[i] = 0;
        }

        /* Send MIDI to this slot */
        if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
            uint8_t msg[3] = { shadow_chain_remap_channel(i, pkt[1]), pkt[2], pkt[3] };
            shadow_plugin_v2->on_midi(shadow_chain_slots[i].instance, msg, 3,
                                      MOVE_MIDI_SOURCE_EXTERNAL);
        }
        dispatched++;
    }

    /* Broadcast MIDI to ALL active slots for audio FX (e.g. ducker).
     * FX_BROADCAST only forwards to audio FX, not synth/MIDI FX, so this
     * is safe even for slots that already received normal MIDI dispatch. */
    if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
        for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
            if (!shadow_chain_slots[i].active || !shadow_chain_slots[i].instance)
                continue;
            uint8_t msg[3] = { pkt[1], pkt[2], pkt[3] };
            shadow_plugin_v2->on_midi(shadow_chain_slots[i].instance, msg, 3,
                                      MOVE_MIDI_SOURCE_FX_BROADCAST);
        }
    }

    /* Forward MIDI to master FX (e.g. ducker) regardless of slot routing */
    {
        uint8_t msg[3] = { pkt[1], pkt[2], pkt[3] };
        shadow_master_fx_forward_midi(msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    }

    if (log_on && type == 0x90 && pkt[3] > 0 && *midi_log_count < 100) {
        char dbg[256];
        snprintf(dbg, sizeof(dbg),
            "midi_out: note=%u vel=%u ch=%u dispatched=%d",
            note, pkt[3], midi_ch, dispatched);
        shadow_log(dbg);
        shadow_midi_out_logf("midi_out: note=%u vel=%u ch=%u dispatched=%d",
            note, pkt[3], midi_ch, dispatched);
        (*midi_log_count)++;
    }
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
    
    size_t nread = fread(json, 1, size, f);
    json[nread] = '\0';
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

    /* Apply channel settings saved in the patch preset.
     * 0 = not saved in preset, keep current values. */
    if (shadow_plugin_v2->get_param) {
        char ch_buf[16];
        int len;
        len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
            "patch:receive_channel", ch_buf, sizeof(ch_buf));
        if (len > 0) {
            ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
            int recv_ch = atoi(ch_buf);
            if (recv_ch != 0) {
                shadow_chain_slots[slot].channel = (recv_ch >= 1 && recv_ch <= 16) ? recv_ch - 1 : -1;
            }
        }
        len = shadow_plugin_v2->get_param(shadow_chain_slots[slot].instance,
            "patch:forward_channel", ch_buf, sizeof(ch_buf));
        if (len > 0) {
            ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
            int fwd_ch = atoi(ch_buf);
            if (fwd_ch != 0) {
                shadow_chain_slots[slot].forward_channel = (fwd_ch > 0) ? fwd_ch - 1 : fwd_ch;
            }
        }
    }

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
        if (ch < -2) ch = -2;
        if (ch > 15) ch = 15;
        shadow_chain_slots[slot].forward_channel = ch;
        shadow_ui_state_update_slot(slot);
        return 1;
    }
    if (strcmp(key, "slot:receive_channel") == 0) {
        int ch = atoi(value);
        if (ch == 0) {
            shadow_chain_slots[slot].channel = -1;  /* All channels */
            shadow_ui_state_update_slot(slot);
        } else if (ch >= 1 && ch <= 16) {
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
        int ch = shadow_chain_slots[slot].channel;
        return snprintf(buf, buf_len, "%d", (ch < 0) ? 0 : ch + 1);  /* 0=All, 1-16=specific */
    }
    return -1;  /* Not a slot param */
}

static int shadow_param_publish_response(uint32_t req_id) {
    if (!shadow_param) return 0;
    if (shadow_param->request_id != req_id) {
        return 0;
    }
    shadow_param->response_id = req_id;
    shadow_param->response_ready = 1;
    shadow_param->request_type = 0;
    return 1;
}

static void shadow_inprocess_handle_param_request(void) {
    if (!shadow_param) return;

    uint8_t req_type = shadow_param->request_type;
    if (req_type == 0) return;  /* No pending request */
    uint32_t req_id = shadow_param->request_id;

    /* Handle master FX chain params: master_fx:fx1:module, master_fx:fx2:wet, etc. */
    if (strncmp(shadow_param->key, "master_fx:", 10) == 0) {
        const char *fx_key = shadow_param->key + 10;
        int mfx_slot = -1;  /* -1 = legacy (slot 0), 0-3 = specific slot */
        int has_slot_prefix = 0;
        const char *param_key = fx_key;

        /* Parse slot: fx1:, fx2:, fx3:, fx4: */
        if (strncmp(fx_key, "fx1:", 4) == 0) { mfx_slot = 0; param_key = fx_key + 4; has_slot_prefix = 1; }
        else if (strncmp(fx_key, "fx2:", 4) == 0) { mfx_slot = 1; param_key = fx_key + 4; has_slot_prefix = 1; }
        else if (strncmp(fx_key, "fx3:", 4) == 0) { mfx_slot = 2; param_key = fx_key + 4; has_slot_prefix = 1; }
        else if (strncmp(fx_key, "fx4:", 4) == 0) { mfx_slot = 3; param_key = fx_key + 4; has_slot_prefix = 1; }
        else { mfx_slot = 0; param_key = fx_key; }  /* Legacy: default to slot 0 */

        master_fx_slot_t *mfx = &shadow_master_fx_slots[mfx_slot];

        if (req_type == 1) {  /* SET */
            if (!has_slot_prefix && strcmp(param_key, "resample_bridge") == 0) {
                native_resample_bridge_mode_t new_mode =
                    native_resample_bridge_mode_from_text(shadow_param->value);
                if (new_mode != native_resample_bridge_mode) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Native resample bridge mode: %s",
                             native_resample_bridge_mode_name(new_mode));
                    shadow_log(msg);
                }
                native_resample_bridge_mode = new_mode;
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (!has_slot_prefix && strcmp(param_key, "link_audio_routing") == 0) {
                int val = atoi(shadow_param->value);
                link_audio_routing_enabled = val ? 1 : 0;
                {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Link Audio routing: %s",
                             link_audio_routing_enabled ? "ON" : "OFF");
                    shadow_log(msg);
                }
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (strcmp(param_key, "module") == 0) {
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
            if (!has_slot_prefix && strcmp(param_key, "resample_bridge") == 0) {
                int mode = (int)native_resample_bridge_mode;
                if (mode < 0 || mode > 2) mode = 0;
                shadow_param->result_len = snprintf(shadow_param->value, SHADOW_PARAM_VALUE_LEN, "%d", mode);
                shadow_param->error = 0;
            } else if (!has_slot_prefix && strcmp(param_key, "link_audio_routing") == 0) {
                shadow_param->result_len = snprintf(shadow_param->value, SHADOW_PARAM_VALUE_LEN, "%d",
                                                    link_audio_routing_enabled);
                shadow_param->error = 0;
            } else if (strcmp(param_key, "module") == 0) {
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
                        shadow_param_publish_response(req_id);
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
                        shadow_param_publish_response(req_id);
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
                /* Try module's get_param first (for dynamic hierarchies like CLAP FX) */
                if (mfx->api && mfx->instance && mfx->api->get_param) {
                    int len = mfx->api->get_param(mfx->instance, "ui_hierarchy",
                                                   shadow_param->value, SHADOW_PARAM_VALUE_LEN);
                    if (len > 2) {
                        shadow_param->error = 0;
                        shadow_param->result_len = len;
                        shadow_param_publish_response(req_id);
                        return;
                    }
                }
                /* Fall back to reading ui_hierarchy from module.json */
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
                            size_t nread = fread(json, 1, size, f);
                            json[nread] = '\0';

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
                                        shadow_param_publish_response(req_id);
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
        shadow_param_publish_response(req_id);
        return;
    }

    /* Handle overtake DSP params: overtake_dsp:load, overtake_dsp:unload, overtake_dsp:<param> */
    if (strncmp(shadow_param->key, "overtake_dsp:", 13) == 0) {
        const char *param_key = shadow_param->key + 13;
        if (req_type == 1) {  /* SET */
            if (strcmp(param_key, "load") == 0) {
                shadow_overtake_dsp_load(shadow_param->value);
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (strcmp(param_key, "unload") == 0) {
                shadow_overtake_dsp_unload();
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (overtake_dsp_gen && overtake_dsp_gen_inst && overtake_dsp_gen->set_param) {
                overtake_dsp_gen->set_param(overtake_dsp_gen_inst, param_key, shadow_param->value);
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->set_param) {
                overtake_dsp_fx->set_param(overtake_dsp_fx_inst, param_key, shadow_param->value);
                shadow_param->error = 0;
                shadow_param->result_len = 0;
            } else {
                shadow_param->error = 13;
                shadow_param->result_len = -1;
            }
        } else if (req_type == 2) {  /* GET */
            int len = -1;
            if (overtake_dsp_gen && overtake_dsp_gen_inst && overtake_dsp_gen->get_param) {
                len = overtake_dsp_gen->get_param(overtake_dsp_gen_inst, param_key,
                                                   shadow_param->value, SHADOW_PARAM_VALUE_LEN);
            } else if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->get_param) {
                len = overtake_dsp_fx->get_param(overtake_dsp_fx_inst, param_key,
                                                  shadow_param->value, SHADOW_PARAM_VALUE_LEN);
            }
            if (len >= 0) {
                shadow_param->error = 0;
                shadow_param->result_len = len;
            } else {
                shadow_param->error = 14;
                shadow_param->result_len = -1;
            }
        }
        shadow_param_publish_response(req_id);
        return;
    }

    int slot = shadow_param->slot;
    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) {
        shadow_param->error = 1;
        shadow_param->result_len = -1;
        shadow_param_publish_response(req_id);
        return;
    }

    /* Handle slot-level params first */
    if (req_type == 1) {  /* SET param */
        if (shadow_handle_slot_param_set(slot, shadow_param->key, shadow_param->value)) {
            shadow_param->error = 0;
            shadow_param->result_len = 0;
            shadow_param_publish_response(req_id);
            return;
        }
    }
    else if (req_type == 2) {  /* GET param */
        int len = shadow_handle_slot_param_get(slot, shadow_param->key,
                                                shadow_param->value, SHADOW_PARAM_VALUE_LEN);
        if (len >= 0) {
            shadow_param->error = 0;
            shadow_param->result_len = len;
            shadow_param_publish_response(req_id);
            return;
        }
    }

    /* Not a slot param - forward to plugin */
    if (!shadow_plugin_v2 || !shadow_chain_slots[slot].instance) {
        shadow_param->error = 2;
        shadow_param->result_len = -1;
        shadow_param_publish_response(req_id);
        return;
    }

    if (req_type == 1) {  /* SET param */
        if (shadow_plugin_v2->set_param) {
            /* Make local copies - shared memory may be modified during set_param.
             * value_copy is static because SHADOW_PARAM_VALUE_LEN (64KB) is too large
             * for the stack. Safe because this runs in single-threaded ioctl context. */
            char key_copy[SHADOW_PARAM_KEY_LEN];
            static char value_copy[SHADOW_PARAM_VALUE_LEN];
            strncpy(key_copy, shadow_param->key, sizeof(key_copy) - 1);
            key_copy[sizeof(key_copy) - 1] = '\0';
            strncpy(value_copy, shadow_param->value, sizeof(value_copy) - 1);
            value_copy[sizeof(value_copy) - 1] = '\0';

            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance,
                                        key_copy, value_copy);
            shadow_param->error = 0;
            shadow_param->result_len = 0;

            /* Activate slot when synth module is loaded.
             * Don't deactivate when synth is removed — the slot may still
             * have FX that need to process inject audio (Link Audio).
             * Deactivation only happens when the whole patch is cleared. */
            if (strcmp(key_copy, "synth:module") == 0) {
                if (value_copy[0] != '\0') {
                    shadow_chain_slots[slot].active = 1;

                    /* Query synth's default forward channel and apply if slot is still at Auto.
                     * Only apply if slot is still at Auto (-1); preserve explicit user settings. */
                    if (shadow_chain_slots[slot].forward_channel == -1 && shadow_plugin_v2->get_param) {
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
                }
                /* synth cleared: slot stays active for FX processing */
            }
            /* Also activate when FX modules are loaded on an inactive slot */
            if (!shadow_chain_slots[slot].active &&
                (strcmp(key_copy, "fx1:module") == 0 ||
                 strcmp(key_copy, "fx2:module") == 0) &&
                value_copy[0] != '\0') {
                shadow_chain_slots[slot].active = 1;
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

                    /* Query synth's default forward channel after patch load.
                     * Only apply if slot is still at Auto (-1); preserve explicit user settings. */
                    if (shadow_chain_slots[slot].forward_channel == -1 && shadow_plugin_v2->get_param) {
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

    shadow_param_publish_response(req_id);
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
            /* Sampler sees clock from cable 0 only (Move internal) to avoid double-counting */
            if (cable == 0) {
                sampler_on_clock(status_usb);
            }

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

            /* Validate data bytes (MIDI data bytes must be 0-127, high bit clear) */
            if ((pkt[2] & 0x80) || (pkt[3] & 0x80)) {
                continue;  /* Invalid data bytes - skip garbage packet */
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
            shadow_chain_dispatch_midi_to_slots(pkt, log_on, &midi_log_count);

            /* Also route to overtake DSP if loaded */
            if (overtake_dsp_gen && overtake_dsp_gen_inst && overtake_dsp_gen->on_midi) {
                uint8_t msg[3] = { pkt[1], pkt[2], pkt[3] };
                overtake_dsp_gen->on_midi(overtake_dsp_gen_inst, msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            } else if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->on_midi) {
                uint8_t msg[3] = { pkt[1], pkt[2], pkt[3] };
                overtake_dsp_fx->on_midi(overtake_dsp_fx_inst, msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            }
        }
    }
}

static void shadow_inprocess_mix_audio(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    float mv = shadow_master_volume;
    int mfx_active = shadow_master_fx_chain_active();

    /* When MFX is active, build the mix at unity level so FX see a consistent
     * signal regardless of master volume.  Apply mv AFTER MFX instead.
     * When MFX is off, pre-scale ME by master volume (current behavior). */
    float me_input_scale;
    float move_prescale;
    float link_sub_scale;
    if (mfx_active) {
        me_input_scale = 1.0f;
        link_sub_scale = 1.0f;
        if (mv > 0.001f) {
            move_prescale = 1.0f / mv;
            if (move_prescale > 20.0f) move_prescale = 20.0f;
        } else {
            move_prescale = 1.0f;
        }
    } else {
        me_input_scale = (mv < 1.0f) ? mv : 1.0f;
        move_prescale = 1.0f;
        link_sub_scale = mv;
    }

    /* Save Move's audio for bridge split component (before mixing ME). */
    memcpy(native_bridge_move_component, mailbox_audio, sizeof(native_bridge_move_component));

    int32_t mix[FRAMES_PER_BLOCK * 2];
    int32_t me_full[FRAMES_PER_BLOCK * 2];  /* ME at full gain for bridge */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        mix[i] = (int32_t)lroundf((float)mailbox_audio[i] * move_prescale);
        me_full[i] = 0;
    }

    /* Track raw Move audio injected via Link Audio for subtraction */
    int32_t move_injected[FRAMES_PER_BLOCK * 2];
    int any_injected = 0;
    memset(move_injected, 0, sizeof(move_injected));

    if (shadow_plugin_v2 && shadow_plugin_v2->render_block) {
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!shadow_chain_slots[s].active || !shadow_chain_slots[s].instance) continue;

            /* Inject Move track audio from Link Audio into chain before FX */
            int16_t move_track[FRAMES_PER_BLOCK * 2];
            int have_move_track = 0;
            if (link_audio.enabled && link_audio_routing_enabled &&
                shadow_chain_set_inject_audio &&
                s < link_audio.move_channel_count) {
                have_move_track = link_audio_read_channel(s, move_track, FRAMES_PER_BLOCK);
                if (have_move_track) {
                    shadow_chain_set_inject_audio(
                        shadow_chain_slots[s].instance,
                        move_track, FRAMES_PER_BLOCK);
                }
            }

            int16_t render_buffer[FRAMES_PER_BLOCK * 2];
            memset(render_buffer, 0, sizeof(render_buffer));
            shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                           render_buffer,
                                           MOVE_FRAMES_PER_BLOCK);
            /* Capture per-slot audio for Link Audio publisher (with slot volume) */
            if (link_audio.enabled && s < LINK_AUDIO_SHADOW_CHANNELS) {
                float cap_vol = shadow_chain_slots[s].volume;
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    shadow_slot_capture[s][i] = (int16_t)lroundf((float)render_buffer[i] * cap_vol);
                }
            }

            /* Accumulate raw Move audio for subtraction from mailbox */
            if (have_move_track) {
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++)
                    move_injected[i] += (int32_t)move_track[i];
                any_injected = 1;
            }

            float vol = shadow_chain_slots[s].volume;
            float gain = vol * me_input_scale;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                mix[i] += (int32_t)lroundf((float)render_buffer[i] * gain);
                me_full[i] += (int32_t)lroundf((float)render_buffer[i] * vol);
            }
        }
    }

    /* Subtract Move track audio from mix to avoid doubling.
     * Link Audio per-track streams are pre-fader; mailbox is post-fader.
     * When MFX active: mailbox was prescaled to unity, subtract at unity.
     * Otherwise: scale subtraction by Move's volume so the levels match. */
    if (any_injected) {
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++)
            mix[i] -= (int32_t)lroundf((float)move_injected[i] * link_sub_scale);
    }

    /* Save ME full-gain component for bridge split */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        if (me_full[i] > 32767) me_full[i] = 32767;
        if (me_full[i] < -32768) me_full[i] = -32768;
        native_bridge_me_component[i] = (int16_t)me_full[i];
    }
    native_bridge_capture_mv = mv;
    native_bridge_split_valid = 1;

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

    /* Capture native bridge source AFTER master FX, BEFORE master volume.
     * This bakes master FX into native bridge resampling while keeping
     * capture independent of master-volume attenuation. */
    native_capture_total_mix_snapshot_from_buffer(output_buffer);

    /* Apply master volume AFTER MFX.  When MFX is active the mix was built
     * at unity level; scale down now so the DAC output respects master volume.
     * When MFX is off the mix is already at mv level — no extra scaling. */
    if (mfx_active && mv < 0.9999f) {
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            output_buffer[i] = (int16_t)lroundf((float)output_buffer[i] * mv);
        }
    }

    /* Write final output to mailbox */
    memcpy(mailbox_audio, output_buffer, sizeof(output_buffer));
}

/* === OVERTAKE DSP LOAD/UNLOAD ===
 * Overtake modules can optionally include a dsp.so that runs in the shim's
 * audio thread.  V2-only: supports both generator (plugin_api_v2_t, outputs
 * audio) and effect (audio_fx_api_v2_t, processes combined audio in-place).
 */

/* MIDI send callback for overtake DSP → chain slots */
static int overtake_midi_send_internal(const uint8_t *msg, int len) {
    if (!msg || len < 4) return 0;
    /* Build USB-MIDI packet: [CIN, status, d1, d2] */
    uint8_t cin = (msg[1] >> 4) & 0x0F;
    uint8_t pkt[4] = { cin, msg[1], msg[2], msg[3] };
    static int midi_log_count = 0;
    int log_on = shadow_midi_out_log_enabled();
    shadow_chain_dispatch_midi_to_slots(pkt, log_on, &midi_log_count);
    return len;
}

/* MIDI send callback for overtake DSP → external USB MIDI */
static int overtake_midi_send_external(const uint8_t *msg, int len) {
    if (!msg || len < 4) return 0;
    uint8_t *midi_out = shadow_mailbox + MIDI_OUT_OFFSET;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        if (midi_out[i] == 0 && midi_out[i+1] == 0 &&
            midi_out[i+2] == 0 && midi_out[i+3] == 0) {
            memcpy(&midi_out[i], msg, 4);
            return len;
        }
    }
    return 0;  /* Buffer full */
}

static void shadow_overtake_dsp_load(const char *path) {
    /* Unload previous if any */
    if (overtake_dsp_handle) {
        shadow_log("Overtake DSP: unloading previous before loading new");
        if (overtake_dsp_gen && overtake_dsp_gen_inst && overtake_dsp_gen->destroy_instance)
            overtake_dsp_gen->destroy_instance(overtake_dsp_gen_inst);
        if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->destroy_instance)
            overtake_dsp_fx->destroy_instance(overtake_dsp_fx_inst);
        dlclose(overtake_dsp_handle);
        overtake_dsp_handle = NULL;
        overtake_dsp_gen = NULL;
        overtake_dsp_gen_inst = NULL;
        overtake_dsp_fx = NULL;
        overtake_dsp_fx_inst = NULL;
    }

    if (!path || !path[0]) return;

    overtake_dsp_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!overtake_dsp_handle) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Overtake DSP: failed to load %s: %s", path, dlerror());
        shadow_log(msg);
        return;
    }

    /* Set up host API for the overtake plugin */
    memset(&overtake_host_api, 0, sizeof(overtake_host_api));
    overtake_host_api.api_version = MOVE_PLUGIN_API_VERSION;
    overtake_host_api.sample_rate = MOVE_SAMPLE_RATE;
    overtake_host_api.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    overtake_host_api.mapped_memory = global_mmap_addr;
    overtake_host_api.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    overtake_host_api.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    overtake_host_api.log = shadow_log;
    overtake_host_api.midi_send_internal = overtake_midi_send_internal;
    overtake_host_api.midi_send_external = overtake_midi_send_external;

    /* Extract module directory from dsp path */
    char module_dir[256];
    strncpy(module_dir, path, sizeof(module_dir) - 1);
    module_dir[sizeof(module_dir) - 1] = '\0';
    char *last_slash = strrchr(module_dir, '/');
    if (last_slash) *last_slash = '\0';

    /* Try V2 generator first (e.g. SEQOMD) */
    move_plugin_init_v2_fn init_gen = (move_plugin_init_v2_fn)dlsym(
        overtake_dsp_handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (init_gen) {
        overtake_dsp_gen = init_gen(&overtake_host_api);
        if (overtake_dsp_gen && overtake_dsp_gen->create_instance) {
            /* Read defaults from module.json if available */
            char json_path[512];
            snprintf(json_path, sizeof(json_path), "%s/module.json", module_dir);
            char *defaults = NULL;
            FILE *f = fopen(json_path, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 16384) {
                    defaults = malloc(sz + 1);
                    if (defaults) {
                        size_t nr = fread(defaults, 1, sz, f);
                        defaults[nr] = '\0';
                        /* Extract just the "defaults" value */
                        const char *dp = strstr(defaults, "\"defaults\"");
                        if (!dp) { free(defaults); defaults = NULL; }
                    }
                }
                fclose(f);
            }

            overtake_dsp_gen_inst = overtake_dsp_gen->create_instance(
                module_dir, defaults);
            if (defaults) free(defaults);

            if (overtake_dsp_gen_inst) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Overtake DSP: loaded generator from %s", path);
                shadow_log(msg);
                return;
            }
        }
        overtake_dsp_gen = NULL;
    }

    /* Try audio FX v2 (effect mode) */
    audio_fx_init_v2_fn init_fx = (audio_fx_init_v2_fn)dlsym(
        overtake_dsp_handle, AUDIO_FX_INIT_V2_SYMBOL);
    if (init_fx) {
        overtake_dsp_fx = init_fx(&overtake_host_api);
        if (overtake_dsp_fx && overtake_dsp_fx->create_instance) {
            overtake_dsp_fx_inst = overtake_dsp_fx->create_instance(module_dir, NULL);
            if (overtake_dsp_fx_inst) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Overtake DSP: loaded FX from %s", path);
                shadow_log(msg);
                return;
            }
        }
        overtake_dsp_fx = NULL;
    }

    /* Neither worked */
    char msg[512];
    snprintf(msg, sizeof(msg), "Overtake DSP: no V2 generator or FX entry point in %s", path);
    shadow_log(msg);
    dlclose(overtake_dsp_handle);
    overtake_dsp_handle = NULL;
}

static void shadow_overtake_dsp_unload(void) {
    if (!overtake_dsp_handle) return;

    if (overtake_dsp_gen && overtake_dsp_gen_inst) {
        if (overtake_dsp_gen->destroy_instance)
            overtake_dsp_gen->destroy_instance(overtake_dsp_gen_inst);
        shadow_log("Overtake DSP: generator unloaded");
    }
    if (overtake_dsp_fx && overtake_dsp_fx_inst) {
        if (overtake_dsp_fx->destroy_instance)
            overtake_dsp_fx->destroy_instance(overtake_dsp_fx_inst);
        shadow_log("Overtake DSP: FX unloaded");
    }

    dlclose(overtake_dsp_handle);
    overtake_dsp_handle = NULL;
    overtake_dsp_gen = NULL;
    overtake_dsp_gen_inst = NULL;
    overtake_dsp_fx = NULL;
    overtake_dsp_fx_inst = NULL;
}

/* === DEFERRED DSP RENDERING ===
 * Render DSP into buffer (slow, ~300µs) - called POST-ioctl
 * This renders audio for the NEXT frame, adding one frame of latency (~3ms)
 * but allowing Move to process pad events faster after ioctl returns.
 */
static void shadow_inprocess_render_to_buffer(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    /* Clear the deferred buffer (used for overtake DSP) */
    memset(shadow_deferred_dsp_buffer, 0, sizeof(shadow_deferred_dsp_buffer));

    /* Clear per-slot deferred buffers */
    for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
        memset(shadow_slot_deferred[s], 0, FRAMES_PER_BLOCK * 2 * sizeof(int16_t));
        shadow_slot_deferred_valid[s] = 0;
    }

    /* Same-frame FX: render synth only into per-slot buffers.
     * FX + Link Audio inject are processed in mix_from_buffer (same frame as mailbox)
     * so the inject/subtract cancellation is sample-accurate. */
    int same_frame_fx = (shadow_chain_set_external_fx_mode != NULL &&
                         shadow_chain_process_fx != NULL);

    if (shadow_plugin_v2 && shadow_plugin_v2->render_block) {
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!shadow_chain_slots[s].active || !shadow_chain_slots[s].instance) continue;

            /* Idle gate: skip render_block if synth output has been silent.
             * Buffer is already zeroed, so FX chain in mix_from_buffer still runs
             * on zeros to let reverb/delay tails decay naturally.
             * Probe every ~0.5s to detect self-generating audio (LFOs, arps). */
            if (shadow_slot_idle[s]) {
                shadow_slot_silence_frames[s]++;
                if (shadow_slot_silence_frames[s] % 172 != 0) {
                    /* Not a probe frame — skip render, mark valid so FX still runs */
                    shadow_slot_deferred_valid[s] = 1;
                    continue;
                }
                /* Probe frame: fall through to render and check output */
            }

            if (same_frame_fx) {
                /* New path: synth only → per-slot buffer. FX in mix_from_buffer. */
                shadow_chain_set_external_fx_mode(shadow_chain_slots[s].instance, 1);
                shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                               shadow_slot_deferred[s],
                                               MOVE_FRAMES_PER_BLOCK);
                shadow_slot_deferred_valid[s] = 1;
            } else {
                /* Fallback: full render (synth + FX) → accumulated buffer.
                 * No Link Audio inject (one-frame delay would cause issues). */
                int16_t render_buffer[FRAMES_PER_BLOCK * 2];
                memset(render_buffer, 0, sizeof(render_buffer));
                shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                               render_buffer, MOVE_FRAMES_PER_BLOCK);
                if (link_audio.enabled && s < LINK_AUDIO_SHADOW_CHANNELS) {
                    float cap_vol = shadow_chain_slots[s].volume;
                    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++)
                        shadow_slot_capture[s][i] = (int16_t)lroundf((float)render_buffer[i] * cap_vol);
                }
                float vol = shadow_chain_slots[s].volume;
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    int32_t mixed = shadow_deferred_dsp_buffer[i] + (int32_t)(render_buffer[i] * vol);
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    shadow_deferred_dsp_buffer[i] = (int16_t)mixed;
                }
            }

            /* Check if synth render output is silent */
            int16_t *slot_out = same_frame_fx ? shadow_slot_deferred[s] : shadow_deferred_dsp_buffer;
            int is_silent = 1;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                if (slot_out[i] > DSP_SILENCE_LEVEL || slot_out[i] < -DSP_SILENCE_LEVEL) {
                    is_silent = 0;
                    break;
                }
            }

            if (is_silent) {
                shadow_slot_silence_frames[s]++;
                if (shadow_slot_silence_frames[s] >= DSP_IDLE_THRESHOLD) {
                    shadow_slot_idle[s] = 1;
                }
            } else {
                shadow_slot_silence_frames[s] = 0;
                shadow_slot_idle[s] = 0;
            }
        }
    }

    /* Overtake DSP generator: mix its output into the deferred buffer */
    if (overtake_dsp_gen && overtake_dsp_gen_inst && overtake_dsp_gen->render_block) {
        int16_t render_buffer[FRAMES_PER_BLOCK * 2];
        memset(render_buffer, 0, sizeof(render_buffer));
        overtake_dsp_gen->render_block(overtake_dsp_gen_inst, render_buffer, MOVE_FRAMES_PER_BLOCK);
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            int32_t mixed = shadow_deferred_dsp_buffer[i] + (int32_t)render_buffer[i];
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            shadow_deferred_dsp_buffer[i] = (int16_t)mixed;
        }
    }

    /* Note: Master FX is applied in mix_from_buffer() AFTER mixing with Move's audio */

    shadow_deferred_dsp_valid = 1;
}

/* Mix from pre-rendered buffer - called PRE-ioctl
 * When Link Audio is active: zeroes the mailbox and rebuilds from per-track
 * Link Audio data, routing each track through its slot's FX chain.
 * Tracks without active FX pass through at Move's volume level.
 * This eliminates dry signal leakage entirely (no subtraction needed).
 */
static void shadow_inprocess_mix_from_buffer(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;
    if (!shadow_deferred_dsp_valid) return;  /* No buffer to mix yet */

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    float mv = shadow_master_volume;
    int mfx_active = shadow_master_fx_chain_active();
    /* When MFX is active, build the mix at unity level so FX see a consistent
     * signal regardless of master volume.  Apply mv AFTER MFX instead. */
    float me_input_scale = mfx_active ? 1.0f : ((mv < 1.0f) ? mv : 1.0f);

    /* Save Move's audio for bridge split (before zeroing) */
    memcpy(native_bridge_move_component, mailbox_audio, sizeof(native_bridge_move_component));

    /* Accumulate ME output across slots for bridge split component */
    int32_t me_full[FRAMES_PER_BLOCK * 2];
    memset(me_full, 0, sizeof(me_full));

    /* Zero-and-rebuild approach: if Link Audio provides per-track data,
     * zero the mailbox and rebuild from Link Audio, applying FX per-slot.
     * This completely eliminates dry signal leakage — no subtraction needed.
     *
     * IMPORTANT: Only rebuild when audio data is actually flowing.
     * Session announcements set move_channel_count but don't mean audio
     * is streaming.  Without a subscriber triggering ChannelRequests,
     * the ring buffers are empty and zeroing the mailbox kills all audio. */
    uint32_t la_cur = link_audio.packets_intercepted;
    if (la_cur > la_prev_intercepted) {
        la_stale_frames = 0;
        la_prev_intercepted = la_cur;
    } else if (la_cur > 0) {
        la_stale_frames++;
    }
    /* Consider Link Audio active if packets arrived within the last ~290ms */
    int la_receiving = (la_cur > 0 && la_stale_frames < 100);

    int rebuild_from_la = (link_audio.enabled && link_audio_routing_enabled &&
                           shadow_chain_process_fx &&
                           link_audio.move_channel_count >= 4 &&
                           la_receiving);

    /* When MFX active and NOT rebuilding from Link Audio, the mailbox has
     * Move's audio at mv level.  Prescale to unity for consistent MFX input. */
    if (mfx_active && !rebuild_from_la && mv > 0.001f) {
        float inv = 1.0f / mv;
        if (inv > 20.0f) inv = 20.0f;
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            float scaled = (float)mailbox_audio[i] * inv;
            if (scaled > 32767.0f) scaled = 32767.0f;
            if (scaled < -32768.0f) scaled = -32768.0f;
            mailbox_audio[i] = (int16_t)lroundf(scaled);
        }
    }

    if (rebuild_from_la) {
        /* Zero the mailbox — all audio reconstructed from Link Audio */
        memset(mailbox_audio, 0, FRAMES_PER_BLOCK * 2 * sizeof(int16_t));

        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            /* Read Link Audio for this track (pre-fader, same frame as mailbox) */
            int16_t move_track[FRAMES_PER_BLOCK * 2];
            int have_move_track = 0;
            if (s < link_audio.move_channel_count) {
                have_move_track = link_audio_read_channel(s, move_track, FRAMES_PER_BLOCK);
            }

            int slot_active = (shadow_chain_slots[s].active &&
                               shadow_chain_slots[s].instance &&
                               shadow_slot_deferred_valid[s]);

            if (slot_active) {
                /* Phase 2 idle gate: skip FX when synth AND FX output are silent
                 * AND no Link Audio track data is flowing for this slot */
                if (shadow_slot_fx_idle[s] && shadow_slot_idle[s] && !have_move_track) continue;

                /* Active slot: combine synth + Link Audio, run through FX */
                int16_t fx_buf[FRAMES_PER_BLOCK * 2];
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    int32_t combined = (int32_t)shadow_slot_deferred[s][i];
                    if (have_move_track)
                        combined += (int32_t)move_track[i];
                    if (combined > 32767) combined = 32767;
                    if (combined < -32768) combined = -32768;
                    fx_buf[i] = (int16_t)combined;
                }

                /* Run FX chain */
                shadow_chain_process_fx(shadow_chain_slots[s].instance,
                                        fx_buf, MOVE_FRAMES_PER_BLOCK);

                /* Track FX output silence for phase 2 idle */
                int fx_silent = 1;
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    if (fx_buf[i] > DSP_SILENCE_LEVEL || fx_buf[i] < -DSP_SILENCE_LEVEL) {
                        fx_silent = 0;
                        break;
                    }
                }
                if (fx_silent) {
                    shadow_slot_fx_silence_frames[s]++;
                    if (shadow_slot_fx_silence_frames[s] >= DSP_IDLE_THRESHOLD) {
                        shadow_slot_fx_idle[s] = 1;
                    }
                } else {
                    shadow_slot_fx_silence_frames[s] = 0;
                    shadow_slot_fx_idle[s] = 0;
                }

                /* Capture for Link Audio publisher */
                if (s < LINK_AUDIO_SHADOW_CHANNELS) {
                    float cap_vol = shadow_chain_slots[s].volume;
                    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++)
                        shadow_slot_capture[s][i] = (int16_t)lroundf((float)fx_buf[i] * cap_vol);
                }

                /* Add FX output to mailbox */
                float vol = shadow_chain_slots[s].volume;
                float gain = vol * me_input_scale;
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)lroundf((float)fx_buf[i] * gain);
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    mailbox_audio[i] = (int16_t)mixed;
                    me_full[i] += (int32_t)lroundf((float)fx_buf[i] * vol);
                }
            } else if (have_move_track) {
                /* Inactive slot: pass Link Audio through.
                 * When MFX active: unity level (mv applied after MFX).
                 * Otherwise: at Move's volume level. */
                float la_scale = mfx_active ? 1.0f : mv;
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    int32_t scaled = (int32_t)lroundf((float)move_track[i] * la_scale);
                    int32_t mixed = (int32_t)mailbox_audio[i] + scaled;
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    mailbox_audio[i] = (int16_t)mixed;
                }
            }
        }
    } else if (shadow_chain_process_fx) {
        /* Fallback: no Link Audio — just process deferred synth through FX */
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!shadow_slot_deferred_valid[s] || !shadow_chain_slots[s].instance) continue;

            /* Phase 2 idle gate: skip FX when both synth AND FX output are silent */
            if (shadow_slot_fx_idle[s] && shadow_slot_idle[s]) continue;

            int16_t fx_buf[FRAMES_PER_BLOCK * 2];
            memcpy(fx_buf, shadow_slot_deferred[s], sizeof(fx_buf));
            shadow_chain_process_fx(shadow_chain_slots[s].instance,
                                    fx_buf, MOVE_FRAMES_PER_BLOCK);

            /* Track FX output silence for phase 2 idle */
            int fx_silent = 1;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                if (fx_buf[i] > DSP_SILENCE_LEVEL || fx_buf[i] < -DSP_SILENCE_LEVEL) {
                    fx_silent = 0;
                    break;
                }
            }
            if (fx_silent) {
                shadow_slot_fx_silence_frames[s]++;
                if (shadow_slot_fx_silence_frames[s] >= DSP_IDLE_THRESHOLD) {
                    shadow_slot_fx_idle[s] = 1;
                }
            } else {
                shadow_slot_fx_silence_frames[s] = 0;
                shadow_slot_fx_idle[s] = 0;
            }

            float vol = shadow_chain_slots[s].volume;
            float gain = vol * me_input_scale;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)lroundf((float)fx_buf[i] * gain);
                if (mixed > 32767) mixed = 32767;
                if (mixed < -32768) mixed = -32768;
                mailbox_audio[i] = (int16_t)mixed;
                me_full[i] += (int32_t)lroundf((float)fx_buf[i] * vol);
            }
        }
    }

    /* Mix overtake DSP buffer */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t ov = (int32_t)shadow_deferred_dsp_buffer[i];
        if (me_input_scale < 0.9999f)
            ov = (int32_t)lroundf((float)ov * me_input_scale);
        int32_t mixed = (int32_t)mailbox_audio[i] + ov;
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        mailbox_audio[i] = (int16_t)mixed;
        me_full[i] += (int32_t)shadow_deferred_dsp_buffer[i];
    }

    /* Save ME full-gain component for bridge split */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        if (me_full[i] > 32767) me_full[i] = 32767;
        if (me_full[i] < -32768) me_full[i] = -32768;
        native_bridge_me_component[i] = (int16_t)me_full[i];
    }
    native_bridge_capture_mv = mv;
    native_bridge_split_valid = 1;

    /* Overtake DSP FX: process combined Move+shadow audio in-place */
    if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->process_block) {
        overtake_dsp_fx->process_block(overtake_dsp_fx_inst, mailbox_audio, FRAMES_PER_BLOCK);
    }

    /* Apply master FX chain to combined audio - process through all 4 slots in series */
    for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
        master_fx_slot_t *s = &shadow_master_fx_slots[fx];
        if (s->instance && s->api && s->api->process_block) {
            s->api->process_block(s->instance, mailbox_audio, FRAMES_PER_BLOCK);
        }
    }

    /* Capture native bridge source AFTER master FX, BEFORE master volume.
     * This bakes master FX into native bridge resampling while keeping
     * capture independent of master-volume attenuation. */
    native_capture_total_mix_snapshot_from_buffer(mailbox_audio);

    /* Capture audio for sampler BEFORE master volume scaling (Resample source only) */
    if (sampler_source == SAMPLER_SOURCE_RESAMPLE) {
        sampler_capture_audio();
        /* Skipback: always capture Resample source into rolling buffer */
        skipback_init();
        skipback_capture(mailbox_audio);
    }

    /* Apply master volume AFTER MFX.  When MFX is active the mix was built
     * at unity level; scale down now so the DAC output respects master volume.
     * When MFX is off the mix is already at mv level — no extra scaling. */
    if (mfx_active && mv < 0.9999f) {
        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
            float scaled = (float)mailbox_audio[i] * mv;
            if (scaled > 32767.0f) scaled = 32767.0f;
            if (scaled < -32768.0f) scaled = -32768.0f;
            mailbox_audio[i] = (int16_t)lroundf(scaled);
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
static uint8_t *display_live_shm = NULL;
static shadow_midi_out_t *shadow_midi_out_shm = NULL;  /* MIDI output from shadow UI */
static uint8_t last_shadow_midi_out_ready = 0;
static shadow_midi_dsp_t *shadow_midi_dsp_shm = NULL;  /* MIDI to DSP from shadow UI */
static uint8_t last_shadow_midi_dsp_ready = 0;

static uint32_t last_screenreader_sequence = 0;  /* Track last spoken message */
static uint64_t last_speech_time_ms = 0;  /* Rate limiting for TTS */

/* Pending LED queue for rate-limited output (like standalone mode) */
#define SHADOW_LED_MAX_UPDATES_PER_TICK 16
#define SHADOW_LED_QUEUE_SAFE_BYTES 76
/* In overtake mode we clear Move's cable-0 LEDs, freeing most of the buffer */
#define SHADOW_LED_OVERTAKE_BUDGET 48
static int shadow_pending_note_color[128];   /* -1 = not pending */
static uint8_t shadow_pending_note_status[128];
static uint8_t shadow_pending_note_cin[128];
static int shadow_pending_cc_color[128];     /* -1 = not pending */
static uint8_t shadow_pending_cc_status[128];
static uint8_t shadow_pending_cc_cin[128];
static int shadow_led_queue_initialized = 0;

/* Pending INPUT queue for external MIDI (cable 2) LED commands */
/* Queues incoming LED commands from external devices (like M8) */
#define SHADOW_INPUT_LED_MAX_PER_TICK 24
static int shadow_input_pending_note_color[128];   /* -1 = not pending */
static uint8_t shadow_input_pending_note_status[128];
static uint8_t shadow_input_pending_note_cin[128];
static int shadow_input_queue_initialized = 0;

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
static int shm_midi_dsp_fd = -1;
static int shm_screenreader_fd = -1;
static int shm_overlay_fd = -1;

/* Shadow initialization state */
static int shadow_shm_initialized = 0;

/* Initialize shadow shared memory segments */

/* Signal handler for crash diagnostics - async-signal-safe */
static void crash_signal_handler(int sig)
{
    const char *name;
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGBUS:  name = "SIGBUS";  break;
        case SIGABRT: name = "SIGABRT"; break;
        case SIGTERM: name = "SIGTERM"; break;
        case SIGINT:  name = "SIGINT";  break;
        default:      name = "UNKNOWN"; break;
    }
    /* Build message: "Caught <signal> - terminating (pid=<pid>)" */
    char msg[128];
    int pos = 0;
    const char prefix[] = "Caught ";
    for (int i = 0; prefix[i]; i++) msg[pos++] = prefix[i];
    for (int i = 0; name[i]; i++) msg[pos++] = name[i];
    const char suffix[] = " - terminating";
    for (int i = 0; suffix[i]; i++) msg[pos++] = suffix[i];
    msg[pos] = '\0';

    unified_log_crash(msg);
    _exit(128 + sig);
}

static void init_shadow_shm(void)
{
    if (shadow_shm_initialized) return;

    /* Initialize unified logging first so we can log during shm init */
    unified_log_init();

    /* Install crash signal handlers */
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGBUS,  crash_signal_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGTERM, crash_signal_handler);

    /* Log startup identity (always-on, no flag needed) */
    {
        char init_msg[64];
        snprintf(init_msg, sizeof(init_msg), "Shim init: pid=%d ppid=%d", getpid(), getppid());
        unified_log_crash(init_msg);
    }

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

    /* Create/open live display shared memory (for remote display server) */
    int shm_display_live_fd = shm_open(SHM_DISPLAY_LIVE, O_CREAT | O_RDWR, 0666);
    if (shm_display_live_fd >= 0) {
        ftruncate(shm_display_live_fd, DISPLAY_BUFFER_SIZE);
        display_live_shm = (uint8_t *)mmap(NULL, DISPLAY_BUFFER_SIZE,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, shm_display_live_fd, 0);
        if (display_live_shm == MAP_FAILED) {
            display_live_shm = NULL;
            printf("Shadow: Failed to mmap live display shm\n");
        } else {
            memset(display_live_shm, 0, DISPLAY_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create live display shm\n");
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
            /* Initialize TTS defaults */
            shadow_control->tts_enabled = 0;    /* Screen Reader off by default */
            shadow_control->tts_volume = 70;    /* 70% volume */
            shadow_control->tts_pitch = 110;    /* 110 Hz */
            shadow_control->tts_speed = 1.0f;   /* Normal speed */
            shadow_control->tts_engine = 0;     /* 0=espeak-ng, 1=flite */
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

    /* Create/open MIDI-to-DSP shared memory (for shadow UI to route MIDI to chain slots) */
    shm_midi_dsp_fd = shm_open(SHM_SHADOW_MIDI_DSP, O_CREAT | O_RDWR, 0666);
    if (shm_midi_dsp_fd >= 0) {
        ftruncate(shm_midi_dsp_fd, sizeof(shadow_midi_dsp_t));
        shadow_midi_dsp_shm = (shadow_midi_dsp_t *)mmap(NULL, sizeof(shadow_midi_dsp_t),
                                                         PROT_READ | PROT_WRITE,
                                                         MAP_SHARED, shm_midi_dsp_fd, 0);
        if (shadow_midi_dsp_shm == MAP_FAILED) {
            shadow_midi_dsp_shm = NULL;
            printf("Shadow: Failed to mmap midi_dsp shm\n");
        } else {
            memset(shadow_midi_dsp_shm, 0, sizeof(shadow_midi_dsp_t));
        }
    } else {
        printf("Shadow: Failed to create midi_dsp shm\n");
    }

    /* Create/open screen reader shared memory (for accessibility: TTS and D-Bus announcements) */
    shm_screenreader_fd = shm_open(SHM_SHADOW_SCREENREADER, O_CREAT | O_RDWR, 0666);
    if (shm_screenreader_fd >= 0) {
        ftruncate(shm_screenreader_fd, sizeof(shadow_screenreader_t));
        shadow_screenreader_shm = (shadow_screenreader_t *)mmap(NULL, sizeof(shadow_screenreader_t),
                                                                 PROT_READ | PROT_WRITE,
                                                                 MAP_SHARED, shm_screenreader_fd, 0);
        if (shadow_screenreader_shm == MAP_FAILED) {
            shadow_screenreader_shm = NULL;
            printf("Shadow: Failed to mmap screenreader shm\n");
        } else {
            memset(shadow_screenreader_shm, 0, sizeof(shadow_screenreader_t));
        }
    } else {
        printf("Shadow: Failed to create screenreader shm\n");
    }

    /* Create/open overlay state shared memory (sampler/skipback state for JS rendering) */
    shm_overlay_fd = shm_open(SHM_SHADOW_OVERLAY, O_CREAT | O_RDWR, 0666);
    if (shm_overlay_fd >= 0) {
        ftruncate(shm_overlay_fd, SHADOW_OVERLAY_BUFFER_SIZE);
        shadow_overlay_shm = (shadow_overlay_state_t *)mmap(NULL, SHADOW_OVERLAY_BUFFER_SIZE,
                                                             PROT_READ | PROT_WRITE,
                                                             MAP_SHARED, shm_overlay_fd, 0);
        if (shadow_overlay_shm == MAP_FAILED) {
            shadow_overlay_shm = NULL;
            printf("Shadow: Failed to mmap overlay shm\n");
        } else {
            memset(shadow_overlay_shm, 0, SHADOW_OVERLAY_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create overlay shm\n");
    }

    /* TTS engine uses lazy initialization - will init on first speak */
    tts_set_volume(70);  /* Set volume early (safe, doesn't require TTS init) */
    printf("Shadow: TTS engine configured (will init on first use)\n");

    /* Initialize Link Audio state */
    memset(&link_audio, 0, sizeof(link_audio));
    link_audio.move_socket_fd = -1;
    link_audio.publisher_socket_fd = -1;
    memset(shadow_slot_capture, 0, sizeof(shadow_slot_capture));

    shadow_shm_initialized = 1;
    printf("Shadow: Shared memory initialized (audio=%p, midi=%p, ui_midi=%p, display=%p, control=%p, ui=%p, param=%p, midi_out=%p, midi_dsp=%p, screenreader=%p, overlay=%p)\n",
           shadow_audio_shm, shadow_midi_shm, shadow_ui_midi_shm,
           shadow_display_shm, shadow_control, shadow_ui_state, shadow_param, shadow_midi_out_shm, shadow_midi_dsp_shm, shadow_screenreader_shm, shadow_overlay_shm);
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

/* Monitor screen reader messages and speak them with TTS (debounced) */
#define TTS_DEBOUNCE_MS 300  /* Wait 300ms of silence before speaking */
static char pending_tts_message[SHADOW_SCREENREADER_TEXT_LEN] = {0};
static uint64_t last_message_time_ms = 0;
static bool has_pending_message = false;

static void shadow_check_screenreader(void)
{
    if (!shadow_screenreader_shm) return;

    /* Get current time in milliseconds */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);

    /* Check if there's a new message (sequence incremented) */
    uint32_t current_sequence = shadow_screenreader_shm->sequence;
    if (current_sequence != last_screenreader_sequence) {
        /* New message arrived - buffer it and reset debounce timer */
        if (shadow_screenreader_shm->text[0] != '\0') {
            strncpy(pending_tts_message, shadow_screenreader_shm->text, sizeof(pending_tts_message) - 1);
            pending_tts_message[sizeof(pending_tts_message) - 1] = '\0';
            last_message_time_ms = now_ms;
            has_pending_message = true;
            unified_log("tts_monitor", LOG_LEVEL_DEBUG, "Buffered: '%s'", pending_tts_message);
        }
        last_screenreader_sequence = current_sequence;
        return;
    }

    /* Check if debounce period has elapsed and we have a pending message */
    if (has_pending_message && (now_ms - last_message_time_ms >= TTS_DEBOUNCE_MS)) {
        /* Apply TTS settings from shared memory before speaking */
        if (shadow_control) {
            /* Check for engine switch (must happen before other settings) */
            const char *current_engine = tts_get_engine();
            const char *requested_engine = shadow_control->tts_engine == 1 ? "flite" : "espeak";
            if (strcmp(current_engine, requested_engine) != 0) {
                tts_set_engine(requested_engine);
            }

            tts_set_enabled(shadow_control->tts_enabled != 0);
            tts_set_volume(shadow_control->tts_volume);
            tts_set_speed(shadow_control->tts_speed);
            tts_set_pitch((float)shadow_control->tts_pitch);
        }

        /* Speak the buffered message */
        unified_log("tts_monitor", LOG_LEVEL_DEBUG, "Speaking (debounced): '%s'", pending_tts_message);
        if (tts_speak(pending_tts_message)) {
            last_speech_time_ms = now_ms;
        }
        has_pending_message = false;
        pending_tts_message[0] = '\0';
    }
}

/* ==========================================================================
 * PIN Challenge Display Scanner
 *
 * Monitors the pin_challenge_active flag set by the web shim when a browser
 * connects to move.local and triggers a PIN challenge. When detected, we
 * wait for the PIN to render on the display, extract the 6 digits, and
 * speak them via TTS.
 *
 * Display format: 128x64 @ 1bpp, column-major (8 pages of 128 bytes).
 * PIN digits appear on pages 3-4 only, all other pages are blank.
 * ========================================================================== */

/* PIN scanner state machine */
#define PIN_STATE_IDLE     0
#define PIN_STATE_WAITING  1  /* Waiting for PIN to render (~500ms) */
#define PIN_STATE_SCANNING 2  /* Scanning display for digits */
#define PIN_STATE_COOLDOWN 3  /* PIN spoken, cooling down before accepting new */

static int pin_state = PIN_STATE_IDLE;
static uint64_t pin_state_entered_ms = 0;
static char pin_last_spoken[8] = {0};  /* Last PIN we spoke, to avoid repeats */

/* File-scope display buffer for PIN scanning, accumulated from slices */
static uint8_t pin_display_buf[DISPLAY_BUFFER_SIZE];
static int pin_display_slices_seen[6] = {0};
static int pin_display_complete = 0;

/* Shift+Menu double-click detection state */
static uint64_t shift_menu_pending_ms = 0;
static int shift_menu_pending = 0;

/* Accumulate a display slice into the PIN display buffer.
 * Called from the ioctl handler's slice capture section. */
static void pin_accumulate_slice(int idx, const uint8_t *data, int bytes)
{
    if (idx < 0 || idx >= 6) return;
    memcpy(pin_display_buf + idx * 172, data, bytes);
    pin_display_slices_seen[idx] = 1;

    /* Check if all slices received */
    int all = 1;
    for (int i = 0; i < 6; i++) {
        if (!pin_display_slices_seen[i]) { all = 0; break; }
    }
    if (all) {
        pin_display_complete = 1;
        memset(pin_display_slices_seen, 0, sizeof(pin_display_slices_seen));

        /* File-triggered display dump: touch /tmp/dump_display to capture */
        if (access("/tmp/dump_display", F_OK) == 0) {
            unlink("/tmp/dump_display");
            FILE *f = fopen("/tmp/pin_display.bin", "w");
            if (f) {
                fwrite(pin_display_buf, 1, 1024, f);
                fclose(f);
                shadow_log("PIN: display buffer dumped to /tmp/pin_display.bin");
            }
        }
    }
}

/* Digit template hash table.
 * Each digit (0-9) maps to a polynomial hash of its column bytes in pages 3-4.
 * These are populated from actual display captures during testing.
 * A value of 0 means "not yet captured". */
static uint32_t pin_digit_hashes[10] = {
    0x8abc24d1,   /* 0 */
    0xa8721e5e,   /* 1 */
    0x3eeaf9a2,   /* 2 */
    0xb680019e,   /* 3 */
    0xc751c4ad,   /* 4 */
    0xf7a9c384,   /* 5 */
    0xc9805ffb,   /* 6 */
    0x538e156e,   /* 7 */
    0xf35f5d11,   /* 8 */
    0xa061c01d,   /* 9 */
};

/* Compute hash for a digit group spanning columns [start, end) in pages 3-4 */
static uint32_t pin_digit_hash(const uint8_t *display, int start, int end)
{
    uint32_t hash = 5381;
    for (int c = start; c < end; c++) {
        hash = hash * 33 + display[3 * 128 + c];
        hash = hash * 33 + display[4 * 128 + c];
    }
    return hash;
}

/* Check if the display looks like a PIN screen:
 * Pages 3-4 have content, other pages are mostly blank. */
static int pin_display_is_pin_screen(const uint8_t *display)
{
    /* Count non-zero bytes in pages 3-4 */
    int active = 0;
    for (int i = 3 * 128; i < 5 * 128; i++) {
        if (display[i]) active++;
    }
    if (active < 10) return 0;  /* Too few lit pixels */

    /* Check that other pages are mostly blank */
    int other = 0;
    for (int page = 0; page < 8; page++) {
        if (page == 3 || page == 4) continue;
        for (int col = 0; col < 128; col++) {
            if (display[page * 128 + col]) other++;
        }
    }
    return other < 20;  /* Allow a few stray pixels */
}

/* Extract digits from display and build TTS string.
 * Returns 1 on success with pin_text and raw_digits filled, 0 on failure.
 * raw_digits must be at least 7 bytes (6 digits + NUL). */
static int pin_extract_digits(const uint8_t *display, char *pin_text, int text_len, char *raw_digits)
{
    char logbuf[512];

    if (!pin_display_is_pin_screen(display)) {
        shadow_log("PIN: display doesn't look like PIN screen");
        return 0;
    }

    /* Segment digits: find groups of consecutive non-zero columns in pages 3-4 */
    typedef struct { int start; int end; } digit_span_t;
    digit_span_t spans[8];
    int span_count = 0;
    int in_digit = 0;
    int digit_start = 0;

    for (int col = 0; col < 128; col++) {
        int has_content = display[3 * 128 + col] || display[4 * 128 + col];
        if (has_content && !in_digit) {
            digit_start = col;
            in_digit = 1;
        } else if (!has_content && in_digit) {
            if (span_count < 8) {
                spans[span_count].start = digit_start;
                spans[span_count].end = col;
                span_count++;
            }
            in_digit = 0;
        }
    }
    if (in_digit && span_count < 8) {
        spans[span_count].start = digit_start;
        spans[span_count].end = 128;
        span_count++;
    }

    /* Expect exactly 6 digit groups */
    if (span_count != 6) {
        snprintf(logbuf, sizeof(logbuf), "PIN: expected 6 digit groups, found %d", span_count);
        shadow_log(logbuf);
        /* Log digit group info for debugging */
        for (int i = 0; i < span_count; i++) {
            snprintf(logbuf, sizeof(logbuf), "PIN: group %d: cols %d-%d (width %d)",
                     i, spans[i].start, spans[i].end,
                     spans[i].end - spans[i].start);
            shadow_log(logbuf);
        }
        return 0;
    }

    /* Match each digit group */
    char digits[7] = {0};
    int all_matched = 1;

    for (int i = 0; i < 6; i++) {
        uint32_t hash = pin_digit_hash(display, spans[i].start, spans[i].end);
        int matched = -1;

        /* Look up in hash table */
        for (int d = 0; d < 10; d++) {
            if (pin_digit_hashes[d] != 0 && pin_digit_hashes[d] == hash) {
                matched = d;
                break;
            }
        }

        if (matched >= 0) {
            digits[i] = '0' + matched;
        } else {
            digits[i] = '?';
            all_matched = 0;

            /* Log the hash and raw column data for template creation */
            snprintf(logbuf, sizeof(logbuf), "PIN: digit %d (cols %d-%d) hash=0x%08x UNMATCHED",
                     i, spans[i].start, spans[i].end, hash);
            shadow_log(logbuf);

            /* Dump column bytes for pages 3-4 */
            int pos = 0;
            pos += snprintf(logbuf + pos, sizeof(logbuf) - pos, "PIN: digit %d p3:", i);
            for (int c = spans[i].start; c < spans[i].end && pos < 300; c++) {
                pos += snprintf(logbuf + pos, sizeof(logbuf) - pos,
                               " %02x", display[3 * 128 + c]);
            }
            pos += snprintf(logbuf + pos, sizeof(logbuf) - pos, " p4:");
            for (int c = spans[i].start; c < spans[i].end && pos < 480; c++) {
                pos += snprintf(logbuf + pos, sizeof(logbuf) - pos,
                               " %02x", display[4 * 128 + c]);
            }
            shadow_log(logbuf);
        }
    }
    digits[6] = '\0';

    /* Copy raw digits to output parameter for dedup */
    memcpy(raw_digits, digits, 7);

    if (!all_matched) {
        snprintf(logbuf, sizeof(logbuf), "PIN: some digits unmatched, raw string: %s", digits);
        shadow_log(logbuf);
        /* Still try to speak what we have - the user may recognize partial info */
    }

    /* Build TTS string: repeat 2 times with a pause.
     * "Pairing pin displayed: 1, 2, 3, 4, 5, 6. (pause) (repeat)"
     * Keep under 12 seconds of audio to fit in ring buffer. */
    int n = 0;
    for (int rep = 0; rep < 2; rep++) {
        if (rep > 0) n += snprintf(pin_text + n, text_len - n, ".... ");
        n += snprintf(pin_text + n, text_len - n, "Pairing pin displayed: ");
        for (int i = 0; i < 6 && n < text_len - 4; i++) {
            if (i > 0) n += snprintf(pin_text + n, text_len - n, ", ");
            n += snprintf(pin_text + n, text_len - n, "%c", digits[i]);
        }
        n += snprintf(pin_text + n, text_len - n, ". ");
    }

    snprintf(logbuf, sizeof(logbuf), "PIN: extracted digits: %s", digits);
    shadow_log(logbuf);
    return 1;
}

/* Main PIN scanner - called from the display section of the tick loop.
 *
 * State machine: IDLE → WAITING → SCANNING → COOLDOWN → IDLE
 *
 * Key design: we never clear pin_challenge_active from the shim side because
 * the web shim will immediately re-set it on the browser's next HTTP poll.
 * Instead, after speaking we enter COOLDOWN which ignores the flag until it
 * naturally clears (user enters PIN or navigates away) or a timeout expires. */
static void pin_check_and_speak(void)
{
    if (!shadow_control) return;

    /* Get current time */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);

    uint8_t challenge = shadow_control->pin_challenge_active;

    /* If challenge-response submitted (2), cancel any active scan */
    if (challenge == 2 && pin_state != PIN_STATE_IDLE && pin_state != PIN_STATE_COOLDOWN) {
        shadow_log("PIN: challenge-response submitted, cancelling scan");
        pin_state = PIN_STATE_COOLDOWN;
        pin_state_entered_ms = now_ms;
        return;
    }

    /* State machine */
    switch (pin_state) {
    case PIN_STATE_IDLE:
        /* Only trigger on challenge=1 from IDLE */
        if (challenge == 1) {
            pin_state = PIN_STATE_WAITING;
            pin_state_entered_ms = now_ms;
            pin_display_complete = 0;
            memset(pin_display_slices_seen, 0, sizeof(pin_display_slices_seen));
            shadow_log("PIN: challenge detected, waiting for display render");
        }
        break;

    case PIN_STATE_WAITING:
        /* Wait 500ms for the PIN to render on the display */
        if (now_ms - pin_state_entered_ms > 500) {
            pin_state = PIN_STATE_SCANNING;
            pin_display_complete = 0;
            memset(pin_display_slices_seen, 0, sizeof(pin_display_slices_seen));
            shadow_log("PIN: entering scan mode");
        }
        break;

    case PIN_STATE_SCANNING:
        if (pin_display_complete) {
            char pin_text[512];
            char raw_digits[8] = {0};
            if (pin_extract_digits(pin_display_buf, pin_text, sizeof(pin_text), raw_digits)) {
                if (strcmp(raw_digits, pin_last_spoken) == 0) {
                    /* Same PIN as last time — skip to avoid repeating */
                    pin_state = PIN_STATE_COOLDOWN;
                    pin_state_entered_ms = now_ms;
                } else {
                    { char lb[128]; snprintf(lb, sizeof(lb), "PIN: speaking '%s'", pin_text); shadow_log(lb); }
                    tts_speak(pin_text);
                    strncpy(pin_last_spoken, raw_digits, sizeof(pin_last_spoken) - 1);
                    pin_state = PIN_STATE_COOLDOWN;
                    pin_state_entered_ms = now_ms;
                }
            } else {
                /* Try again on next full frame */
                pin_display_complete = 0;
            }
        }
        /* Timeout after 10 seconds of scanning */
        if (now_ms - pin_state_entered_ms > 10000) {
            shadow_log("PIN: scan timeout");
            pin_state = PIN_STATE_COOLDOWN;
            pin_state_entered_ms = now_ms;
        }
        break;

    case PIN_STATE_COOLDOWN:
        /* Wait for the challenge flag to clear naturally (user entered PIN
         * or browser navigated away), then return to IDLE.
         * Safety timeout after 60s to avoid being stuck forever. */
        if (challenge == 0 || challenge == 2) {
            pin_state = PIN_STATE_IDLE;
            pin_last_spoken[0] = '\0';  /* Clear dedup so new session can repeat */
            shadow_log("PIN: challenge cleared, returning to idle");
        } else if (now_ms - pin_state_entered_ms > 5000) {
            pin_state = PIN_STATE_IDLE;
            shadow_log("PIN: cooldown timeout, returning to idle");
        }
        break;

    default:
        break;
    }
}

/* Mix shadow audio into mailbox audio buffer - TRIPLE BUFFERED */
static void shadow_mix_audio(void)
{
    if (!shadow_audio_shm || !global_mmap_addr) return;
    if (!shadow_control || !shadow_control->shadow_ready) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);

    /* Check for new screen reader messages and speak them */
    shadow_check_screenreader();

    /* TTS test: speak once after 3 seconds to verify audio works */
    static int tts_test_frame_count = 0;
    static bool tts_test_done = false;
    if (!tts_test_done && shadow_control->shadow_ready) {
        tts_test_frame_count++;
        if (tts_test_frame_count == 1035) {  /* ~3 seconds at 44.1kHz, 128 frames/block */
            printf("TTS test: Speaking test phrase...\n");
            /* Apply TTS settings before test phrase */
            {
                const char *current_engine = tts_get_engine();
                const char *requested_engine = shadow_control->tts_engine == 1 ? "flite" : "espeak";
                if (strcmp(current_engine, requested_engine) != 0) {
                    tts_set_engine(requested_engine);
                }
            }
            tts_set_enabled(shadow_control->tts_enabled != 0);
            tts_set_volume(shadow_control->tts_volume);
            tts_set_speed(shadow_control->tts_speed);
            tts_set_pitch((float)shadow_control->tts_pitch);
            tts_speak("Text to speech is working");
            tts_test_done = true;
        }
    }

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

    /* NOTE: TTS mixing moved to shadow_mix_tts() which runs AFTER
     * shadow_inprocess_mix_from_buffer(). That function zeros the mailbox
     * when Link Audio is active, so TTS must be mixed in afterward. */
}

/* Mix TTS audio into mailbox.  Called AFTER shadow_inprocess_mix_from_buffer()
 * because that function may zero-and-rebuild the mailbox when Link Audio is
 * active.  Mixing TTS here ensures it is never wiped by the rebuild. */
static void shadow_mix_tts(void)
{
    if (!global_mmap_addr) return;
    if (!tts_is_speaking()) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    static int16_t tts_buffer[FRAMES_PER_BLOCK * 2];  /* Stereo interleaved */
    int frames_read = tts_get_audio(tts_buffer, FRAMES_PER_BLOCK);

    if (frames_read > 0) {
        float mv = shadow_master_volume;
        for (int i = 0; i < frames_read * 2; i++) {
            int32_t scaled_tts = (int32_t)lroundf((float)tts_buffer[i] * mv);
            int32_t mixed = (int32_t)mailbox_audio[i] + scaled_tts;
            /* Clip to int16 range */
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            mailbox_audio[i] = (int16_t)mixed;
        }
    }
}

/* Initialize pending LED queue arrays */
static void shadow_init_led_queue(void) {
    if (shadow_led_queue_initialized) return;
    for (int i = 0; i < 128; i++) {
        shadow_pending_note_color[i] = -1;
        shadow_pending_cc_color[i] = -1;
    }
    shadow_led_queue_initialized = 1;
}

/* Queue an LED update for rate-limited sending */
static void shadow_queue_led(uint8_t cin, uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t type = status & 0xF0;
    if (type == 0x90) {
        /* Note-on: queue by note number */
        shadow_pending_note_color[data1] = data2;
        shadow_pending_note_status[data1] = status;
        shadow_pending_note_cin[data1] = cin;
    } else if (type == 0xB0) {
        /* CC: queue by CC number */
        shadow_pending_cc_color[data1] = data2;
        shadow_pending_cc_status[data1] = status;
        shadow_pending_cc_cin[data1] = cin;
    }
}

/* In overtake mode, clear Move's cable-0 LED packets from MIDI_OUT buffer.
 * Move continuously writes its LED state but in overtake mode we own all LEDs.
 * Must be called BEFORE shadow_inject_ui_midi_out() so cable-2 (external MIDI)
 * messages can find empty slots in the buffer. */
static void shadow_clear_move_leds_if_overtake(void) {
    if (!shadow_control || shadow_control->overtake_mode < 2) return;

    uint8_t *midi_out = shadow_mailbox + MIDI_OUT_OFFSET;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cable = (midi_out[i] >> 4) & 0x0F;
        uint8_t type = midi_out[i+1] & 0xF0;
        if (cable == 0 && (type == 0x90 || type == 0xB0)) {
            midi_out[i] = 0;
            midi_out[i+1] = 0;
            midi_out[i+2] = 0;
            midi_out[i+3] = 0;
        }
    }
}

/* Flush pending LED updates to hardware, rate-limited */
static void shadow_flush_pending_leds(void) {
    shadow_init_led_queue();

    uint8_t *midi_out = shadow_mailbox + MIDI_OUT_OFFSET;
    int overtake = shadow_control && shadow_control->overtake_mode >= 2;

    /* Count how many slots are already used */
    int used = 0;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        if (midi_out[i] != 0 || midi_out[i+1] != 0 ||
            midi_out[i+2] != 0 || midi_out[i+3] != 0) {
            used += 4;
        }
    }

    /* In overtake mode use full buffer (after clearing Move's LEDs).
     * In normal mode stay within safe limit to coexist with Move's packets. */
    int max_bytes = overtake ? MIDI_BUFFER_SIZE : SHADOW_LED_QUEUE_SAFE_BYTES;
    int available = (max_bytes - used) / 4;
    int budget = overtake ? SHADOW_LED_OVERTAKE_BUDGET : SHADOW_LED_MAX_UPDATES_PER_TICK;
    if (available <= 0 || budget <= 0) return;
    if (budget > available) budget = available;

    int sent = 0;
    int hw_offset = 0;

    /* First flush pending note-on messages */
    for (int i = 0; i < 128 && sent < budget; i++) {
        if (shadow_pending_note_color[i] >= 0) {
            /* Find empty slot */
            while (hw_offset < MIDI_BUFFER_SIZE) {
                if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                    midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                    break;
                }
                hw_offset += 4;
            }
            if (hw_offset >= MIDI_BUFFER_SIZE) break;

            midi_out[hw_offset] = shadow_pending_note_cin[i];
            midi_out[hw_offset+1] = shadow_pending_note_status[i];
            midi_out[hw_offset+2] = (uint8_t)i;
            midi_out[hw_offset+3] = (uint8_t)shadow_pending_note_color[i];
            shadow_pending_note_color[i] = -1;
            hw_offset += 4;
            sent++;
        }
    }

    /* Then flush pending CC messages */
    for (int i = 0; i < 128 && sent < budget; i++) {
        if (shadow_pending_cc_color[i] >= 0) {
            /* Find empty slot */
            while (hw_offset < MIDI_BUFFER_SIZE) {
                if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                    midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                    break;
                }
                hw_offset += 4;
            }
            if (hw_offset >= MIDI_BUFFER_SIZE) break;

            midi_out[hw_offset] = shadow_pending_cc_cin[i];
            midi_out[hw_offset+1] = shadow_pending_cc_status[i];
            midi_out[hw_offset+2] = (uint8_t)i;
            midi_out[hw_offset+3] = (uint8_t)shadow_pending_cc_color[i];
            shadow_pending_cc_color[i] = -1;
            hw_offset += 4;
            sent++;
        }
    }
}

/* Initialize pending INPUT LED queue arrays */
static void shadow_init_input_led_queue(void) {
    if (shadow_input_queue_initialized) return;
    for (int i = 0; i < 128; i++) {
        shadow_input_pending_note_color[i] = -1;
    }
    shadow_input_queue_initialized = 1;
}

/* Queue an incoming LED command (cable 2 note-on) for rate-limited forwarding */
static void shadow_queue_input_led(uint8_t cin, uint8_t status, uint8_t note, uint8_t velocity) {
    shadow_init_input_led_queue();
    uint8_t type = status & 0xF0;
    if (type == 0x90) {
        shadow_input_pending_note_color[note] = velocity;
        shadow_input_pending_note_status[note] = status;
        shadow_input_pending_note_cin[note] = cin;
    }
}

/* Flush pending input LED commands to UI MIDI buffer, rate-limited */
static void shadow_flush_pending_input_leds(void) {
    if (!shadow_ui_midi_shm || !shadow_control) return;
    shadow_init_input_led_queue();

    int budget = SHADOW_INPUT_LED_MAX_PER_TICK;
    int sent = 0;

    for (int i = 0; i < 128 && sent < budget; i++) {
        if (shadow_input_pending_note_color[i] >= 0) {
            /* Find empty slot in UI MIDI buffer */
            int found = 0;
            for (int slot = 0; slot < MIDI_BUFFER_SIZE; slot += 4) {
                if (shadow_ui_midi_shm[slot] == 0) {
                    shadow_ui_midi_shm[slot] = shadow_input_pending_note_cin[i];
                    shadow_ui_midi_shm[slot + 1] = shadow_input_pending_note_status[i];
                    shadow_ui_midi_shm[slot + 2] = (uint8_t)i;
                    shadow_ui_midi_shm[slot + 3] = (uint8_t)shadow_input_pending_note_color[i];
                    shadow_control->midi_ready++;
                    found = 1;
                    break;
                }
            }
            if (!found) break;  /* Buffer full, try again next tick */
            shadow_input_pending_note_color[i] = -1;
            sent++;
        }
    }
}

/* Inject shadow UI MIDI out into mailbox before ioctl. */
static void shadow_inject_ui_midi_out(void) {
    if (!shadow_midi_out_shm) return;
    if (shadow_midi_out_shm->ready == last_shadow_midi_out_ready) return;

    last_shadow_midi_out_ready = shadow_midi_out_shm->ready;
    shadow_init_led_queue();

    /* Snapshot buffer first, then reset write_idx.
     * Copy before resetting to avoid a race where the JS process writes
     * new data between our reset and memcpy. */
    int snapshot_len = shadow_midi_out_shm->write_idx;
    uint8_t local_buf[SHADOW_MIDI_OUT_BUFFER_SIZE];
    int copy_len = snapshot_len < (int)SHADOW_MIDI_OUT_BUFFER_SIZE
                 ? snapshot_len : (int)SHADOW_MIDI_OUT_BUFFER_SIZE;
    if (copy_len > 0) {
        memcpy(local_buf, shadow_midi_out_shm->buffer, copy_len);
    }
    __sync_synchronize();
    shadow_midi_out_shm->write_idx = 0;
    memset(shadow_midi_out_shm->buffer, 0, SHADOW_MIDI_OUT_BUFFER_SIZE);

    /* Inject into shadow_mailbox at MIDI_OUT_OFFSET */
    uint8_t *midi_out = shadow_mailbox + MIDI_OUT_OFFSET;

    int hw_offset = 0;
    for (int i = 0; i < copy_len; i += 4) {
        uint8_t cin = local_buf[i];
        uint8_t cable = (cin >> 4) & 0x0F;
        uint8_t status = local_buf[i + 1];
        uint8_t data1 = local_buf[i + 2];
        uint8_t data2 = local_buf[i + 3];
        uint8_t type = status & 0xF0;

        /* Queue cable 0 LED messages (note-on, CC) for rate-limited sending */
        if (cable == 0 && (type == 0x90 || type == 0xB0)) {
            shadow_queue_led(cin, status, data1, data2);
            continue;  /* Don't copy directly, will be flushed later */
        }

        /* All other messages: copy directly to mailbox */
        while (hw_offset < MIDI_BUFFER_SIZE) {
            if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                break;
            }
            hw_offset += 4;
        }
        if (hw_offset >= MIDI_BUFFER_SIZE) break;  /* Buffer full */

        memcpy(&midi_out[hw_offset], &local_buf[i], 4);
        hw_offset += 4;
    }
}

/* Drain MIDI-to-DSP buffer from shadow UI and dispatch to chain slots. */
static void shadow_drain_ui_midi_dsp(void) {
    if (!shadow_midi_dsp_shm) return;
    if (shadow_midi_dsp_shm->ready == last_shadow_midi_dsp_ready) return;

    last_shadow_midi_dsp_ready = shadow_midi_dsp_shm->ready;

    static int midi_log_count = 0;
    int log_on = shadow_midi_out_log_enabled();

    for (int i = 0; i < shadow_midi_dsp_shm->write_idx && i < SHADOW_MIDI_DSP_BUFFER_SIZE; i += 4) {
        uint8_t status = shadow_midi_dsp_shm->buffer[i];
        uint8_t d1 = shadow_midi_dsp_shm->buffer[i + 1];
        uint8_t d2 = shadow_midi_dsp_shm->buffer[i + 2];

        /* Validate status byte has high bit set */
        if (!(status & 0x80)) continue;

        /* Construct USB-MIDI packet for dispatch: [CIN, status, d1, d2] */
        uint8_t cin = (status >> 4) & 0x0F;
        uint8_t pkt[4] = { cin, status, d1, d2 };

        shadow_chain_dispatch_midi_to_slots(pkt, log_on, &midi_log_count);
    }

    /* Clear after processing */
    shadow_midi_dsp_shm->write_idx = 0;
    memset(shadow_midi_dsp_shm->buffer, 0, SHADOW_MIDI_DSP_BUFFER_SIZE);
}

/* Check for and send screen reader announcements via D-Bus */
static void shadow_check_screenreader_announcements(void) {
    static uint32_t last_announcement_sequence = 0;

    if (!shadow_screenreader_shm) return;

    /* Check if there's a new message (sequence incremented) */
    uint32_t current_sequence = shadow_screenreader_shm->sequence;
    if (current_sequence == last_announcement_sequence) return;

    last_announcement_sequence = current_sequence;

    /* Queue announcement for D-Bus broadcast */
    if (shadow_screenreader_shm->text[0]) {
        send_screenreader_announcement(shadow_screenreader_shm->text);
        /* Inject immediately - don't wait for Move's next D-Bus activity */
        shadow_inject_pending_announcements();
    }
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
            if (d1 == 9) {  /* Jog wheel touch - track state, block from Move */
                shadow_jog_touched = (type == 0x90 && d2 > 0) ? 1 : 0;
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
    static int display_phase = 0;  /* 0-6: phases of display push */
    static int display_hidden_for_volume = 0;

    if (!shadow_display_shm || !global_mmap_addr) {
        return;
    }
    if (!shadow_control || !shadow_control->shadow_ready) {
        return;
    }
    if (!shadow_display_mode) {
        display_phase = 0;
        display_hidden_for_volume = 0;
        shadow_block_plain_volume_hide_until_release = 0;
        return;  /* Not in shadow mode */
    }
    if (!shadow_volume_knob_touched) {
        shadow_block_plain_volume_hide_until_release = 0;
    }
    if (shadow_volume_knob_touched && !shadow_shift_held) {
        if (shadow_block_plain_volume_hide_until_release) {
            /* Keep shadow UI visible until shortcut's volume touch is fully released. */
            if (display_hidden_for_volume) {
                display_phase = 0;
                display_hidden_for_volume = 0;
            }
        } else if (!shadow_control->overtake_mode) {
            /* Let native Move overlays remain visible while volume touch is held. */
            display_phase = 0;
            display_hidden_for_volume = 1;
            return;
        }
    } else if (display_hidden_for_volume) {
        /* Restart shadow slicing cleanly after releasing volume touch. */
        display_phase = 0;
        display_hidden_for_volume = 0;
    }
    if ((ui_check_counter++ % 256) == 0) {
        launch_shadow_ui();
    }

    /* Composite overlays onto shadow display if active */
    static uint8_t shadow_composited[DISPLAY_BUFFER_SIZE];
    const uint8_t *display_src = shadow_display_shm;

    if (skipback_overlay_timeout > 0) {
        skipback_overlay_timeout--;
        shadow_overlay_sync();
    }

    /* Write full display to DISPLAY_OFFSET (768) */
    memcpy(global_mmap_addr + DISPLAY_OFFSET, display_src, DISPLAY_BUFFER_SIZE);

    /* Write display using slice protocol - one slice per ioctl */
    /* No rate limiting because we must overwrite Move every ioctl */

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
        memcpy(global_mmap_addr + 84, display_src + slice_offset, slice_bytes);
    }

    display_phase = (display_phase + 1) % 7;  /* Cycle 0,1,2,3,4,5,6,0,... */
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
        load_feature_config();  /* Load feature flags from config */
        if (shadow_control) {
            shadow_control->display_mirror = display_mirror_enabled ? 1 : 0;
        }
        /* Launch Link Audio subscriber if feature is enabled */
        if (link_audio.enabled) {
            launch_link_subscriber();
            start_link_sub_monitor();
        }
        native_resample_bridge_load_mode_from_shadow_config();  /* Restore bridge mode on Move restart */
#if SHADOW_INPROCESS_POC
        shadow_inprocess_load_chain();
        shadow_dbus_start();  /* Start D-Bus monitoring for volume sync */
        shadow_read_initial_volume();  /* Read initial master volume from settings */
        shadow_load_state();  /* Load saved slot volumes */

        /* Initialize TTS and sync loaded state to shared memory */
        tts_init(44100);
        if (shadow_control) {
            shadow_control->tts_enabled = tts_get_enabled() ? 1 : 0;
            shadow_control->tts_volume = tts_get_volume();
            shadow_control->tts_speed = tts_get_speed();
            shadow_control->tts_pitch = (uint16_t)tts_get_pitch();
            shadow_control->tts_engine = (strcmp(tts_get_engine(), "flite") == 0) ? 1 : 0;
            unified_log("shim", LOG_LEVEL_INFO,
                       "TTS initialized, synced to shared memory: enabled=%s speed=%.2f pitch=%.1f volume=%d",
                       shadow_control->tts_enabled ? "ON" : "OFF",
                       shadow_control->tts_speed, (float)shadow_control->tts_pitch, shadow_control->tts_volume);
        }
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

/* write() hook removed - conflicts with system headers
 * Using send() hook instead for D-Bus interception */

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
        execl(pBinPath, pBinName, pArgs, (char *)0);
        /* execl only returns on error */
        perror("execl failed");
        _exit(1);
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

/* Link Audio subscriber process management */
static volatile int link_sub_started = 0;
static volatile pid_t link_sub_pid = -1;
static volatile uint32_t link_sub_ever_received = 0;  /* high-water mark of packets_intercepted */
static volatile int link_sub_restart_count = 0;       /* total restarts for logging */
static volatile int link_sub_monitor_started = 0;
static volatile int link_sub_monitor_running = 0;
static pthread_t link_sub_monitor_thread;

/* Recovery constants for background monitor thread */
#define LINK_SUB_STALE_THRESHOLD_MS 5000
#define LINK_SUB_WAIT_MS            3000
#define LINK_SUB_COOLDOWN_MS        10000
#define LINK_SUB_ALIVE_CHECK_MS     5000
#define LINK_SUB_MONITOR_POLL_US    100000

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
    /* Guard against PID reuse: verify the process is actually shadow_ui */
    if (!strstr(comm, "shadow_ui")) return 0;
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
    /* Quick check before reap/refresh — if we just forked, trust the state
     * even if /proc hasn't appeared yet (avoids double-fork race). */
    if (shadow_ui_started && shadow_ui_pid > 0) return;
    shadow_ui_reap();
    shadow_ui_refresh_pid();
    if (shadow_ui_started && shadow_ui_pid > 0) return;
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

/* --- Link Audio subscriber process management --- */

static int link_sub_pid_alive(pid_t pid)
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
    if (!strstr(comm, "link-sub")) return 0;
    return 1;
}

static void link_sub_reap(void)
{
    if (link_sub_pid <= 0) return;
    int status = 0;
    pid_t res = waitpid(link_sub_pid, &status, WNOHANG);
    if (res == link_sub_pid) {
        link_sub_pid = -1;
        link_sub_started = 0;
    }
}

static void link_sub_kill(void)
{
    if (link_sub_pid > 0) {
        kill(link_sub_pid, SIGTERM);
    }
}

/* Kill any orphaned link-subscriber processes left from a previous shim instance.
 * This happens when the shim exits (e.g. entering standalone mode) but the
 * detached link-subscriber survives.  On restart, static state is reset so
 * the shim doesn't know about the orphan and would spawn a duplicate. */
static void link_sub_kill_orphans(void)
{
    DIR *dp = opendir("/proc");
    if (!dp) return;
    struct dirent *ent;
    pid_t my_pid = getpid();
    while ((ent = readdir(dp)) != NULL) {
        /* Only look at numeric directory names (PIDs) */
        int pid = atoi(ent->d_name);
        if (pid <= 1) continue;
        if (pid == my_pid) continue;
        if (pid == link_sub_pid) continue;  /* Already tracked */

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int rpid = 0;
        char comm[64] = {0};
        char state = 0;
        int matched = fscanf(f, "%d %63s %c", &rpid, comm, &state);
        fclose(f);
        if (matched != 3) continue;
        if (state == 'Z') continue;
        if (!strstr(comm, "link-sub")) continue;

        /* Found an orphaned link-subscriber */
        unified_log("shim", LOG_LEVEL_INFO,
                    "Killing orphaned link-subscriber pid=%d", pid);
        kill(pid, SIGTERM);
        /* Give it a moment, then force-kill if still alive */
        usleep(50000);  /* 50ms */
        kill(pid, SIGKILL);
        waitpid(pid, NULL, WNOHANG);
    }
    closedir(dp);
}

static void launch_link_subscriber(void)
{
    if (link_sub_started && link_sub_pid > 0) return;
    link_sub_reap();
    if (link_sub_started && link_sub_pid > 0) return;

    /* Kill any orphans from a previous shim instance before spawning */
    link_sub_kill_orphans();

    const char *sub_path = "/data/UserData/move-anything/link-subscriber";
    if (access(sub_path, X_OK) != 0) return;

    /* Write current tempo to file so subscriber uses it instead of 120 */
    {
        float bpm = sampler_get_bpm(NULL);
        FILE *fp = fopen("/tmp/link-tempo", "w");
        if (fp) {
            fprintf(fp, "%.1f\n", bpm);
            fclose(fp);
        }
    }

    int pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        /* Redirect stdout/stderr to log file before closing fds */
        freopen("/tmp/link-subscriber.log", "w", stdout);
        freopen("/tmp/link-subscriber.log", "a", stderr);
        int fdlimit = (int)sysconf(_SC_OPEN_MAX);
        for (int i = STDERR_FILENO + 1; i < fdlimit; i++) {
            close(i);
        }
        /* Clear LD_PRELOAD so shim hooks don't apply to subscriber */
        unsetenv("LD_PRELOAD");
        execl(sub_path, "link-subscriber", (char *)0);
        _exit(1);
    }
    link_sub_started = 1;
    link_sub_pid = pid;
    unified_log("shim", LOG_LEVEL_INFO,
                "Link subscriber launched: pid=%d", pid);
}

static uint64_t link_sub_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void *link_sub_monitor_main(void *arg)
{
    (void)arg;

    uint32_t last_packets = link_audio.packets_intercepted;
    uint64_t last_packet_ms = link_sub_now_ms();
    uint64_t cooldown_until_ms = 0;
    uint64_t kill_deadline_ms = 0;
    uint64_t next_alive_check_ms = last_packet_ms + LINK_SUB_ALIVE_CHECK_MS;
    int kill_pending = 0;

    if (last_packets > link_sub_ever_received) {
        link_sub_ever_received = last_packets;
    }

    while (link_sub_monitor_running) {
        uint64_t now_ms = link_sub_now_ms();

        if (!link_audio.enabled) {
            usleep(LINK_SUB_MONITOR_POLL_US);
            continue;
        }

        uint32_t packets_now = link_audio.packets_intercepted;
        if (packets_now != last_packets) {
            last_packets = packets_now;
            last_packet_ms = now_ms;
            if (packets_now > link_sub_ever_received) {
                link_sub_ever_received = packets_now;
            }
        }

        if (kill_pending) {
            if (now_ms >= kill_deadline_ms) {
                /* Wait period over: reap zombie, reset state, launch fresh */
                link_sub_reap();
                pid_t pid = link_sub_pid;
                if (pid > 0) {
                    kill(pid, SIGKILL);
                    waitpid(pid, NULL, 0);
                    link_sub_pid = -1;
                    link_sub_started = 0;
                }
                kill_pending = 0;
                link_sub_reset_state();
                launch_link_subscriber();
                link_sub_restart_count++;
                cooldown_until_ms = now_ms + LINK_SUB_COOLDOWN_MS;
                last_packets = link_audio.packets_intercepted;
                last_packet_ms = now_ms;
                next_alive_check_ms = now_ms + LINK_SUB_ALIVE_CHECK_MS;
                unified_log("shim", LOG_LEVEL_INFO,
                            "Link subscriber restarted after stale detection (restart #%d)",
                            (int)link_sub_restart_count);
            }
            usleep(LINK_SUB_MONITOR_POLL_US);
            continue;
        }

        if (link_sub_ever_received > 0 &&
            now_ms > last_packet_ms + LINK_SUB_STALE_THRESHOLD_MS &&
            now_ms >= cooldown_until_ms) {
            /* Audio was flowing but stopped for ~5s — trigger recovery */
            pid_t pid = link_sub_pid;
            unified_log("shim", LOG_LEVEL_INFO,
                        "Link audio stale detected: la_stale=%u la_ever=%u, killing subscriber pid=%d",
                        la_stale_frames, link_sub_ever_received, (int)pid);
            link_sub_kill();
            kill_pending = 1;
            kill_deadline_ms = now_ms + LINK_SUB_WAIT_MS;
            usleep(LINK_SUB_MONITOR_POLL_US);
            continue;
        }

        if (now_ms >= next_alive_check_ms) {
            next_alive_check_ms = now_ms + LINK_SUB_ALIVE_CHECK_MS;
            link_sub_reap();
            pid_t pid = link_sub_pid;
            if (link_sub_started && !link_sub_pid_alive(pid) &&
                now_ms >= cooldown_until_ms) {
                /* Subscriber crashed — restart */
                unified_log("shim", LOG_LEVEL_INFO,
                            "Link subscriber died (pid=%d), restarting",
                            (int)pid);
                link_sub_pid = -1;
                link_sub_started = 0;
                link_sub_reset_state();
                launch_link_subscriber();
                link_sub_restart_count++;
                cooldown_until_ms = now_ms + LINK_SUB_COOLDOWN_MS;
                last_packets = link_audio.packets_intercepted;
                last_packet_ms = now_ms;
            }
        }

        usleep(LINK_SUB_MONITOR_POLL_US);
    }

    return NULL;
}

static void start_link_sub_monitor(void)
{
    if (link_sub_monitor_started) return;

    link_sub_monitor_running = 1;
    int rc = pthread_create(&link_sub_monitor_thread, NULL, link_sub_monitor_main, NULL);
    if (rc != 0) {
        link_sub_monitor_running = 0;
        unified_log("shim", LOG_LEVEL_WARN,
                    "Link subscriber monitor start failed: %s",
                    strerror(rc));
        return;
    }

    pthread_detach(link_sub_monitor_thread);
    link_sub_monitor_started = 1;
    unified_log("shim", LOG_LEVEL_INFO, "Link subscriber monitor started");
}

static void link_sub_reset_state(void)
{
    /* Reset Link Audio state so fresh subscriber can rediscover everything */
    link_audio.packets_intercepted = 0;
    link_audio.session_parsed = 0;
    link_audio.move_channel_count = 0;
    link_sub_ever_received = 0;
    la_prev_intercepted = 0;
    la_stale_frames = 0;

    /* Clear ring buffers */
    for (int i = 0; i < LINK_AUDIO_MOVE_CHANNELS; i++) {
        link_audio.channels[i].write_pos = 0;
        link_audio.channels[i].read_pos = 0;
        link_audio.channels[i].active = 0;
        link_audio.channels[i].pkt_count = 0;
        link_audio.channels[i].peak = 0;
    }
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

    uint8_t *src = (hardware_mmap_addr ? hardware_mmap_addr : global_mmap_addr) + MIDI_IN_OFFSET;

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

        /* OLD standalone launch code - check feature flag */
        if (shiftHeld && volumeTouched && knob8touched && !alreadyLaunched && standalone_enabled)
        {
            alreadyLaunched = 1;
            printf("Launching Move Anything!\n");
            link_sub_kill();
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

    /* === IOCTL GAP DETECTION (always-on, no flag needed) === */
    {
        static struct timespec last_ioctl_time = {0, 0};
        if (last_ioctl_time.tv_sec > 0) {
            uint64_t gap_ms = (ioctl_start.tv_sec - last_ioctl_time.tv_sec) * 1000 +
                              (ioctl_start.tv_nsec - last_ioctl_time.tv_nsec) / 1000000;
            if (gap_ms > 1000) {
                char gap_msg[64];
                snprintf(gap_msg, sizeof(gap_msg), "Ioctl gap: %lu ms", (unsigned long)gap_ms);
                unified_log_crash(gap_msg);
            }
        }
        last_ioctl_time = ioctl_start;
    }

    /* === HEARTBEAT (every ~5700 frames / ~100s) === */
    {
        static uint32_t heartbeat_counter = 0;
        heartbeat_counter++;
        if (heartbeat_counter >= 5700) {
            heartbeat_counter = 0;
            if (unified_log_enabled()) {
                /* Read VmRSS from /proc/self/statm */
                long rss_pages = 0;
                FILE *statm = fopen("/proc/self/statm", "r");
                if (statm) {
                    long size;
                    if (fscanf(statm, "%ld %ld", &size, &rss_pages) != 2)
                        rss_pages = 0;
                    fclose(statm);
                }
                long rss_kb = rss_pages * 4;  /* 4KB pages on ARM */

                int ui_alive = shadow_ui_pid_alive(shadow_ui_pid);
                unified_log("shim", LOG_LEVEL_DEBUG,
                    "Heartbeat: pid=%d rss=%ldKB overruns=%d shadow_ui_pid=%d(alive=%d) display_mode=%d la_pkts=%u la_ch=%d la_stale=%u la_sub_pid=%d la_restarts=%d pin=%d/%d",
                    getpid(), rss_kb, consecutive_overruns,
                    (int)shadow_ui_pid, ui_alive, shadow_display_mode,
                    link_audio.packets_intercepted, link_audio.move_channel_count,
                    la_stale_frames, (int)link_sub_pid, link_sub_restart_count,
                    shadow_control ? shadow_control->pin_challenge_active : -1, pin_state);
            }
        }
    }

    /* === SET DETECTION (poll every ~3s / ~1000 frames) === */
    {
        static uint32_t set_poll_counter = 0;
        set_poll_counter++;
        if (set_poll_counter >= 500) {  /* ~1.5s at 44100/128 */
            set_poll_counter = 0;
            shadow_poll_current_set();
        }
    }

    /* Link subscriber stale/restart recovery runs in a background monitor thread
     * to keep process management and waitpid() out of this real-time path. */
    if (link_audio.enabled) {
        uint32_t la_pkts_now = link_audio.packets_intercepted;
        if (la_pkts_now > link_sub_ever_received) {
            link_sub_ever_received = la_pkts_now;
        }
    }

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

    /* Drain MIDI-to-DSP from shadow UI (overtake modules sending to chain slots) */
    shadow_drain_ui_midi_dsp();

    /* Pre-ioctl: Mix from pre-rendered buffer (FAST, ~5µs)
     * DSP was rendered post-ioctl in the previous frame.
     * This adds ~3ms latency but lets Move process pad events faster.
     */
    static uint64_t mix_time_sum = 0;
    static int mix_time_count = 0;
    static uint64_t mix_time_max = 0;

    /* Always run pre-ioctl mix/capture path.
     * This path is lightweight and feeds native bridge state; skipping it causes
     * stale/invalid bridge snapshots and inconsistent resample behavior. */
    {
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

    /* Mix TTS audio AFTER inprocess mix (which may zero-rebuild mailbox for Link Audio) */
    shadow_mix_tts();

    /* Signal Link Audio publisher thread to drain accumulated audio */
    if (link_audio.publisher_running) {
        link_audio.publisher_tick = 1;
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

    /* Native Move display is visible either when shadow mode is off, or when
     * plain volume-touch temporarily hides shadow UI to reveal Move overlays. */
    int native_display_visible = (!shadow_display_mode) ||
                                 (shadow_display_mode &&
                                  shadow_volume_knob_touched &&
                                  !shadow_shift_held &&
                                  shadow_control &&
                                  !shadow_control->overtake_mode);

    if (global_mmap_addr && native_display_visible) {
        uint8_t *mem = (uint8_t *)global_mmap_addr;
        uint8_t slice_num = mem[80];

        /* Always capture incoming slices */
        if (slice_num >= 1 && slice_num <= 6) {
            int idx = slice_num - 1;
            int bytes = (idx == 5) ? 164 : 172;
            memcpy(captured_slices[idx], mem + 84, 172);
            slice_fresh[idx] = 1;

            /* Always accumulate into PIN display buffer for dump trigger */
            pin_accumulate_slice(idx, mem + 84, bytes);
        }

        /* When volume knob touched (and no track held), start capturing */
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

                /* Find the volume position indicator in the gap between VU bars.
                 * Rows 30-32 are blank on the volume overlay except for the 1-pixel
                 * vertical indicator.  Require: vertical alignment on rows 30+31+32
                 * at the same column AND the gap rows are otherwise blank (total lit
                 * pixels across all three rows <= 6).  Waveform screens have many
                 * scattered pixels on these rows. */
                int bar_col = -1;
                int gap_total_lit = 0;
                {
                    int page3 = 30 / 8;  /* page 3 for rows 30-31 */
                    int page4 = 32 / 8;  /* page 4 for row 32 */
                    int bit30 = 30 % 8;
                    int bit31 = 31 % 8;
                    int bit32 = 32 % 8;
                    for (int col = 0; col < 128; col++) {
                        int l30 = !!(full_display[page3 * 128 + col] & (1 << bit30));
                        int l31 = !!(full_display[page3 * 128 + col] & (1 << bit31));
                        int l32 = !!(full_display[page4 * 128 + col] & (1 << bit32));
                        gap_total_lit += l30 + l31 + l32;
                        if (l30 && l31 && l32 && bar_col < 0)
                            bar_col = col;
                    }
                }

                if (bar_col >= 0 && gap_total_lit <= 6) {
                    float normalized = (float)(bar_col - 4) / (122.0f - 4.0f);
                    if (normalized < 0.0f) normalized = 0.0f;
                    if (normalized > 1.0f) normalized = 1.0f;

                    /* Map pixel bar position to amplitude matching Move's volume curve.
                     * sqrt model: dB = -70 * (1 - sqrt(pos))
                     * Measured from Move's Settings.json globalVolume:
                     *   pos 0.25 → -33.2 dB (model: -35.0)
                     *   pos 0.50 → -19.9 dB (model: -20.5)
                     *   pos 0.75 → -10.4 dB (model:  -9.4)
                     *   pos 1.00 →   0.0 dB (model:   0.0) */
                    float amplitude;
                    if (normalized <= 0.0f) {
                        amplitude = 0.0f;
                    } else if (normalized >= 1.0f) {
                        amplitude = 1.0f;
                    } else {
                        float db = -70.0f * (1.0f - sqrtf(normalized));
                        amplitude = powf(10.0f, db / 20.0f);
                    }

                    if (fabsf(amplitude - shadow_master_volume) > 0.003f) {
                        shadow_master_volume = amplitude;
                        float db_val = (amplitude > 0.0f) ? (20.0f * log10f(amplitude)) : -99.0f;
                        char msg[112];
                        snprintf(msg, sizeof(msg), "Master volume: x=%d pos=%.3f dB=%.1f amp=%.4f", bar_col, normalized, db_val, amplitude);
                        shadow_log(msg);
                    }
                }

                memset(slice_fresh, 0, 6);  /* Reset for next capture */
                volume_capture_cooldown = 12;  /* ~2 display frames between reads */
            }
        } else {
            volume_capture_active = 0;
            volume_capture_warmup = 0;  /* Reset warmup for next touch */
        }

        if (volume_capture_cooldown > 0) volume_capture_cooldown--;

        /* === OVERLAY COMPOSITING ===
         * JS sets display_overlay in shadow_control_t:
         *   0 = off (normal native display)
         *   1 = rect overlay (blit rect from shadow display onto native)
         *   2 = fullscreen (replace native display with shadow display)
         * All overlays (sampler, skipback, shift+knob) are JS-rendered. */
        int shift_knob_overlay_on = (shift_knob_overlay_active && shift_knob_overlay_timeout > 0);
        int sampler_overlay_on = (sampler_overlay_active &&
                                  (sampler_state != SAMPLER_IDLE || sampler_overlay_timeout > 0));
        int sampler_fullscreen_on = (sampler_fullscreen_active &&
                                     (sampler_state != SAMPLER_IDLE || sampler_overlay_timeout > 0));
        int skipback_overlay_on = (skipback_overlay_timeout > 0);

        /* Read JS display_overlay request */
        uint8_t disp_overlay = shadow_control ? shadow_control->display_overlay : 0;

        int any_overlay = shift_knob_overlay_on || sampler_overlay_on ||
                          sampler_fullscreen_on || skipback_overlay_on || disp_overlay;
        if (any_overlay && slice_num >= 1 && slice_num <= 6) {
            static uint8_t overlay_display[1024];
            static int overlay_frame_ready = 0;

            if (slice_num == 1) {
                /* Track MIDI clock staleness (once per frame) */
                if (sampler_clock_active) {
                    sampler_clock_stale_frames++;
                    if (sampler_clock_stale_frames > SAMPLER_CLOCK_STALE_THRESHOLD) {
                        sampler_clock_active = 0;
                        sampler_clock_stale_frames = 0;
                    }
                }

                /* Update VU / sync for sampler when active */
                if (sampler_fullscreen_on || sampler_overlay_on) {
                    sampler_update_vu();
                    shadow_overlay_sync();
                }

                if (disp_overlay == 2 && shadow_display_shm) {
                    /* JS fullscreen: replace native display with shadow display */
                    memcpy(overlay_display, shadow_display_shm, 1024);
                    overlay_frame_ready = 1;
                } else if (disp_overlay == 1 && shadow_display_shm && shadow_control) {
                    /* JS rect overlay: reconstruct native, blit shadow rect on top */
                    int all_present = 1;
                    for (int i = 0; i < 6; i++) {
                        if (!slice_fresh[i]) all_present = 0;
                    }
                    if (all_present) {
                        for (int s = 0; s < 6; s++) {
                            int offset = s * 172;
                            int bytes = (s == 5) ? 164 : 172;
                            memcpy(overlay_display + offset, captured_slices[s], bytes);
                        }
                        overlay_blit_rect(overlay_display, shadow_display_shm,
                                          shadow_control->overlay_rect_x,
                                          shadow_control->overlay_rect_y,
                                          shadow_control->overlay_rect_w,
                                          shadow_control->overlay_rect_h);
                        overlay_frame_ready = 1;
                    }
                } else if (!disp_overlay) {
                    overlay_frame_ready = 0;
                }

                /* Decrement timeouts once per frame */
                if (shift_knob_overlay_on) {
                    shift_knob_overlay_timeout--;
                    if (shift_knob_overlay_timeout <= 0) {
                        shift_knob_overlay_active = 0;
                        shadow_overlay_sync();
                    }
                }
                if ((sampler_overlay_on || sampler_fullscreen_on) && sampler_state == SAMPLER_IDLE) {
                    sampler_overlay_timeout--;
                    if (sampler_overlay_timeout <= 0) {
                        sampler_overlay_active = 0;
                        sampler_fullscreen_active = 0;
                        shadow_overlay_sync();
                    }
                }
                if (skipback_overlay_on) {
                    skipback_overlay_timeout--;
                    if (skipback_overlay_timeout <= 0)
                        shadow_overlay_sync();
                }

                if (!any_overlay)
                    overlay_frame_ready = 0;
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

    /* Capture final display to live shm for remote viewer.
     * Shadow mode: copy from shadow display shm (full composited frame).
     * Native mode: reconstruct from captured slices (written above). */
    if (display_live_shm && shadow_control && shadow_control->display_mirror) {
        if (shadow_display_mode && shadow_display_shm) {
            memcpy(display_live_shm, shadow_display_shm, DISPLAY_BUFFER_SIZE);
        } else {
            static uint8_t live_native[DISPLAY_BUFFER_SIZE];
            static int live_slice_seen[6] = {0};
            uint8_t cur_slice = global_mmap_addr ? ((uint8_t *)global_mmap_addr)[80] : 0;
            if (cur_slice >= 1 && cur_slice <= 6) {
                int idx = cur_slice - 1;
                int bytes = (idx == 5) ? 164 : 172;
                memcpy(live_native + idx * 172, (uint8_t *)global_mmap_addr + 84, bytes);
                live_slice_seen[idx] = 1;
                /* On last slice, push full frame */
                if (cur_slice == 6) {
                    int all = 1;
                    for (int i = 0; i < 6; i++) { if (!live_slice_seen[i]) all = 0; }
                    if (all) {
                        memcpy(display_live_shm, live_native, DISPLAY_BUFFER_SIZE);
                        memset(live_slice_seen, 0, sizeof(live_slice_seen));
                    }
                }
            }
        }
    }

    /* === PIN CHALLENGE SCANNER ===
     * Check if a web PIN challenge is active and speak the digits. */
    pin_check_and_speak();

    /* Mark end of pre-ioctl processing */
    clock_gettime(CLOCK_MONOTONIC, &pre_end);

do_ioctl:
    /* In baseline mode, pre_end wasn't set - set it now */
    if (baseline_mode) clock_gettime(CLOCK_MONOTONIC, &pre_end);

    /* === SHADOW UI MIDI OUT (PRE-IOCTL) ===
     * Inject any MIDI from shadow UI into the mailbox before sync.
     * In overtake mode, also clears Move's cable 0 packets when shadow has new data. */
    shadow_clear_move_leds_if_overtake();  /* Free buffer space before inject */
    shadow_inject_ui_midi_out();
    shadow_flush_pending_leds();  /* Rate-limited LED output */

    /* === SCREEN READER ANNOUNCEMENTS ===
     * Check for and send accessibility announcements via D-Bus. */
    shadow_check_screenreader_announcements();

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

        /* Bridge Move Everything's total mix into native resampling path when selected. */
        native_resample_bridge_apply();

        /* Capture audio for sampler post-ioctl (Move Input source only - fresh hardware input) */
        if (sampler_source == SAMPLER_SOURCE_MOVE_INPUT) {
            sampler_capture_audio();
            /* Skipback: always capture Move Input source into rolling buffer */
            skipback_init();
            skipback_capture((int16_t *)(hardware_mmap_addr + AUDIO_IN_OFFSET));
        }

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
                    /* Overtake mode split:
                     * - mode 2 (module): block all cable 0 events from Move
                     * - mode 1 (menu): allow only volume touch/turn passthrough */
                    if (overtake_mode == 2) {
                        filter = 1;
                    } else if (overtake_mode == 1) {
                        filter = 1;
                        if (cin == 0x0B && type == 0xB0 && d1 == CC_MASTER_KNOB) {
                            filter = 0;
                        }
                        if ((cin == 0x09 || cin == 0x08) &&
                            (type == 0x90 || type == 0x80) &&
                            d1 == 8) {
                            filter = 0;
                        }
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
                        /* Note messages: filter knob touches (0-7,9).
                         * Keep note 8 (volume touch) so Move can do track+volume
                         * and native volume workflows while shadow UI is active. */
                        if ((cin == 0x09 || cin == 0x08) && (type == 0x90 || type == 0x80)) {
                            if (d1 <= 7 || d1 == 9) {
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

        /* === SHIFT+MENU SHORTCUT DETECTION AND BLOCKING (POST-IOCTL) ===
         * Scan hardware MIDI_IN for Shift+Menu, perform action, and block from reaching Move.
         * This works regardless of shadow_display_mode.
         * Skip entirely in overtake mode - overtake module owns all input. */
        if (overtake_mode) goto skip_shift_menu;
        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            uint8_t cin = hw_midi[j] & 0x0F;
            uint8_t cable = (hw_midi[j] >> 4) & 0x0F;
            if (cable != 0x00) continue;  /* Only internal cable */
            if (cin == 0x0B) {  /* Control Change */
                uint8_t d1 = hw_midi[j + 2];
                uint8_t d2 = hw_midi[j + 3];

                /* Shift + Menu: single press = Master FX / screen reader settings
                 *                double press = toggle screen reader on/off
                 * First press is deferred 400ms to detect double-click. */
                /* Block Menu CC entirely when Shift is held (both press and release) */
                if (d1 == CC_MENU && shadow_shift_held) {
                    if (d2 > 0 && shadow_control) {
                        struct timespec sm_ts;
                        clock_gettime(CLOCK_MONOTONIC, &sm_ts);
                        uint64_t sm_now = (uint64_t)(sm_ts.tv_sec * 1000) + (sm_ts.tv_nsec / 1000000);

                        if (shift_menu_pending && (sm_now - shift_menu_pending_ms) < 300) {
                            /* Double-click: toggle screen reader */
                            shift_menu_pending = 0;
                            uint8_t was_on = shadow_control->tts_enabled;
                            shadow_control->tts_enabled = was_on ? 0 : 1;
                            tts_set_enabled(!was_on);
                            tts_speak(was_on ? "Screen reader off" : "Screen reader on");
                            shadow_log(was_on ? "Shift+Menu double-click: screen reader OFF"
                                              : "Shift+Menu double-click: screen reader ON");
                        } else {
                            /* First press: defer action */
                            shift_menu_pending = 1;
                            shift_menu_pending_ms = sm_now;
                        }
                    }
                    /* Block Menu CC from reaching Move by zeroing in shadow buffer */
                    char block_msg[128];
                    snprintf(block_msg, sizeof(block_msg), "Blocking Menu CC (POST-IOCTL d2=%d)", d2);
                    shadow_log(block_msg);
                    sh_midi[j] = 0;
                    sh_midi[j + 1] = 0;
                    sh_midi[j + 2] = 0;
                    sh_midi[j + 3] = 0;
                }
            }
        }
        skip_shift_menu:

        /* Deferred Shift+Menu single-press action (fires 400ms after first press if no double-click) */
        if (shift_menu_pending && shadow_control) {
            struct timespec sm_ts2;
            clock_gettime(CLOCK_MONOTONIC, &sm_ts2);
            uint64_t sm_now2 = (uint64_t)(sm_ts2.tv_sec * 1000) + (sm_ts2.tv_nsec / 1000000);
            if (sm_now2 - shift_menu_pending_ms >= 300) {
                shift_menu_pending = 0;
                char log_msg[128];
                snprintf(log_msg, sizeof(log_msg), "Shift+Menu single-press (deferred), shadow_ui_enabled=%s",
                         shadow_ui_enabled ? "true" : "false");
                shadow_log(log_msg);

                if (shadow_ui_enabled) {
                    if (!shadow_display_mode) {
                        shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_MASTER_FX;
                        shadow_display_mode = 1;
                        shadow_control->display_mode = 1;
                        launch_shadow_ui();
                    } else {
                        shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_MASTER_FX;
                    }
                } else {
                    shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_SCREENREADER;
                    shadow_display_mode = 1;
                    shadow_control->display_mode = 1;
                    launch_shadow_ui();
                }
            }
        }

        /* === SAMPLER MIDI FILTERING ===
         * Block events from reaching Move for sampler use.
         * Always block Shift+Record so the first press doesn't leak through.
         * Block jog while sampler is armed or recording. */
        {
            for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
                uint8_t cin = sh_midi[j] & 0x0F;
                uint8_t cable = (sh_midi[j] >> 4) & 0x0F;
                if (cable != 0x00) continue;
                uint8_t s_type = sh_midi[j + 1] & 0xF0;
                uint8_t s_d1 = sh_midi[j + 2];

                if (cin == 0x0B && s_type == 0xB0) {
                    /* Block Record (CC 118) from Move: always when Shift held,
                     * and also when sampler is non-idle (armed or recording) */
                    if (s_d1 == CC_RECORD && (shadow_shift_held || sampler_state != SAMPLER_IDLE)) {
                        sh_midi[j] = 0; sh_midi[j+1] = 0; sh_midi[j+2] = 0; sh_midi[j+3] = 0;
                    }
                    /* Block Shift+Capture from reaching Move */
                    if (s_d1 == CC_CAPTURE && shadow_shift_held) {
                        sh_midi[j] = 0; sh_midi[j+1] = 0; sh_midi[j+2] = 0; sh_midi[j+3] = 0;
                    }
                    /* Block jog, back while sampler is armed or recording */
                    if (sampler_state != SAMPLER_IDLE) {
                        if (s_d1 == CC_JOG_WHEEL || s_d1 == CC_JOG_CLICK || s_d1 == CC_BACK) {
                            sh_midi[j] = 0; sh_midi[j+1] = 0; sh_midi[j+2] = 0; sh_midi[j+3] = 0;
                        }
                    }
                }
            }
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
        int overtake_active = shadow_control ? shadow_control->overtake_mode : 0;
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
                /* In overtake mode, skip all shortcuts except Shift+Vol+Jog Click (exit) */
                if (overtake_active && !(d1 == CC_JOG_CLICK && shadow_shift_held && shadow_volume_knob_touched)) {
                    continue;
                }
                /* DEBUG: log CCs while shift held */
                if (shadow_shift_held && d2 > 0) {
                    char dbg[64];
                    snprintf(dbg, sizeof(dbg), "Shift+CC: cc=%d val=%d", d1, d2);
                    shadow_log(dbg);
                }
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

                        /* Mute + Track = toggle shadow slot mute */
                        if (shadow_mute_held) {
                            shadow_apply_mute(new_slot, !shadow_chain_slots[new_slot].muted);
                        }

                        /* Shift + Volume + Track = jump to that slot's edit screen (if shadow UI enabled) */
                        if (shadow_shift_held && shadow_volume_knob_touched && shadow_control && shadow_ui_enabled) {
                            shadow_block_plain_volume_hide_until_release = 1;
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

                /* Mute button (CC 88): track held state */
                if (d1 == CC_MUTE) {
                    shadow_mute_held = (d2 > 0) ? 1 : 0;
                }

                /* Shift + Volume + Jog Click = toggle overtake module menu (if shadow UI enabled) */
                if (d1 == CC_JOG_CLICK && d2 > 0) {
                    if (shadow_shift_held && shadow_volume_knob_touched && shadow_control && shadow_ui_enabled) {
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

                /* Shift+Capture: save skipback buffer */
                if (d1 == CC_CAPTURE && d2 > 0 && shadow_shift_held) {
                    skipback_trigger_save();
                    src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                }

                /* Sample/Record button (CC 118) - sampler intercept */
                if (d1 == CC_RECORD && d2 > 0) {
                    if (shadow_shift_held) {
                        /* Shift+Sample: arm/cancel/force-stop */
                        if (sampler_state == SAMPLER_IDLE && !shadow_display_mode) {
                            sampler_state = SAMPLER_ARMED;
                            sampler_overlay_active = 1;
                            sampler_overlay_timeout = 0;
                            sampler_fullscreen_active = 1;
                            sampler_menu_cursor = SAMPLER_MENU_SOURCE;
                            shadow_overlay_sync();
                            shadow_log("Sampler: ARMED");
                            {
                                char sr_buf[256];
                                const char *src = (sampler_source == SAMPLER_SOURCE_RESAMPLE)
                                    ? "Resample" : "Move Input";
                                snprintf(sr_buf, sizeof(sr_buf),
                                    "Quantized Sampler. Source: %s. "
                                    "Press play or a pad to begin recording.",
                                    src);
                                send_screenreader_announcement(sr_buf);
                            }
                        } else if (sampler_state == SAMPLER_ARMED) {
                            sampler_state = SAMPLER_IDLE;
                            sampler_overlay_active = 0;
                            sampler_fullscreen_active = 0;
                            shadow_overlay_sync();
                            shadow_log("Sampler: cancelled");
                            send_screenreader_announcement("Sampler cancelled");
                        } else if (sampler_state == SAMPLER_RECORDING) {
                            shadow_log("Sampler: force stop via Shift+Sample");
                            sampler_stop_recording();
                        }
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    } else if (sampler_state == SAMPLER_RECORDING) {
                        /* Bare Sample while recording: stop */
                        shadow_log("Sampler: stopped via Sample button");
                        sampler_stop_recording();
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    }
                }

                /* Back button while sampler is armed = exit */
                if (d1 == CC_BACK && d2 > 0 && sampler_state == SAMPLER_ARMED) {
                    sampler_state = SAMPLER_IDLE;
                    sampler_overlay_active = 0;
                    sampler_fullscreen_active = 0;
                    shadow_overlay_sync();
                    shadow_log("Sampler: cancelled via Back");
                    send_screenreader_announcement("Sampler cancelled");
                    src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                }

                /* Jog wheel while sampler is armed = navigate menu */
                if (d1 == CC_JOG_WHEEL && sampler_state == SAMPLER_ARMED) {
                    /* Decode relative value: 1-63=CW, 65-127=CCW */
                    if (d2 >= 1 && d2 <= 63) {
                        if (sampler_menu_cursor < SAMPLER_MENU_COUNT - 1)
                            sampler_menu_cursor++;
                    } else if (d2 >= 65 && d2 <= 127) {
                        if (sampler_menu_cursor > 0)
                            sampler_menu_cursor--;
                    }
                    shadow_overlay_sync();
                    sampler_announce_menu_item();
                    /* Block jog from reaching Move/shadow UI */
                    src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                }

                /* Jog click while sampler is armed = cycle selected menu item */
                if (d1 == CC_JOG_CLICK && d2 > 0 && sampler_state == SAMPLER_ARMED) {
                    if (sampler_menu_cursor == SAMPLER_MENU_SOURCE) {
                        sampler_source = (sampler_source == SAMPLER_SOURCE_RESAMPLE)
                            ? SAMPLER_SOURCE_MOVE_INPUT : SAMPLER_SOURCE_RESAMPLE;
                    } else if (sampler_menu_cursor == SAMPLER_MENU_DURATION) {
                        sampler_duration_index = (sampler_duration_index + 1) % SAMPLER_DURATION_COUNT;
                    }
                    shadow_overlay_sync();
                    sampler_announce_menu_item();
                    src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                }
            }

            /* Note On/Off messages (CIN 0x09/0x08) for knob touches and step buttons */
            if ((cin == 0x09 || cin == 0x08) && (type == 0x90 || type == 0x80)) {
                int touched = (type == 0x90 && d2 > 0);

                /* Volume knob touch (note 8) */
                if (d1 == 8) {
                    if (touched != shadow_volume_knob_touched) {
                        shadow_volume_knob_touched = touched;
                        volumeTouched = touched;
                        if (!touched) {
                            shadow_block_plain_volume_hide_until_release = 0;
                        }
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Volume knob touch: %s", touched ? "ON" : "OFF");
                        shadow_log(msg);
                    }
                }

                /* Jog encoder touch (note 9) */
                if (d1 == 9) {
                    shadow_jog_touched = touched;
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

                /* Shift + Volume + Knob8 = launch standalone Move Anything (if enabled) */
                if (shadow_shift_held && shadow_volume_knob_touched && knob8touched && !alreadyLaunched && standalone_enabled) {
                    alreadyLaunched = 1;
                    shadow_log("Launching Move Anything (Shift+Vol+Knob8)!");
                    link_sub_kill();
                    launchChildAndKillThisProcess("/data/UserData/move-anything/start.sh", "start.sh", "");
                }

                /* Shift + Volume + Step 2 (note 17) = jump to Global Settings */
                if (d1 == 17 && type == 0x90 && d2 > 0) {
                    if (shadow_shift_held && shadow_volume_knob_touched && shadow_control && shadow_ui_enabled) {
                        shadow_block_plain_volume_hide_until_release = 1;
                        shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_SETTINGS;
                        /* Always ensure display shows shadow UI */
                        shadow_display_mode = 1;
                        shadow_control->display_mode = 1;
                        launch_shadow_ui();  /* No-op if already running */
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    }
                }

                /* Pad note-on while sampler armed = trigger recording */
                if (type == 0x90 && d2 > 0 && d1 >= 68 && d1 <= 99 &&
                    sampler_state == SAMPLER_ARMED) {
                    shadow_log("Sampler: triggered by pad note-on");
                    sampler_start_recording();
                    /* Do NOT block the note - it must play so it gets recorded */
                }
            }
        }

        /* External MIDI trigger (cable 2): any note-on triggers recording when armed */
        if (sampler_state == SAMPLER_ARMED) {
            for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
                uint8_t cable = (src[j] >> 4) & 0x0F;
                uint8_t cin = src[j] & 0x0F;
                if (cable != 0x02) continue;
                if (cin == 0x09) {  /* Note-on */
                    uint8_t vel = src[j + 3];
                    if (vel > 0) {
                        shadow_log("Sampler: triggered by external MIDI (cable 2)");
                        sampler_start_recording();
                        /* Do NOT block - let note pass through for playback/recording */
                        break;
                    }
                }
            }
        }
    }

    /* === POST-IOCTL: OVERLAY KNOB INTERCEPTION (MOVE MODE) ===
     * When in Move mode (not shadow mode) and the overlay activation condition is met,
     * intercept knob CCs (71-78) and route to shadow chain DSP.
     * Also block knob touch notes (0-7) to prevent them reaching Move.
     * Activation depends on overlay_knobs_mode: Shift (0), Jog Touch (1), or Off (2). */
    uint8_t overlay_knobs_mode = shadow_control ? shadow_control->overlay_knobs_mode : OVERLAY_KNOBS_SHIFT;
    int overlay_active = 0;
    if (overlay_knobs_mode == OVERLAY_KNOBS_SHIFT) overlay_active = shiftHeld;
    else if (overlay_knobs_mode == OVERLAY_KNOBS_JOG_TOUCH) overlay_active = shadow_jog_touched;

    if (!shadow_display_mode && overlay_active && shadow_ui_enabled &&
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
                        shadow_overlay_sync();
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

                /* Queue cable 2 note-on messages (external LED commands like M8)
                 * for rate-limited forwarding to prevent buffer overflow */
                if (cable == 0x02 && type == 0x90) {
                    shadow_queue_input_led(src[j], status, d1, d2);
                    continue;
                }

                /* All other messages: forward directly */
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

                /* Broadcast internal MIDI to ALL active slots for audio FX (e.g. ducker).
                 * FX_BROADCAST only forwards to audio FX, not synth/MIDI FX, so this
                 * is safe even for the focused slot that received normal dispatch. */
                if (d1 >= 10 && shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                    for (int si = 0; si < SHADOW_CHAIN_INSTANCES; si++) {
                        if (!shadow_chain_slots[si].active || !shadow_chain_slots[si].instance)
                            continue;
                        uint8_t msg[3] = { status, d1, d2 };
                        shadow_plugin_v2->on_midi(shadow_chain_slots[si].instance, msg, 3,
                                                  MOVE_MIDI_SOURCE_FX_BROADCAST);
                    }
                }

                /* Forward note events to master FX (e.g. ducker) */
                if (d1 >= 10) {
                    uint8_t msg[3] = { status, d1, d2 };
                    shadow_master_fx_forward_midi(msg, 3, MOVE_MIDI_SOURCE_INTERNAL);
                }
                continue;
            }
        }

        /* Flush pending input LED queue (for cable 2 external MIDI in overtake mode) */
        shadow_flush_pending_input_leds();
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

    /* === POST-IOCTL: CHECK FOR RESTART REQUEST === */
    /* Shadow UI can request a Move restart (e.g. after core update) */
    if (shadow_control && shadow_control->restart_move) {
        shadow_control->restart_move = 0;
        shadow_control->should_exit = 1;  /* Tell shadow_ui to exit */
        shadow_log("Restart requested by shadow UI — restarting Move");
        /* Use restart script for clean restart (kill as root, start fresh).
         * launchChildAndKillThisProcess won't work because MoveOriginal has
         * file capabilities that trigger AT_SECURE, blocking LD_PRELOAD
         * when forked from a non-root process. */
        system("/data/UserData/move-anything/restart-move.sh");
    }

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
