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
volatile int shim_inject_boot_jack = -1;
volatile int shim_jack_persist = -1;

/* Persisted jack state (last CC 115 value). Survives reboot so the worker can
 * re-assert it to Move at boot — XMOS doesn't report jack-in at boot, so an
 * already-plugged headphone otherwise leaves Move's enhancer on "speaker"
 * (hollow audio). */
#define JACK_STATE_PATH "/data/UserData/schwung/jack_state"

static int jack_state_read(void) {
    FILE *f = fopen(JACK_STATE_PATH, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    if (v != 0 && v != 127) return -1;
    return v;
}

static void jack_state_write(int v) {
    FILE *f = fopen(JACK_STATE_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", v);
    fclose(f);
}

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

static shim_worker_hooks_t worker_hooks;

void shim_worker_set_hooks(const shim_worker_hooks_t *hooks) {
    if (hooks) worker_hooks = *hooks;
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
        case SHIM_EVT_SAMPLER_PREP:
            if (worker_hooks.sampler_prepare) worker_hooks.sampler_prepare();
            break;
        case SHIM_EVT_SAMPLER_FINALIZE:
            if (worker_hooks.sampler_finalize) worker_hooks.sampler_finalize();
            break;
        case SHIM_EVT_SAMPLER_CANCEL:
            if (worker_hooks.sampler_cancel_preroll) worker_hooks.sampler_cancel_preroll();
            break;
        case SHIM_EVT_SKIPBACK_SAVE:
            if (worker_hooks.skipback_save) worker_hooks.skipback_save();
            break;
        case SHIM_EVT_SKIPBACK_RESIZE:
            if (worker_hooks.skipback_resize) worker_hooks.skipback_resize();
            break;
        case SHIM_EVT_PREVIEW_PLAY:
            if (worker_hooks.preview_play_pending) worker_hooks.preview_play_pending();
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
    /* Engage debug flags immediately on worker start (before the first 200 ms
     * sleep) so frame-0 diagnostics (e.g. boot-window XMOS jack capture) don't
     * miss the early frames waiting for the first poll. */
    poll_flags();

    /* Jack-state persistence + boot re-assert. XMOS reports jack state only on
     * a physical plug/unplug and (observed) at boot only when the jack is OUT —
     * so booting with headphones already plugged leaves Move's enhancer on
     * "speaker" → hollow headphone audio. Read the last persisted state now and
     * re-assert it to Move ~5 s in (once its firmware is up). If the real state
     * differs (cable swapped while off), XMOS's own report corrects it shortly
     * after — this only closes the boot-with-HP-plugged gap. */
    int boot_jack = jack_state_read();        /* -1 if never persisted */
    int last_persisted = boot_jack;
    int boot_reasserted = 0;

    for (;;) {
        usleep(200 * 1000);             /* 200 ms cadence */
        drain_events();                 /* event latency ≤ ~200 ms */

        /* Persist jack state when the RT path reports a new CC 115 value. */
        int jp = shim_jack_persist;
        if (jp >= 0 && jp != last_persisted) {
            last_persisted = jp;
            jack_state_write(jp);
        }

        /* Re-assert jack state to Move once, ~5 s after start (Move's firmware
         * is up by then). Prefer the value XMOS actually reported THIS boot
         * (captured at ~f6 into shim_jack_persist) — that's the true current
         * state and handles cables swapped while powered off. Fall back to the
         * persisted file only if XMOS hasn't reported yet this boot. */
        if (!boot_reasserted && tick >= 25) {
            boot_reasserted = 1;
            int v = (shim_jack_persist >= 0) ? shim_jack_persist : boot_jack;
            if (v >= 0) shim_inject_boot_jack = v;
        }

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
