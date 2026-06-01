// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Charles Vestal
//
// Schwung SPI Library — LD_PRELOAD implementation.
// See schwung_spi_lib.h for API documentation.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "schwung_spi_lib.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Sim Backend integration (opt-in; the shim's build defines this so the
// LD_PRELOAD lib gets sim-mode handling for Sim B. Standalone consumers
// of the lib leave it undefined and get the device-only path as today.)
#ifdef SCHWUNG_LIB_WITH_SIM
#include "../host/sim_backend.h"
#endif

#define SCHWUNG_LOG_PATH "/data/UserData/schwung/schwung.log"

// ============================================================================
// Singleton state
// ============================================================================

struct SchwungSpi {
    int spi_fd;
    uint8_t shadow[SCHWUNG_PAGE_SIZE] __attribute__((aligned(64)));
    uint8_t *shadow_ptr;   // → &shadow[0] on device, sim's shadow in sim mode
    uint8_t *hw;
    schwung_spi_pre_fn pre_fn;
    schwung_spi_post_fn post_fn;
    void *ctx;
    int ready;
};

static SchwungSpi g_spi = { .spi_fd = -1 };

#ifdef SCHWUNG_LIB_WITH_SIM
// SCHWUNG_SPI_BACKEND=sim → route open/mmap/ioctl(WAIT_SEND_SIZE) through
// sim_backend instead of /dev/ablspi0.0. Read once, cached for the process.
static int sim_mode_cache = -1;
static int sim_mode_active(void) {
    if (sim_mode_cache < 0) {
        const char *e = getenv("SCHWUNG_SPI_BACKEND");
        sim_mode_cache = (e && strcmp(e, "sim") == 0) ? 1 : 0;
    }
    return sim_mode_cache;
}
#else
static inline int sim_mode_active(void) { return 0; }
#endif

