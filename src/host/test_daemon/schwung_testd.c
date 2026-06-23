/*
 * schwung-testd — test-bus daemon for on-device E2E tests.
 *
 * Listens on TCP loopback (default 127.0.0.1:47777), accepts a single
 * client connection at a time, and exposes a line-based text protocol
 * that lets a test runner inject MIDI events into Move's MIDI_IN buffer
 * and snapshot basic shim state (frame counter, pad LED colors).
 *
 * Talks to the shim through existing SHM contracts:
 *   /schwung-control      — read shim_counter for frame-sync ack
 *   /schwung-midi-inject  — write USB-MIDI packets into the inject ring
 *                           (multi-producer; uses shadow_midi_inject_push)
 *   /schwung-overlay      — read pad_led_colors snapshot
 *
 * Designed to be opt-in and dev-only: not started by the production
 * shim-entrypoint, no setuid, binds loopback by default. See README.md
 * for usage.
 *
 * Code structure:
 *   schwung_testd.c — main: arg/env, signals, SHM mapping, listener
 *   protocol.{c,h}  — line I/O, hex codec, command-line parsing
 *   commands.{c,h}  — verb table + handlers (the only place to touch
 *                     when adding a new command)
 *
 * Protocol v1 (line-based, \n-terminated, ASCII):
 *   PING                        -> OK schwung-testd <version>
 *   INJECT_MIDI <8-hex-chars>   -> OK            (1 USB-MIDI packet, 4 bytes)
 *   WAIT_FRAME <N>              -> OK frame=<counter>
 *   SNAPSHOT_PAD_LEDS           -> OK <64-hex-chars>
 *   QUIT                        -> OK bye        (server closes connection)
 *   <unknown>                   -> ERR <message>
 */

#define _GNU_SOURCE

#include "commands.h"
#include "protocol.h"
#include "shadow_constants.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define TESTD_VERSION       "0.1.0"
#define TESTD_DEFAULT_PORT  47777

/* --------------------------------------------------------------------------
 * SHM wiring
 * -------------------------------------------------------------------------- */

static int map_shm_ro(const char *name, size_t size, void **out) {
    int fd = shm_open(name, O_RDONLY, 0666);
    if (fd < 0) {
        fprintf(stderr, "shm_open(%s, RO) failed: %s\n", name, strerror(errno));
        return -1;
    }
    void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap(%s, RO) failed: %s\n", name, strerror(errno));
        return -1;
    }
    *out = p;
    return 0;
}

static int map_shm_rw(const char *name, size_t size, void **out) {
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) {
        fprintf(stderr, "shm_open(%s, RW) failed: %s\n", name, strerror(errno));
        return -1;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap(%s, RW) failed: %s\n", name, strerror(errno));
        return -1;
    }
    *out = p;
    return 0;
}

static int wire_shm(daemon_shm_t *out) {
    /* Control is mapped RW: STATE reads many fields, RESTART_MOVE writes
     * the `restart_move` flag. The cost of widening the mapping is zero
     * for fields the daemon never writes — kernel doesn't care. */
    if (map_shm_rw(SHM_SHADOW_CONTROL, sizeof(shadow_control_t),
                   (void **)&out->control) < 0) return -1;
    if (map_shm_rw(SHM_SHADOW_MIDI_INJECT, sizeof(shadow_midi_inject_t),
                   (void **)&out->inject) < 0) return -1;
    if (map_shm_ro(SHM_SHADOW_OVERLAY, sizeof(shadow_overlay_state_t),
                   (void **)&out->overlay) < 0) return -1;
    /* Test-stream is RW: daemon writes `enabled` to gate shim's capture,
     * shim writes the buffer. */
    if (map_shm_rw(SHM_TEST_STREAM_MIDI_OUT, sizeof(test_stream_shm_t),
                   (void **)&out->midi_out_stream) < 0) return -1;
    /* Param SHM: daemon is a *second* producer here (shadow_ui already is),
     * routing SET_PARAM / GET_PARAM tests into chain DSPs and overtake
     * modules. The wait_idle protocol in shadow_param_wait_idle()
     * serializes the two producers — see SET_PARAM handler in commands.c
     * for the rationale + race window. */
    if (map_shm_rw(SHM_SHADOW_PARAM, sizeof(shadow_param_t),
                   (void **)&out->param) < 0) return -1;
    return 0;
}

