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

#include "host/plugin_api_v1.h"

/* Debug flags - set to 1 to enable various debug logging */
#define SHADOW_DEBUG 0           /* Master debug flag for mailbox/MIDI debug */
#define SHADOW_TRACE_DEBUG 0     /* SPI/MIDI trace logging */

unsigned char *global_mmap_addr = NULL;
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
#define MIDI_BUFFER_SIZE 256
#define DISPLAY_BUFFER_SIZE 1024   /* 128x64 @ 1bpp = 1024 bytes */
#define CONTROL_BUFFER_SIZE 64
#define SHADOW_UI_BUFFER_SIZE 512
#define SHADOW_PARAM_BUFFER_SIZE 512
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

typedef struct shadow_control_t {
    volatile uint8_t display_mode;    /* 0=stock Move, 1=shadow display */
    volatile uint8_t shadow_ready;    /* 1 when shadow process is running */
    volatile uint8_t should_exit;     /* 1 to signal shadow to exit */
    volatile uint8_t midi_ready;      /* increments when new MIDI available */
    volatile uint8_t write_idx;       /* triple buffer: shadow writes here */
    volatile uint8_t read_idx;        /* triple buffer: shim reads here */
    volatile uint8_t ui_slot;         /* shadow UI slot selection (0-3) */
    volatile uint8_t ui_flags;        /* reserved for UI flags */
    volatile uint16_t ui_patch_index; /* shadow UI patch index */
    volatile uint16_t reserved16;     /* padding/alignment */
    volatile uint32_t ui_request_id;  /* increment to request patch change */
    volatile uint32_t shim_counter;   /* increments each ioctl for drift correction */
    volatile uint8_t reserved[44];    /* padding for future use */
} shadow_control_t;

static shadow_control_t *shadow_control = NULL;
static uint8_t shadow_display_mode = 0;
typedef char shadow_control_size_check[(sizeof(shadow_control_t) == CONTROL_BUFFER_SIZE) ? 1 : -1];

#define SHADOW_UI_NAME_LEN 64
#define SHADOW_UI_SLOTS 4
typedef struct shadow_ui_state_t {
    uint32_t version;
    uint8_t slot_count;
    uint8_t reserved[3];
    uint8_t slot_channels[SHADOW_UI_SLOTS];
    uint8_t slot_volumes[SHADOW_UI_SLOTS];       /* 0-100 percentage */
    int8_t slot_forward_ch[SHADOW_UI_SLOTS];     /* -1=none, 0-15=channel */
    char slot_names[SHADOW_UI_SLOTS][SHADOW_UI_NAME_LEN];
} shadow_ui_state_t;

static shadow_ui_state_t *shadow_ui_state = NULL;
typedef char shadow_ui_state_size_check[(sizeof(shadow_ui_state_t) <= SHADOW_UI_BUFFER_SIZE) ? 1 : -1];

/* Shadow param request structure - for set_param/get_param calls from shadow UI */
#define SHADOW_PARAM_KEY_LEN 64
#define SHADOW_PARAM_VALUE_LEN 440
typedef struct shadow_param_t {
    volatile uint8_t request_type;  /* 0=none, 1=set, 2=get */
    volatile uint8_t slot;          /* Which chain slot (0-3) */
    volatile uint8_t response_ready;/* Set by shim when response is ready */
    volatile uint8_t error;         /* Non-zero on error */
    volatile int32_t result_len;    /* Length of result, -1 on error */
    char key[SHADOW_PARAM_KEY_LEN];
    char value[SHADOW_PARAM_VALUE_LEN];
} shadow_param_t;

static shadow_param_t *shadow_param = NULL;
typedef char shadow_param_size_check[(sizeof(shadow_param_t) <= SHADOW_PARAM_BUFFER_SIZE) ? 1 : -1];

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
#define SHADOW_CHAIN_MODULE_DIR "/data/UserData/move-anything/modules/chain"
#define SHADOW_CHAIN_DSP_PATH "/data/UserData/move-anything/modules/chain/dsp.so"
#define SHADOW_CHAIN_CONFIG_PATH "/data/UserData/move-anything/shadow_chain_config.json"
#define SHADOW_CHAIN_INSTANCES 4

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