// Real libc functions
static int (*real_open)(const char *path, int flags, ...) = NULL;
static int (*real_ioctl)(int fd, unsigned long request, ...) = NULL;
static void *(*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;

// ============================================================================
// Logging
// ============================================================================

void schwung_spi_log(const char *msg) {
    FILE *f = fopen(SCHWUNG_LOG_PATH, "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "[%ld.%09ld] %s\n", (long)ts.tv_sec, ts.tv_nsec, msg);
    fclose(f);
}

// ============================================================================
// MIDI helpers
// ============================================================================

int schwung_midi_msg_is_empty(SchwungMidiMsg msg) {
    return msg.channel == 0 && msg.type == 0 && msg.data1 == 0 && msg.data2 == 0;
}

int schwung_usb_midi_msg_is_empty(SchwungUsbMidiMsg msg) {
    return msg.cin == 0 && msg.cable == 0 && schwung_midi_msg_is_empty(msg.midi);
}

SchwungUsbMidiMsg schwung_encode_usb_midi(SchwungMidiMsg msg, uint8_t cable) {
    SchwungUsbMidiMsg out;
    out.cin   = msg.type;
    out.cable = cable;
    out.midi  = msg;
    return out;
}

// ============================================================================
// Audio helpers
// ============================================================================

void schwung_deinterleave_stereo(const int16_t *interleaved,
                                  int16_t *left, int16_t *right, int frames) {
    for (int i = 0; i < frames; i++) {
        left[i]  = interleaved[2 * i];
        right[i] = interleaved[2 * i + 1];
    }
}

void schwung_interleave_stereo(const int16_t *left, const int16_t *right,
                                int16_t *interleaved, int frames) {
    for (int i = 0; i < frames; i++) {
        interleaved[2 * i]     = left[i];
        interleaved[2 * i + 1] = right[i];
    }
}

void schwung_mix_interleaved(int16_t *dest, const int16_t *src, int frames) {
    int total = frames * 2;
    for (int i = 0; i < total; i++) {
        int32_t sum = (int32_t)dest[i] + (int32_t)src[i];
        if (sum > INT16_MAX) sum = INT16_MAX;
        if (sum < INT16_MIN) sum = INT16_MIN;
        dest[i] = (int16_t)sum;
    }
}

// ============================================================================
// API
// ============================================================================

SchwungSpi *schwung_spi_init(void) {
    return &g_spi;
}

void schwung_spi_set_callbacks(SchwungSpi *spi,
                                schwung_spi_pre_fn pre,
                                schwung_spi_post_fn post,
                                void *ctx) {
    spi->pre_fn = pre;
    spi->post_fn = post;
    spi->ctx = ctx;
}

uint8_t *schwung_spi_get_shadow(SchwungSpi *spi) {
    return spi->ready ? spi->shadow_ptr : NULL;
}

uint8_t *schwung_spi_get_hw(SchwungSpi *spi) {
    return spi->hw;
}

int schwung_spi_get_fd(SchwungSpi *spi) {
    return spi->spi_fd;
}

int schwung_spi_is_ready(SchwungSpi *spi) {
    return spi->ready;
}

// ============================================================================
// LD_PRELOAD hooks
// ============================================================================

static void ensure_real_funcs(void) {
    if (!real_open)
        real_open = dlsym(RTLD_NEXT, "open");
    if (!real_ioctl)
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (!real_mmap)
        real_mmap = dlsym(RTLD_NEXT, "mmap");
}

int open(const char *path, int flags, ...) {
    ensure_real_funcs();

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

#ifdef SCHWUNG_LIB_WITH_SIM
    if (sim_mode_active() && path && strcmp(path, SCHWUNG_SPI_DEVICE) == 0) {
        int fd = schwung_sim_open();
        if (fd >= 0) {
            g_spi.spi_fd = fd;
            schwung_spi_log("schwung: SPI sim device opened");
        }
        return fd;
    }
#endif

    int fd = real_open(path, flags, mode);

    if (fd >= 0 && path && strcmp(path, SCHWUNG_SPI_DEVICE) == 0) {
        g_spi.spi_fd = fd;
        schwung_spi_log("schwung: SPI device opened");
    }

    return fd;
}

static int (*real_open64)(const char *, int, ...) = NULL;

int open64(const char *path, int flags, ...) {
    if (!real_open64)
        real_open64 = dlsym(RTLD_NEXT, "open64");

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

#ifdef SCHWUNG_LIB_WITH_SIM
    if (sim_mode_active() && path && strcmp(path, SCHWUNG_SPI_DEVICE) == 0) {
        int fd = schwung_sim_open();
        if (fd >= 0) {
            g_spi.spi_fd = fd;
            schwung_spi_log("schwung: SPI sim device opened (open64)");
        }
        return fd;
    }
#endif

    int fd = real_open64(path, flags, mode);

    if (fd >= 0 && path && strcmp(path, SCHWUNG_SPI_DEVICE) == 0) {
        g_spi.spi_fd = fd;
        schwung_spi_log("schwung: SPI device opened (open64)");
    }

    return fd;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    ensure_real_funcs();

#ifdef SCHWUNG_LIB_WITH_SIM
    if (sim_mode_active() && fd == g_spi.spi_fd && fd >= 0) {
        uint8_t *shadow = schwung_sim_mmap();
        if (!shadow) return MAP_FAILED;
        g_spi.hw         = schwung_sim_get_hw_buffer();
        g_spi.shadow_ptr = shadow;
        g_spi.ready      = 1;
        schwung_spi_log("schwung: SPI sim buffer mapped (shadow active)");
        return shadow;
    }
#endif

    void *ret = real_mmap(addr, length, prot, flags, fd, offset);

    if (fd == g_spi.spi_fd && fd >= 0 && ret != MAP_FAILED) {
        g_spi.hw         = (uint8_t *)ret;
        g_spi.shadow_ptr = g_spi.shadow;
        g_spi.ready      = 1;
        memset(g_spi.shadow, 0, SCHWUNG_PAGE_SIZE);
        schwung_spi_log("schwung: SPI buffer mapped (shadow active)");
        return g_spi.shadow;
    }

    return ret;
}

int ioctl(int fd, unsigned long request, ...) {
    ensure_real_funcs();

    va_list ap;
    va_start(ap, request);
    unsigned long arg = va_arg(ap, unsigned long);
    va_end(ap);

#ifdef SCHWUNG_LIB_WITH_SIM
    if (sim_mode_active() && fd == g_spi.spi_fd && fd >= 0 && g_spi.ready
        && request == (unsigned long)SCHWUNG_IOCTL_WAIT_SEND_SIZE) {
        // Sim mode: drive the same pre/barrier/post sequence as the device
        // path, but use sim_backend's tick pipe instead of a real SPI transfer.
        // pre_fn/post_fn get the sim shadow pointer — same contract as device.
        if (g_spi.pre_fn)
            g_spi.pre_fn(g_spi.ctx, g_spi.shadow_ptr, SCHWUNG_PAGE_SIZE);
        memcpy(g_spi.hw, g_spi.shadow_ptr, SCHWUNG_OFF_IN_BASE);
        if (schwung_sim_wait_for_tick() != 0) return -1;
        memcpy(g_spi.shadow_ptr + SCHWUNG_OFF_IN_BASE,
               g_spi.hw + SCHWUNG_OFF_IN_BASE,
               SCHWUNG_PAGE_SIZE - SCHWUNG_OFF_IN_BASE);
        if (g_spi.post_fn)
            g_spi.post_fn(g_spi.ctx, g_spi.shadow_ptr, g_spi.hw, SCHWUNG_PAGE_SIZE);
        return 0;
    }
    // Other ioctls in sim mode fall through to real_ioctl on the tempfile fd.
    // That fails predictably (ENOTTY/EINVAL) — caller sees "unsupported" not
    // a fabricated success. Add per-ioctl stubs here (GET_STATE, GET_SPEED, …)
    // only when first-boot debugging shows MoveOriginal actually requires them.
#endif

    if (fd == g_spi.spi_fd && fd >= 0
        && request == (unsigned long)SCHWUNG_IOCTL_WAIT_SEND_SIZE
        && g_spi.ready) {

        // Pre-transfer: let domain logic write to shadow buffer
        if (g_spi.pre_fn)
            g_spi.pre_fn(g_spi.ctx, g_spi.shadow_ptr, SCHWUNG_PAGE_SIZE);

        // Copy shadow → hardware (output region only: MIDI + display + audio)
        memcpy(g_spi.hw, g_spi.shadow_ptr, SCHWUNG_OFF_IN_BASE);
    }

    // Blocking SPI transfer with EINTR retry
    int ret;
    do {
        ret = real_ioctl(fd, request, arg);
    } while (ret != 0 && errno == EINTR
             && fd == g_spi.spi_fd
             && request == (unsigned long)SCHWUNG_IOCTL_WAIT_SEND_SIZE);

    if (fd == g_spi.spi_fd && fd >= 0
        && request == (unsigned long)SCHWUNG_IOCTL_WAIT_SEND_SIZE
        && g_spi.ready) {

        // Copy hardware → shadow (input region: MIDI + display status + audio)
        memcpy(g_spi.shadow_ptr + SCHWUNG_OFF_IN_BASE,
               g_spi.hw + SCHWUNG_OFF_IN_BASE,
               SCHWUNG_PAGE_SIZE - SCHWUNG_OFF_IN_BASE);

        // Post-transfer: let domain logic read MIDI, filter, etc.
        if (g_spi.post_fn)
            g_spi.post_fn(g_spi.ctx, g_spi.shadow_ptr, g_spi.hw, SCHWUNG_PAGE_SIZE);
    }

    return ret;
}
