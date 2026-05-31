// SPDX-License-Identifier: MIT
//
// schwung-host sim backend — see sim_backend.h.
//
// The sim mailbox lives entirely in this process. We hand the host back a
// shadow buffer (static, aligned) on mmap, and back the "hw" side with calloc.
// The ioctl barrier blocks on a self-pipe; the sim daemon writes 1 byte to the
// write end after refreshing the RX region.

#include "sim_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Pull in the SPI protocol constants (SCHWUNG_PAGE_SIZE, OFF_IN_BASE, etc.).
// We DO NOT link against schwung-spi — only the header is needed.
#include "schwung_spi_lib.h"

// ============================================================================
// Singleton state
// ============================================================================

static struct {
    int fd;                                                          // tempfile fd
    uint8_t shadow[SCHWUNG_PAGE_SIZE] __attribute__((aligned(64)));  // shown to host
    uint8_t *hw;                                                     // calloc-backed
    int tick_pipe[2];                                                // {read, write}
    int ready;                                                       // mmap done
} g_sim = { .fd = -1, .tick_pipe = {-1, -1} };

// ============================================================================
// Public API
// ============================================================================

int schwung_sim_open(void) {
    if (g_sim.fd >= 0) return g_sim.fd;  // idempotent

    // mkstemp + unlink gives us a memfd-equivalent: real fd backed by a real
    // inode, but no directory entry — the inode dies when the fd closes.
    char tmpl[] = "/tmp/schwung-sim-spi-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    unlink(tmpl);
    if (ftruncate(fd, SCHWUNG_PAGE_SIZE) != 0) {
        close(fd);
        return -1;
    }
    g_sim.fd = fd;

    // Lazy-create the tick pipe. Pipe lives for the process lifetime.
    if (g_sim.tick_pipe[0] < 0) {
        if (pipe(g_sim.tick_pipe) != 0) {
            // Caller will hit -1/EBADF in schwung_sim_ioctl_wait. We
            // deliberately don't fail the open — the caller may not need
            // the barrier (e.g. unit tests that only exercise mmap).
            fprintf(stderr, "schwung_sim: pipe() failed: %s\n", strerror(errno));
        } else {
            fcntl(g_sim.tick_pipe[0], F_SETFD, FD_CLOEXEC);
            fcntl(g_sim.tick_pipe[1], F_SETFD, FD_CLOEXEC);
        }
    }
    return fd;
}

uint8_t *schwung_sim_mmap(void) {
    if (g_sim.fd < 0) return NULL;
    if (!g_sim.hw) {
        g_sim.hw = (uint8_t *)calloc(1, SCHWUNG_PAGE_SIZE);
        if (!g_sim.hw) return NULL;
    }
    memset(g_sim.shadow, 0, SCHWUNG_PAGE_SIZE);
    g_sim.ready = 1;
    return g_sim.shadow;
}

int schwung_sim_ioctl_wait(void) {
    if (!g_sim.ready) { errno = EINVAL; return -1; }
    if (g_sim.tick_pipe[0] < 0) { errno = EBADF; return -1; }

    // Shadow → hw (TX region: MIDI OUT + display + audio OUT)
    memcpy(g_sim.hw, g_sim.shadow, SCHWUNG_OFF_IN_BASE);

    // Barrier: wait for the sim daemon to populate hw[IN_BASE..] and pulse
    // the pipe.
    char b;
    ssize_t n;
    do {
        n = read(g_sim.tick_pipe[0], &b, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) {
        // EOF (n == 0) means the daemon exited; surface as EPIPE so callers
        // can distinguish from generic I/O failure.
        errno = (n == 0) ? EPIPE : EIO;
        return -1;
    }

    // hw → shadow (RX region: MIDI IN + display status + audio IN)
    memcpy(g_sim.shadow + SCHWUNG_OFF_IN_BASE,
           g_sim.hw + SCHWUNG_OFF_IN_BASE,
           SCHWUNG_PAGE_SIZE - SCHWUNG_OFF_IN_BASE);

    return 0;
}

void schwung_sim_close(void) {
    if (g_sim.fd >= 0) {
        close(g_sim.fd);
        g_sim.fd = -1;
    }
    free(g_sim.hw);
    g_sim.hw = NULL;
    g_sim.ready = 0;
    // Note: tick_pipe is intentionally kept open across close/reopen cycles
    // because the sim daemon may hold the write end. Leaks at process exit
    // only, which is fine.
}

// ============================================================================
// Sim Daemon API
// ============================================================================

uint8_t *schwung_sim_get_hw_buffer(void) {
    return g_sim.ready ? g_sim.hw : NULL;
}

int schwung_sim_get_tick_fd(void) {
    return g_sim.tick_pipe[1];
}

// ============================================================================
// Display SHM
// ============================================================================
//
// On device the LD_PRELOAD shim writes /schwung-display-live. In sim mode,
// schwung-host calls schwung_sim_push_display() from push_screen() with the
// fully-packed 1024-byte framebuffer; we mmap the SHM lazily and memcpy it in.
// display-server then streams it via SSE to any connected browser.

#define SCHWUNG_DISPLAY_SHM_NAME "/schwung-display-live"
#define SCHWUNG_DISPLAY_BYTES    1024

static struct {
    uint8_t *map;
    int init_failed;   // sticky: once we've logged, don't keep retrying
} g_disp = { NULL, 0 };

static int display_shm_init(void) {
    if (g_disp.map) return 0;
    if (g_disp.init_failed) return -1;

    int fd = shm_open(SCHWUNG_DISPLAY_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        fprintf(stderr, "sim_display: shm_open(%s) failed: %s\n",
                SCHWUNG_DISPLAY_SHM_NAME, strerror(errno));
        g_disp.init_failed = 1;
        return -1;
    }
    // macOS quirk: ftruncate on a POSIX SHM works only at initial creation.
    // Subsequent calls fail with EINVAL even when reducing to the same size.
    // Try ftruncate, but tolerate EINVAL as long as mmap succeeds below at
    // the expected size (which we verify via fstat).
    if (ftruncate(fd, SCHWUNG_DISPLAY_BYTES) != 0 && errno != EINVAL) {
        fprintf(stderr, "sim_display: ftruncate failed: %s\n", strerror(errno));
        close(fd);
        g_disp.init_failed = 1;
        return -1;
    }
    void *m = mmap(NULL, SCHWUNG_DISPLAY_BYTES, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);  // mmap'd region keeps the SHM alive
    if (m == MAP_FAILED) {
        fprintf(stderr, "sim_display: mmap failed: %s\n", strerror(errno));
        g_disp.init_failed = 1;
        return -1;
    }
    g_disp.map = (uint8_t *)m;
    memset(g_disp.map, 0, SCHWUNG_DISPLAY_BYTES);
    fprintf(stderr, "sim_display: %s mapped (%d bytes)\n",
            SCHWUNG_DISPLAY_SHM_NAME, SCHWUNG_DISPLAY_BYTES);
    return 0;
}

int schwung_sim_push_display(const uint8_t *frame_1024) {
    if (!frame_1024) return -1;
    if (display_shm_init() != 0) return -1;
    memcpy(g_disp.map, frame_1024, SCHWUNG_DISPLAY_BYTES);
    return 0;
}