typedef struct shadow_chain_slot_t {
    void *instance;
    int channel;
    int patch_index;
    int active;
    float volume;           /* 0.0 to 1.0, applied to audio output */
    int forward_channel;    /* -1 = none, 0-15 = forward MIDI to this channel */
    char patch_name[64];
} shadow_chain_slot_t;

static shadow_chain_slot_t shadow_chain_slots[SHADOW_CHAIN_INSTANCES];

static const char *shadow_chain_default_patches[SHADOW_CHAIN_INSTANCES] = {
    "",  /* No default patch - user must select */
    "",
    "",
    ""
};;

static int shadow_chain_parse_channel(int ch) {
    /* Config uses 1-based MIDI channels; convert to 0-based for status nibble. */
    if (ch >= 1 && ch <= 16) {
        return ch - 1;
    }
    return ch;
}

static void shadow_log(const char *msg) {
    FILE *log = fopen("/data/UserData/move-anything/shadow_inprocess.log", "a");
    if (log) {
        fprintf(log, "%s\n", msg ? msg : "(null)");
        fclose(log);
    }
}

static void shadow_chain_defaults(void) {
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        shadow_chain_slots[i].instance = NULL;
        shadow_chain_slots[i].active = 0;
        shadow_chain_slots[i].patch_index = -1;
        shadow_chain_slots[i].channel = shadow_chain_parse_channel(5 + i);
        shadow_chain_slots[i].volume = 1.0f;
        shadow_chain_slots[i].forward_channel = -1;
        strncpy(shadow_chain_slots[i].patch_name,
                shadow_chain_default_patches[i],
                sizeof(shadow_chain_slots[i].patch_name) - 1);
        shadow_chain_slots[i].patch_name[sizeof(shadow_chain_slots[i].patch_name) - 1] = '\0';
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
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Shadow inprocess: patch not found: %s",
                     shadow_chain_slots[i].patch_name);
            shadow_log(msg);
        }
    }

    shadow_ui_state_refresh();
    shadow_inprocess_ready = 1;
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
        if (shadow_chain_slots[i].active && shadow_chain_slots[i].channel == ch) {
            return i;
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
    return (note < 10) || (note >= 16 && note <= 31) || (note >= 40 && note <= 43);
}

static int shadow_allow_midi_to_dsp(uint8_t status, uint8_t data1)
{
    uint8_t type = status & 0xF0;
    if (type == 0x90 || type == 0x80) {
        return !shadow_is_internal_control_note(data1);
    }
    if (type == 0xA0 || type == 0xD0 || type == 0xE0) {
        return 1;
    }
    return 0;
}

static uint32_t shadow_ui_request_seen = 0;
/* Special patch index value meaning "none" / clear the slot */
#define SHADOW_PATCH_INDEX_NONE 65535

