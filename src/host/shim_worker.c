/* shim_worker.c - Background housekeeping thread for the shim.
 * See shim_worker.h for the contract. Extracted as part of RT pass 1
 * (docs/plans/2026-06-11-codebase-cleanup-review.md §1). */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>

#include "shim_worker.h"
#include "shadow_set_pages.h"
#include "unified_log.h"

volatile uint32_t shim_debug_flags = 0;
volatile int shim_pending_sysex_inject = -1;

/* SPSC event ring: RT producer (SPI callbacks), worker consumer. */
#define EVT_RING_SIZE 16  /* power of two */
static volatile uint8_t evt_ring[EVT_RING_SIZE];
static volatile unsigned evt_head = 0;  /* producer writes */
static volatile unsigned evt_tail = 0;  /* consumer reads */

void shim_worker_post(uint8_t evt) {
    unsigned head = evt_head;
    if (head - evt_tail >= EVT_RING_SIZE) return;  /* full — drop */
    evt_ring[head & (EVT_RING_SIZE - 1)] = evt;
    __sync_synchronize();
    evt_head = head + 1;
}

/* ---- flag polling ---------------------------------------------------- */

typedef struct {
    const char *path;
    uint32_t bit;
    int oneshot;  /* unlink on detect; RT consumes via test-and-clear */
} flag_spec_t;

static const flag_spec_t FLAGS[] = {
    { "/data/UserData/schwung/spi_snap_trigger",     SHIM_FLAG_SPI_SNAP,     0 },
    { "/data/UserData/schwung/log_xmos_sysex_on",    SHIM_FLAG_XMOS_LOG,     0 },
    { "/data/UserData/schwung/spi_midi_log_on",      SHIM_FLAG_SPI_MIDI_LOG, 0 },
    { "/data/UserData/schwung/slot_fx_dump_trigger", SHIM_FLAG_SLOT_FX_DUMP, 1 },
    { "/data/UserData/schwung/align_dump_trigger",   SHIM_FLAG_ALIGN_DUMP,   1 },
    { "/data/UserData/schwung/main_fx_dump_trigger", SHIM_FLAG_MAIN_FX_DUMP, 1 },
};

static void poll_flags(void) {
    for (size_t i = 0; i < sizeof(FLAGS) / sizeof(FLAGS[0]); i++) {
        int present = (access(FLAGS[i].path, F_OK) == 0);
        if (FLAGS[i].oneshot) {
            if (present) {
                unlink(FLAGS[i].path);
                __sync_fetch_and_or(&shim_debug_flags, FLAGS[i].bit);
            }
        } else {
            if (present) __sync_fetch_and_or(&shim_debug_flags, FLAGS[i].bit);
            else         __sync_fetch_and_and(&shim_debug_flags, ~FLAGS[i].bit);
        }
    }

    /* SysEx inject trigger: file content is the value byte. Publish once;
     * the RT consumer swaps shim_pending_sysex_inject back to -1. */
    static const char inject_path[] = "/data/UserData/schwung/spi_sysex_inject";
    if (shim_pending_sysex_inject < 0 && access(inject_path, F_OK) == 0) {
        int fd = open(inject_path, O_RDONLY);
        int val = 0;
        if (fd >= 0) {
            char buf[8] = {0};
            if (read(fd, buf, sizeof(buf) - 1) > 0) val = atoi(buf);
            close(fd);
        }
        unlink(inject_path);
        shim_pending_sysex_inject = val;
    }
}

/* ---- deferred events -------------------------------------------------- */

/* Overtake exit hook: resolve per-module hook from .exiting-module-id,
 * fall back to the global hook. Runs on the worker (SCHED_OTHER), so the
 * fork/exec inside system() inherits safe scheduling. Moved verbatim from
 * shim_post_transfer. */
static void run_overtake_exit_hook(void) {
    char module_id[64] = {0};
    FILE *f = fopen("/data/UserData/schwung/hooks/.exiting-module-id", "r");
    if (f) {
        if (fgets(module_id, sizeof(module_id), f)) {
            char *nl = strchr(module_id, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
        unlink("/data/UserData/schwung/hooks/.exiting-module-id");
    }

    char hook_path[256];
    int have_per_module = 0;
    if (module_id[0]) {
        snprintf(hook_path, sizeof(hook_path),
                 "/data/UserData/schwung/hooks/overtake-exit-%s.sh", module_id);
        have_per_module = (access(hook_path, X_OK) == 0);
    }

    if (have_per_module) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s &", hook_path);
        system(cmd);
    } else if (!module_id[0]) {
        /* No module ID file — old-style exit, run global hook for backward compat */
        system("sh -c 'test -x /data/UserData/schwung/hooks/overtake-exit.sh && "
               "/data/UserData/schwung/hooks/overtake-exit.sh' &");
    }
    /* If module ID was set but no per-module hook exists, skip cleanup —
     * don't run the global hook which may belong to another module */
}

static void drain_events(void) {
    while (evt_tail != evt_head) {
        uint8_t evt = evt_ring[evt_tail & (EVT_RING_SIZE - 1)];
        __sync_synchronize();
        evt_tail++;
        switch (evt) {
        case SHIM_EVT_OVERTAKE_EXIT_HOOK:
            run_overtake_exit_hook();
            break;
        case SHIM_EVT_RESTART_MOVE:
            /* Clean restart (kill as root, start fresh). Fork+exec won't
             * work because MoveOriginal has file capabilities that trigger
             * AT_SECURE, blocking LD_PRELOAD from a non-root process. */
            system("/data/UserData/schwung/restart-move.sh");
            break;
        default:
            break;
        }
    }
}

/* ---- thread ------------------------------------------------------------ */

static void *worker_main(void *arg) {
    (void)arg;

    /* SCHED_OTHER, pinned to cores 0-2 — keep core 3 free for the SPI
     * SCHED_FIFO 90 callback (same pattern as the link subscriber). */
    struct sched_param sp = { .sched_priority = 0 };
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask);
    CPU_SET(1, &mask);
    CPU_SET(2, &mask);
    pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

    unsigned tick = 0;
    for (;;) {
        usleep(200 * 1000);             /* 200 ms cadence */
        drain_events();                 /* event latency ≤ ~200 ms */
        if (tick % 5 == 0) poll_flags();          /* ~1 Hz */
        if (tick % 7 == 0) shadow_poll_current_set(); /* ~1.4 s FS scan */
        tick++;
    }
    return NULL;
}

void shim_worker_start(void) {
    static volatile int started = 0;
    if (__sync_lock_test_and_set(&started, 1)) return;
    pthread_t tid;
    if (pthread_create(&tid, NULL, worker_main, NULL) != 0) {
        started = 0;
        unified_log("shim", LOG_LEVEL_ERROR, "shim_worker: pthread_create failed");
        return;
    }
    pthread_detach(tid);
}