/* --------------------------------------------------------------------------
 * Per-connection serve loop
 * -------------------------------------------------------------------------- */

static void serve_client(int fd) {
    char line[TESTD_LINE_MAX];
    for (;;) {
        int n = protocol_read_line(fd, line, sizeof(line));
        if (n < 0) return;
        if (n == 0) continue;                    /* empty line, ignore */
        char *verb, *args;
        protocol_split_command(line, &verb, &args);
        int rc = commands_dispatch(fd, verb, args);
        if (rc != 0) return;                     /* QUIT or error: close */
    }
}

/* --------------------------------------------------------------------------
 * TCP listener
 * -------------------------------------------------------------------------- */

static int open_listener(const char *bind_addr, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "bad bind address: %s\n", bind_addr);
        close(s);
        return -1;
    }
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "bind(%s:%d) failed: %s\n", bind_addr, port, strerror(errno));
        close(s);
        return -1;
    }
    if (listen(s, 4) < 0) {
        perror("listen");
        close(s);
        return -1;
    }
    return s;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;
static volatile int g_listen_fd = -1;  /* main writes once; signal handler reads */

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    /* Close the listening socket so accept() returns immediately with
     * EBADF; the loop checks g_stop and exits cleanly. Without this,
     * accept() blocks indefinitely and SIGTERM has no effect — forcing
     * SIGKILL during hot-swap. close() is async-signal-safe per POSIX. */
    int fd = g_listen_fd;
    if (fd >= 0) {
        g_listen_fd = -1;
        close(fd);
    }
}

int main(int argc, char **argv) {
    const char *bind_addr = getenv("SCHWUNG_TEST_BIND");
    if (!bind_addr) bind_addr = "127.0.0.1";
    const char *port_env = getenv("SCHWUNG_TEST_PORT");
    int port = port_env ? atoi(port_env) : TESTD_DEFAULT_PORT;
    if (port <= 0 || port > 65535) port = TESTD_DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("schwung-testd %s\n", TESTD_VERSION);
            printf("Usage: schwung-testd [--help]\n");
            printf("Env: SCHWUNG_TEST_BIND (default 127.0.0.1)\n");
            printf("     SCHWUNG_TEST_PORT (default %d)\n", TESTD_DEFAULT_PORT);
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    daemon_shm_t shm;
    if (wire_shm(&shm) < 0) {
        fprintf(stderr, "schwung-testd: failed to map SHM segments. "
                        "Is the shim running (MoveOriginal active)?\n");
        return 1;
    }
    commands_init(&shm);

    int srv = open_listener(bind_addr, port);
    if (srv < 0) return 1;
    g_listen_fd = srv;

    fprintf(stderr, "schwung-testd %s listening on %s:%d\n",
            TESTD_VERSION, bind_addr, port);

    while (!g_stop) {
        struct sockaddr_in ca;
        socklen_t cal = sizeof(ca);
        int c = accept(srv, (struct sockaddr *)&ca, &cal);
        if (c < 0) {
            if (errno == EINTR) continue;
            if (g_stop && (errno == EBADF || errno == EINVAL)) {
                /* Signal handler closed srv to break us out — clean exit. */
                break;
            }
            perror("accept");
            break;
        }
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof(ip));
        fprintf(stderr, "schwung-testd: client connected from %s:%d\n",
                ip, ntohs(ca.sin_port));
        serve_client(c);
        close(c);
        /* Unconditionally drop per-client state so a dropped TCP
         * connection (without QUIT) doesn't leak a stale subscription
         * into the next client's first DUMP. See commands.h. */
        commands_reset_client_state();
        fprintf(stderr, "schwung-testd: client disconnected\n");
    }

    /* Signal handler may have already closed srv; close() on -1 is a
     * no-op error. Either way, drop the global so a late signal can't
     * race a fresh fd if main() were ever restarted in-process. */
    int fd = g_listen_fd;
    g_listen_fd = -1;
    if (fd >= 0) close(fd);
    return 0;
}