static void shadow_inprocess_handle_ui_request(void) {
    if (!shadow_control || !shadow_plugin_v2 || !shadow_plugin_v2->set_param) return;

    uint32_t request_id = shadow_control->ui_request_id;
    if (request_id == shadow_ui_request_seen) return;
    shadow_ui_request_seen = request_id;

    int slot = shadow_control->ui_slot;
    int patch_index = shadow_control->ui_patch_index;
    if (slot < 0 || slot >= SHADOW_CHAIN_INSTANCES) return;
    if (patch_index < 0) return;
    if (!shadow_chain_slots[slot].instance) return;

    /* Handle "none" special value - clear the slot */
    if (patch_index == SHADOW_PATCH_INDEX_NONE) {
        shadow_chain_slots[slot].active = 0;
        shadow_chain_slots[slot].patch_index = -1;
        strncpy(shadow_chain_slots[slot].patch_name, "none", sizeof(shadow_chain_slots[slot].patch_name) - 1);
        shadow_chain_slots[slot].patch_name[sizeof(shadow_chain_slots[slot].patch_name) - 1] = '\0';
        /* Update UI state */
        if (shadow_ui_state && slot < SHADOW_UI_SLOTS) {
            strncpy(shadow_ui_state->slot_names[slot], "none", SHADOW_UI_NAME_LEN - 1);
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
            shadow_plugin_v2->set_param(shadow_chain_slots[slot].instance,
                                        shadow_param->key, shadow_param->value);
            shadow_param->error = 0;
            shadow_param->result_len = 0;
        } else {
            shadow_param->error = 3;
            shadow_param->result_len = -1;
        }
    }
    else if (req_type == 2) {  /* GET param */
        if (shadow_plugin_v2->get_param) {
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

static void shadow_inprocess_process_midi(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    const uint8_t *in_src = global_mmap_addr + MIDI_IN_OFFSET;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        const uint8_t *pkt = &in_src[i];
        if (pkt[0] == 0 && pkt[1] == 0 && pkt[2] == 0 && pkt[3] == 0) continue;

        uint8_t cable = (pkt[0] >> 4) & 0x0F;
        uint8_t cin = pkt[0] & 0x0F;
        uint8_t status = pkt[1];

        if (pkt[1] == 0xFE || pkt[1] == 0xF8 || cin == 0x0F) continue;
        if (cable != 0) continue;
        if (cin < 0x08 || cin > 0x0E) continue;
        if ((status & 0xF0) < 0x80 || (status & 0xF0) > 0xE0) continue;
        if (!shadow_allow_midi_to_dsp(status, pkt[2])) continue;

        int slot = shadow_chain_slot_for_channel(status & 0x0F);
        if (slot < 0) continue;

        if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
            /* Remap channel based on slot's forward_channel setting */
            uint8_t msg[3] = { shadow_chain_remap_channel(slot, pkt[1]), pkt[2], pkt[3] };
            shadow_plugin_v2->on_midi(shadow_chain_slots[slot].instance, msg, 3,
                                      MOVE_MIDI_SOURCE_INTERNAL);
        }
    }

    const uint8_t *out_src = global_mmap_addr + MIDI_OUT_OFFSET;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        const uint8_t *pkt = &out_src[i];
        if (pkt[0] == 0 && pkt[1] == 0 && pkt[2] == 0 && pkt[3] == 0) continue;

        uint8_t cin = pkt[0] & 0x0F;
        uint8_t status_usb = pkt[1];
        uint8_t status_raw = pkt[0];

        if (cin >= 0x08 && cin <= 0x0E && (status_usb & 0x80)) {
            if ((status_usb & 0xF0) < 0x80 || (status_usb & 0xF0) > 0xE0) continue;
            if (!shadow_allow_midi_to_dsp(status_usb, pkt[2])) continue;
            int slot = shadow_chain_slot_for_channel(status_usb & 0x0F);
            if (slot < 0) continue;
            if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                /* Remap channel based on slot's forward_channel setting */
                uint8_t msg[3] = { shadow_chain_remap_channel(slot, pkt[1]), pkt[2], pkt[3] };
                shadow_plugin_v2->on_midi(shadow_chain_slots[slot].instance, msg, 3,
                                          MOVE_MIDI_SOURCE_INTERNAL);
            }
        } else if ((status_raw & 0xF0) >= 0x80 && (status_raw & 0xF0) <= 0xE0) {
            if (pkt[1] <= 0x7F && pkt[2] <= 0x7F) {
                if (!shadow_allow_midi_to_dsp(status_raw, pkt[1])) continue;
                int slot = shadow_chain_slot_for_channel(status_raw & 0x0F);
                if (slot < 0) continue;
                if (shadow_plugin_v2 && shadow_plugin_v2->on_midi) {
                    /* Remap channel based on slot's forward_channel setting */
                    uint8_t msg[3] = { shadow_chain_remap_channel(slot, status_raw), pkt[1], pkt[2] };
                    shadow_plugin_v2->on_midi(shadow_chain_slots[slot].instance, msg, 3,
                                              MOVE_MIDI_SOURCE_INTERNAL);
                }
            }
        }
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

    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        if (mix[i] > 32767) mix[i] = 32767;
        if (mix[i] < -32768) mix[i] = -32768;
        mailbox_audio[i] = (int16_t)mix[i];
    }
}

