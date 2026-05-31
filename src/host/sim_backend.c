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