/* Shared memory segment names */
#define SHM_SHADOW_AUDIO    "/move-shadow-audio"    /* Shadow's mixed output */
#define SHM_SHADOW_MIDI     "/move-shadow-midi"
#define SHM_SHADOW_UI_MIDI  "/move-shadow-ui-midi"
#define SHM_SHADOW_DISPLAY  "/move-shadow-display"
#define SHM_SHADOW_CONTROL  "/move-shadow-control"
#define SHM_SHADOW_MOVEIN   "/move-shadow-movein"   /* Move's audio for shadow to read */
#define SHM_SHADOW_UI       "/move-shadow-ui"       /* Shadow UI state */
#define SHM_SHADOW_PARAM    "/move-shadow-param"    /* Shadow param requests */


#define NUM_AUDIO_BUFFERS 3  /* Triple buffering */

/* Shadow shared memory pointers */
static int16_t *shadow_audio_shm = NULL;    /* Shadow's mixed output */
static int16_t *shadow_movein_shm = NULL;   /* Move's audio for shadow to read */
static uint8_t *shadow_midi_shm = NULL;
static uint8_t *shadow_ui_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;

/* Shadow shared memory file descriptors */
static int shm_audio_fd = -1;
static int shm_movein_fd = -1;
static int shm_midi_fd = -1;
static int shm_ui_midi_fd = -1;
static int shm_display_fd = -1;
static int shm_control_fd = -1;
static int shm_ui_fd = -1;
static int shm_param_fd = -1;

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

    shadow_shm_initialized = 1;
    printf("Shadow: Shared memory initialized (audio=%p, midi=%p, ui_midi=%p, display=%p, control=%p, ui=%p, param=%p)\n",
           shadow_audio_shm, shadow_midi_shm, shadow_ui_midi_shm,
           shadow_display_shm, shadow_control, shadow_ui_state, shadow_param);
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

    /* Cache flag file checks - only re-check every 1000 calls to avoid filesystem overhead */
    static int cache_counter = 0;
    static int cached_ch3_only = 0;
    static int cached_block_ch1 = 0;
    static int cached_allow_ch5_8 = 0;
    static int cached_notes_only = 0;
    static int cached_allow_cable0 = 0;
    static int cached_drop_cable_f = 0;
    static int cached_log_on = 0;
    static int cached_drop_ui = 0;

    if (cache_counter++ % 1000 == 0) {
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

static void shadow_filter_move_input(void)
{
    if (!shadow_control || !shadow_display_mode) return;
    if (!global_mmap_addr) return;

    uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;

    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = src[i] & 0x0F;
        uint8_t cable = (src[i] >> 4) & 0x0F;
        if (cin < 0x08 || cin > 0x0E) {
            continue;
        }

        /* Pass through non-cable-0 events (external MIDI) */
        if (cable != 0x00) {
            continue;
        }

        uint8_t status = src[i + 1];
        uint8_t type = status & 0xF0;
        uint8_t d1 = src[i + 2];
        uint8_t d2 = src[i + 3];

        /* Forward CC events to shadow UI, then zero them out from Move */
        if (type == 0xB0) {
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
            /* Zero out CC from Move - only shadow UI should see it */
            src[i] = 0;
            src[i + 1] = 0;
            src[i + 2] = 0;
            src[i + 3] = 0;
            continue;
        }

        /* For notes: only pass through pad notes (68-99) and aftertouch */
        if (type == 0x90 || type == 0x80) {
            if (d1 >= 68 && d1 <= 99) {
                continue;  /* Pass pad notes through to Move */
            }
            /* Zero out non-pad notes (knob touches, steps, etc.) */
            src[i] = 0;
            src[i + 1] = 0;
            src[i + 2] = 0;
            src[i + 3] = 0;
            continue;
        }

        /* Pass through aftertouch for pads */
        if (type == 0xA0 || type == 0xD0) {
            continue;
        }

        /* Zero out everything else */
        src[i] = 0;
        src[i + 1] = 0;
        src[i + 2] = 0;
        src[i + 3] = 0;
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
        global_mmap_addr = result;
        /* Initialize shadow shared memory when we detect the SPI mailbox */
        init_shadow_shm();
#if SHADOW_INPROCESS_POC
        shadow_inprocess_load_chain();
#endif
    }

    printf("mmap hooked! addr=%p, length=%zu, prot=%d, flags=%d, fd=%d, offset=%lld, result=%p\n",
           addr, length, prot, flags, fd, (long long)offset, result);

    // output_file = fopen("spi_memory.txt", "w+");

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
int knob1touched = 0;
int alreadyLaunched = 0;       /* Prevent multiple launches */
int shadowModeDebounce = 0;    /* Debounce for shadow mode toggle */

/* Debug logging disabled for performance - set to 1 to enable */
#define SHADOW_HOTKEY_DEBUG 0
#if SHADOW_HOTKEY_DEBUG
static FILE *hotkey_state_log = NULL;
#endif
static uint64_t shift_on_ms = 0;
static uint64_t vol_on_ms = 0;
static uint64_t knob1_on_ms = 0;
static uint8_t hotkey_prev[MIDI_BUFFER_SIZE];
static int hotkey_prev_valid = 0;
static int shift_armed = 0;
static int volume_armed = 0;
static int knob1_armed = 0;

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

static void maybe_toggle_shadow(void)
{
    if (shadowModeDebounce) return;

    uint64_t now = now_mono_ms();
    if (!shadow_hotkey_enable_ms) {
        shadow_hotkey_enable_ms = now + SHADOW_HOTKEY_GRACE_MS;
    }
    if (now < shadow_hotkey_enable_ms) {
        return;
    }

    if (shiftHeld && volumeTouched && knob1touched &&
        within_window(now, shift_on_ms, SHADOW_HOTKEY_WINDOW_MS) &&
        within_window(now, vol_on_ms, SHADOW_HOTKEY_WINDOW_MS) &&
        within_window(now, knob1_on_ms, SHADOW_HOTKEY_WINDOW_MS)) {
        shadowModeDebounce = 1;
        log_hotkey_state("toggle");
        if (shadow_control) {
            shadow_display_mode = !shadow_display_mode;
            shadow_control->display_mode = shadow_display_mode;
            shadow_inject_knob_release = 1;  /* Inject note-offs to release knob touches */
            printf("Shadow mode toggled: %s\n",
                   shadow_display_mode ? "SHADOW" : "STOCK");
            if (shadow_display_mode) {
                launch_shadow_ui();
            }
        }
    }
}

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
        fprintf(hotkey_state_log, "%ld %s shift=%d vol=%d knob1=%d knob8=%d debounce=%d\n",
                (long)now, tag, shiftHeld, volumeTouched, knob1touched, knob8touched, shadowModeDebounce);
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
                        shift_on_ms = now_mono_ms();
                        log_hotkey_state("shift_on");
                        maybe_toggle_shadow();
                    }
                }
                else
                {
#if SHADOW_HOTKEY_DEBUG
                    printf("Shift off\n");
#endif

                    shiftHeld = 0;
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

        /* Knob 1 touch detection (Note 0) - for shadow mode toggle */
        if ((midi_0 & 0xF0) == 0x90 && midi_1 == 0x00)
        {
            if (midi_2 == 0x7f)
            {
                if (!knob1touched && knob1_armed) {
                    knob1touched = 1;
#if SHADOW_HOTKEY_DEBUG
                    printf("Knob 1 touch start\n");
#endif
                    knob1_on_ms = now_mono_ms();
                    log_hotkey_state("knob1_on");
                    maybe_toggle_shadow();
                }
            }
            else
            {
                knob1touched = 0;
                knob1_armed = 1;
#if SHADOW_HOTKEY_DEBUG
                printf("Knob 1 touch stop\n");
#endif
                knob1_on_ms = 0;
                log_hotkey_state("knob1_off");
            }
        }

        if ((midi_0 & 0xF0) == 0x90 && midi_1 == 0x08)
        {
            if (midi_2 == 0x7f)
            {
                if (!volumeTouched && volume_armed) {
                    volumeTouched = 1;
                    vol_on_ms = now_mono_ms();
                    log_hotkey_state("vol_on");
                    maybe_toggle_shadow();
                }
            }
            else
            {
                volumeTouched = 0;
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

        /* Shadow mode toggle: Shift + Volume + Knob 1 */

        /* Reset debounce once any part of the combo is released */
        if (shadowModeDebounce && (!shiftHeld || !volumeTouched || !knob1touched))
        {
            shadowModeDebounce = 0;
            log_hotkey_state("debounce_reset");
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

    // print_mem();
    // write_mem();

    // TODO: Consider using move-anything host code and quickjs for flexibility
    midi_monitor();

    /* Check if shadow UI requested exit via shared memory */
    if (shadow_control && shadow_display_mode && !shadow_control->display_mode) {
        shadow_display_mode = 0;
        shadow_inject_knob_release = 1;  /* Inject note-offs when exiting shadow mode */
    }

    /* NOTE: MIDI filtering moved to AFTER ioctl - see post-ioctl section below */

    spi_trace_ioctl(request, (char *)argp);

    /* === SHADOW INSTRUMENT: PRE-IOCTL PROCESSING === */
    /* Capture outgoing/incoming MIDI before hardware clears the buffer */
    shadow_capture_midi_probe();
    shadow_scan_mailbox_raw();
    mailbox_diff_probe();
    mailbox_midi_scan_strict();
    mailbox_usb_midi_scan();
    mailbox_midi_region_scan();
    mailbox_midi_out_frame_log();

    /* Forward MIDI BEFORE ioctl - hardware clears the buffer during transaction */
    shadow_forward_midi();

    /* Mix shadow audio into mailbox BEFORE hardware transaction */
    shadow_mix_audio();

#if SHADOW_INPROCESS_POC
    shadow_inprocess_handle_ui_request();
    shadow_inprocess_handle_param_request();
    shadow_inprocess_process_midi();
    shadow_inprocess_mix_audio();
#endif

    /* Write display BEFORE ioctl - overwrites Move's content right before send */
    shadow_swap_display();

    /* === HARDWARE TRANSACTION === */
    int result = real_ioctl(fd, request, argp);

    /* === POST-IOCTL: FILTER INCOMING MIDI ===
     * MIDI INPUT comes FROM the hardware DURING the ioctl.
     * We must filter AFTER ioctl returns, before Move reads the mailbox.
     * This prevents jog/click/back from reaching Move while in shadow mode. */
    if (shadow_display_mode && shadow_control && global_mmap_addr) {
        uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;
        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            uint8_t cin = src[j] & 0x0F;
            uint8_t cable = (src[j] >> 4) & 0x0F;
            if (cin < 0x08 || cin > 0x0E) continue;
            if (cable != 0x00) continue;  /* Only filter internal cable 0 (Move hardware) */

            uint8_t status = src[j + 1];
            uint8_t type = status & 0xF0;
            uint8_t d1 = src[j + 2];
            uint8_t d2 = src[j + 3];

            /* Forward CC events to shadow UI, then zero from Move's view */
            if (type == 0xB0) {
                /* Don't filter shift (0x31) - needed for hotkey detection to exit shadow mode */
                if (d1 != 0x31) {
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
                    /* Zero out the CC event so Move doesn't see it */
                    src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                }
                continue;
            }

            /* Only pass pad notes (68-99) to Move for MIDI output */
            if (type == 0x90 || type == 0x80) {
                if (d1 < 68 || d1 > 99) {
                    /* Zero out non-pad notes (buttons, knob touches, etc.) */
                    src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
                }
                continue;
            }

            /* Pass aftertouch through to Move */
            if (type == 0xA0 || type == 0xD0) {
                continue;
            }

            /* Zero everything else */
            src[j] = 0; src[j + 1] = 0; src[j + 2] = 0; src[j + 3] = 0;
        }
    }

    /* === POST-IOCTL: INJECT KNOB RELEASE EVENTS ===
     * When toggling shadow mode, inject note-off events for knob touches
     * so Move doesn't think knobs are still being held.
     * This MUST happen AFTER filtering to avoid being zeroed out. */
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

    return result;
}
