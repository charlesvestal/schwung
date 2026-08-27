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
#include <errno.h>

#include "shim_worker.h"
#include "rt_thread_audit.h"
#include "spi_tally.h"
#include "align_capture.h"
#include "shadow_set_pages.h"
#include "unified_log.h"
#include "usbc_out_gate.h"
#include "shadow_resample.h"   /* usbc_out_persist_enabled */

volatile uint32_t shim_debug_flags = 0;
volatile int shim_pending_sysex_inject = -1;
volatile int shim_inject_boot_jack = -1;
volatile int shim_jack_persist = -1;
volatile int shim_usbc_out_persist = -1;
volatile int shim_usbc_out_replay = -1;
volatile int shim_usbc_out_level = -1;
volatile int shim_usbc_monitor = -1;

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

/* USB-C audio-out source (0 = Mic, 1 = Main Out). Move's firmware forgets this
 * across reboots; we observe it on the wire and re-assert it after boot. */
#define USBC_OUT_STATE_PATH "/data/UserData/schwung/usbc_out_state"

static int usbc_out_state_read(void) {
    FILE *f = fopen(USBC_OUT_STATE_PATH, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    if (v != 0 && v != 1) return -1;
    return v;
}

static void usbc_out_state_write(int v) {
    FILE *f = fopen(USBC_OUT_STATE_PATH, "w");
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
    { "/data/UserData/schwung/main_fx_dump_trigger", SHIM_FLAG_MAIN_FX_DUMP, 1 },
    { "/data/UserData/schwung/rt_thread_audit_on",   SHIM_FLAG_RT_AUDIT,     0 },
    { "/data/UserData/schwung/spi_tally_on",         SHIM_FLAG_SPI_TALLY,    0 },
};

/* ---- SPI frame tally --------------------------------------------------- */

/* Pairs the kernel's per-transfer spi_tx_time (accumulated on the SPI callback,
 * see spi_tally.h) with /proc/ableton/<dev>/irq_count, which only the worker
 * may read — it is file I/O. Off unless armed:
 *     touch /data/UserData/schwung/spi_tally_on
 */

#define ABLSPI_IRQ_COUNT_PATH "/proc/ableton/ablspi0.0/irq_count"

/* Returns 0 and fills *out on success. The counter is printed from an int that
 * only ever increments, so it eventually prints negative; parse it wide, then
 * narrow to exactly 32 bits so spi_tally_fold's modular subtraction sees the
 * same width the kernel counts in. */
static int ablspi_irq_count_read(uint32_t *out)
{
    FILE *f = fopen(ABLSPI_IRQ_COUNT_PATH, "r");
    if (!f) return -1;
    long v = 0;
    int got = (fscanf(f, "%ld", &v) == 1);
    fclose(f);
    if (!got) return -1;
    *out = (uint32_t)v;
    return 0;
}

static void spi_tally_tick(void)
{
    static spi_tally_state_t state;
    static int armed = 0;

    if (!(shim_debug_flags & SHIM_FLAG_SPI_TALLY)) {
        /* Disarmed: drop the accumulator so a later session does not inherit
         * this one's peak transfer time or backlog. */
        if (armed) {
            spi_tally_reset(&shim_spi_tally, &state);
            armed = 0;
        }
        return;
    }

    /* Same rule as the RT-thread audit: do not start measuring into a log that
     * is still dropping writes, or the whole session reads as "armed, nothing
     * found" — which is indistinguishable from a clean result. */
    if (!unified_log_enabled()) return;

    if (!armed) {
        spi_tally_reset(&shim_spi_tally, &state);
        armed = 1;
    }

    uint32_t irqs = 0;
    if (ablspi_irq_count_read(&irqs) != 0) {
        /* A tally that cannot read the counter must SAY so — reporting frames
         * with no IRQ side would look like a clean zero-backlog result while
         * measuring only half of the comparison that is the entire point. */
        static int moaned = 0;
        if (!moaned) {
            moaned = 1;
            unified_log("shim", LOG_LEVEL_ERROR,
                        "spi-tally: armed but " ABLSPI_IRQ_COUNT_PATH
                        " is unreadable — NO backlog measurement is running");
        }
        return;
    }

    spi_tally_sample_t s;
    spi_tally_fold(&state, &shim_spi_tally, irqs, &s);

    char line[256];
    if (s.late) {
        spi_tally_format_late(&s, line, sizeof(line));
        unified_log("shim", LOG_LEVEL_WARN, line);
    }
    spi_tally_format(&s, line, sizeof(line));
    unified_log("shim", LOG_LEVEL_INFO, line);
}

/* ---- realtime-thread audit ------------------------------------------- */

/* Module entry points run on the SPI callback at SCHED_FIFO 90, and POSIX
 * inherits scheduling by default — so a pthread_create from create_instance or
 * set_param yields a worker born at FIFO 90 that starves Move's Link Main
 * (FIFO 35). It also inherits the parent's `comm`, so it reports as
 * "Audio Main/SPI" and is invisible in top or any thread list. See
 * rt_thread_audit.h; the detector is a set diff over tids for that reason.
 *
 * Reading /proc is file I/O, which is why this lives on the worker and not in
 * the callback. Off unless armed:
 *     touch /data/UserData/schwung/rt_thread_audit_on
 */

static char rt_audit_module[64];
static volatile int rt_audit_module_seq;

void shim_rt_audit_note_module(const char *id)
{
    /* RT-safe: bounded copy, no allocation, no lock. */
    if (!id || !id[0]) {
        rt_audit_module[0] = '\0';
    } else {
        size_t n = strnlen(id, sizeof(rt_audit_module) - 1);
        memcpy(rt_audit_module, id, n);
        rt_audit_module[n] = '\0';
    }
    __sync_fetch_and_add(&rt_audit_module_seq, 1);
}

static void rt_audit_tick(void)
{
    static rt_thread_info_t prev[RT_AUDIT_MAX_THREADS];
    static int prev_n = 0;
    static int have_baseline = 0;
    /* The pre-module snapshot, kept separately from `prev`: Move's own audio
     * threads are permanently busy at FIFO 70 and would otherwise be reported
     * as burners on every single tick, burying the finding. */
    static rt_thread_info_t base[RT_AUDIT_MAX_THREADS];
    static int base_n = 0;
    static long clk_hz = 0;

    /* CPU accounting is in whole clock ticks (10 ms at the usual USER_HZ 100),
     * so the floor cannot usefully go below one tick. 20 ms in a ~1 s window is
     * 2% of a core — well under what starves `Link Main`, and high enough that
     * an idle thread never trips it. */
    const int RT_BURN_FLOOR_MS = 20;
    const int RT_BURN_WINDOW_MS = 1000;   /* nominal; the tick is ~1 Hz */

    if (!(shim_debug_flags & SHIM_FLAG_RT_AUDIT)) {
        /* Disarmed: drop the baseline so re-arming starts clean rather than
         * diffing against a snapshot from minutes ago. */
        have_baseline = 0;
        prev_n = 0;
        return;
    }

    /* Do not latch a baseline the log will not accept.
     *
     * The baseline is emitted ONCE and is the whole report for anything
     * already loaded, so losing it loses the finding. unified_log only starts
     * accepting after it notices debug_log_on, which it rechecks every 100
     * calls — so arming both flags together, or leaving rt_thread_audit_on in
     * place across a reboot, latched the baseline into a log that was still
     * dropping writes. The audit then ran for twelve minutes reporting
     * nothing, which is indistinguishable from a clean result. Wait for the
     * log instead; the audit is a diagnostic and has nothing to do until
     * someone can read it. */
    if (!unified_log_enabled()) return;

    rt_thread_info_t cur[RT_AUDIT_MAX_THREADS];
    int cur_n = rt_thread_audit_scan(cur, RT_AUDIT_MAX_THREADS);

    char line[256];

    /* A scan that cannot read /proc must SAY so. Returning quietly here reads
     * downstream as "armed, nothing realtime found" — a false all-clear, which
     * is the one answer this tool must never give. */
    if (cur_n < 0) {
        static int moaned = 0;
        if (!moaned) {
            moaned = 1;
            snprintf(line, sizeof(line),
                     "rt-audit: armed but /proc/self/task is unreadable (errno=%d) — NO audit is running",
                     errno);
            unified_log("shim", LOG_LEVEL_ERROR, line);
        }
        return;
    }

    if (!have_baseline) {
        /* Report the whole realtime set once. Arming mid-session cannot
         * retroactively see a thread inherited at boot, so the baseline IS the
         * finding for anything already loaded — printing only the diff would
         * silently exonerate every module in the current set. */
        snprintf(line, sizeof(line),
                 "rt-audit: armed — %d thread(s), %d realtime (baseline)",
                 cur_n, rt_thread_count_realtime(cur, cur_n));
        unified_log("shim", LOG_LEVEL_INFO, line);

        for (int i = 0; i < cur_n; i++) {
            if (!rt_thread_is_realtime(&cur[i])) continue;
            char desc[224];
            rt_thread_format(&cur[i], NULL, desc, sizeof(desc));
            snprintf(line, sizeof(line), "rt-audit: baseline %s", desc);
            unified_log("shim", LOG_LEVEL_INFO, line);
        }

        if (cur_n >= RT_AUDIT_MAX_THREADS)
            unified_log("shim", LOG_LEVEL_WARN,
                        "rt-audit: thread table full — some threads not scanned");

        memcpy(prev, cur, sizeof(rt_thread_info_t) * (size_t)cur_n);
        prev_n = cur_n;
        memcpy(base, cur, sizeof(rt_thread_info_t) * (size_t)cur_n);
        base_n = cur_n;
        clk_hz = sysconf(_SC_CLK_TCK);
        if (clk_hz <= 0) clk_hz = 100;
        have_baseline = 1;
        return;
    }

    rt_thread_info_t found[RT_AUDIT_MAX_THREADS];
    int n = rt_thread_new_realtime(prev, prev_n, cur, cur_n,
                                   found, RT_AUDIT_MAX_THREADS);
    for (int i = 0; i < n; i++) {
        char desc[224];
        rt_thread_format(&found[i],
                         rt_audit_module[0] ? rt_audit_module : NULL,
                         desc, sizeof(desc));
        snprintf(line, sizeof(line), "rt-audit: NEW realtime thread %s", desc);
        unified_log("shim", LOG_LEVEL_WARN, line);
    }

    /* WHICH threads exist is the suspect list; how much CPU they BURN at
     * realtime priority is the harm. `Link Main` runs at FIFO 35 and only gets
     * what a FIFO 70 thread leaves it, so a parked worker costs nothing and a
     * sample loader costs everything. Report the second. */
    rt_thread_burn_t burn[RT_AUDIT_MAX_THREADS];
    int bn = rt_thread_burners(base, base_n, prev, prev_n, cur, cur_n,
                               (int)clk_hz, RT_BURN_FLOOR_MS,
                               burn, RT_AUDIT_MAX_THREADS);
    for (int i = 0; i < bn; i++) {
        char desc[224];
        rt_thread_format_burn(&burn[i],
                              rt_audit_module[0] ? rt_audit_module : NULL,
                              RT_BURN_WINDOW_MS, desc, sizeof(desc));
        snprintf(line, sizeof(line), "rt-audit: %s", desc);
        unified_log("shim", LOG_LEVEL_WARN, line);
    }

    memcpy(prev, cur, sizeof(rt_thread_info_t) * (size_t)cur_n);
    prev_n = cur_n;
}

/* Knob-touch ground truth — see the touch trace block in schwung_shim.c.
 * A plain int rather than a shim_debug_flags bit because the SPI callback
 * reads it on every note event and a plain load is the cheapest thing it can
 * do; correctness does not depend on when the change is observed. */
extern int shim_touch_trace_on;
void shim_touch_trace_drain(void);

/* ---- align capture ---------------------------------------------------- */

/* The align dump is armed HERE, not on the SPI callback, because arming
 * allocates. The callback only memcpys (align_capture_record); this side does
 * the malloc, the fopen and the fwrite. See align_capture.h.
 *
 * The trigger file's contents are the capture length in seconds; empty or
 * unparseable means ALIGN_CAPTURE_DEFAULT_SECONDS. A single 2.9 s snapshot was
 * too short to tell a real signal from run-to-run variance — the measured
 * splice ratio moved between 1.01x and 5.61x across five consecutive captures
 * of the same configuration.
 */
#define ALIGN_CAPTURE_TRIGGER_PATH "/data/UserData/schwung/align_dump_trigger"
#define ALIGN_CAPTURE_DEFAULT_SECONDS 30

extern align_capture_t g_align_capture;

static void align_capture_tick(void) {
    /* Finish any capture already in flight before starting another. */
    align_capture_poll(&g_align_capture);

    if (access(ALIGN_CAPTURE_TRIGGER_PATH, F_OK) != 0) return;

    int seconds = 0;
    FILE *f = fopen(ALIGN_CAPTURE_TRIGGER_PATH, "r");
    if (f) {
        if (fscanf(f, "%d", &seconds) != 1) seconds = 0;
        fclose(f);
    }
    unlink(ALIGN_CAPTURE_TRIGGER_PATH);
    if (seconds <= 0) seconds = ALIGN_CAPTURE_DEFAULT_SECONDS;

    /* Four streams: the two summands, the slot's post-FX output, and the
     * finished mailbox. Inputs alone cannot tell "Move sent bad audio" from
     * "we damaged good audio" — capture the chain, not its ends. */
    static const char *const paths[4] = {
        "/data/UserData/schwung/slot0_move_track.pcm",
        "/data/UserData/schwung/slot0_synth_src.pcm",
        "/data/UserData/schwung/slot0_post_fx.pcm",
        "/data/UserData/schwung/mailbox_out.pcm",
    };
    uint32_t samples = (uint32_t)seconds * 44100u * 2u;
    if (align_capture_arm(&g_align_capture, paths, 4, samples) == 0) {
        unified_log("shim", LOG_LEVEL_INFO,
                    "align capture armed: %d s per stream", seconds);
    } else {
        /* Almost always "a capture is already running" — say so rather than
         * leaving the user to wonder why the trigger did nothing. */
        unified_log("shim", LOG_LEVEL_WARN,
                    "align capture NOT armed (already running, or out of memory)");
    }
}

static void poll_flags(void) {
    shim_touch_trace_on =
        (access("/data/UserData/schwung/touch_trace_on", F_OK) == 0);

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
        case SHIM_EVT_OVERTAKE_DSP_LOAD:
            if (worker_hooks.overtake_dsp_load_pending) worker_hooks.overtake_dsp_load_pending();
            break;
        case SHIM_EVT_OVERTAKE_DSP_FREE:
            if (worker_hooks.overtake_dsp_free_pending) worker_hooks.overtake_dsp_free_pending();
            break;
        default:
            break;
        }
    }
}

/* ---- thread ------------------------------------------------------------ */

/* Report ROUTE_EXTERNAL ring-full drops at ~1 Hz, and only when there are any.
 * Silent on an idle device by construction: no drops, no line. Reports the
 * DELTA and the running total, because "is it still dropping?" is the question
 * a stale motor raises and a cumulative counter alone cannot answer. */
static void ext_midi_drop_tick(void)
{
    static int last_total = 0;
    int total = shim_ext_midi_drops;
    int delta = total - last_total;
    if (delta <= 0) { last_total = total; return; }
    last_total = total;

    char msg[128];
    snprintf(msg, sizeof(msg),
             "ext-midi: %d MIDI_OUT ring drop(s) this window (%d total) - "
             "mailbox saturated, external CC out is lagging",
             delta, total);
    LOG_DEBUG("shim", msg);
}

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

    /* USB-C audio-out arbitration. The gate decides what is Move's boot
     * default and what is the user; see usbc_out_gate.h for why that cannot be
     * a deadline. `usbc_last_fed` is the edge detector for the level the RT
     * path publishes — it only ever writes on a change, so feeding the gate
     * once per distinct value is exactly one call per real transition. */
    usbc_gate_t usbc_gate;
    usbc_gate_init(&usbc_gate, usbc_out_state_read());  /* -1 if never persisted */
    int usbc_last_fed = -1;

    for (;;) {
        usleep(200 * 1000);             /* 200 ms cadence */
        drain_events();                 /* event latency ≤ ~200 ms */
        shim_touch_trace_drain();       /* file I/O for the SPI callback */

        /* Persist jack state when the RT path reports a new CC 115 value. */
        int jp = shim_jack_persist;
        if (jp >= 0 && jp != last_persisted) {
            last_persisted = jp;
            jack_state_write(jp);
        }

        /* Feed the USB-C arbitration gate. Two things put values on the wire
         * that carry no user intent: Move's own Mic default at boot, and our
         * re-assert echoing back (the shim's SysEx emit runs earlier in the
         * same pre_transfer than its scan, so scan cannot tell our bytes from
         * Move's). Neither may reach the state file.
         *
         * This used to be a ~7 s deadline, which raced Move's assert: the
         * worker's clock starts when MoveOriginal opens the SPI device, while
         * Move's assert floats with boot load, so a slow boot landed it on the
         * trusting side and wrote Mic over a stored Main Out — reverting in
         * session and forgetting across the reboot. The gate replaces the
         * deadline with the one thing that genuinely separates the two: we
         * only ever re-assert Main Out, so during the boot window an observed
         * Mic can only have come from Move. */
        int up = shim_usbc_out_persist;
        if (up >= 0 && up != usbc_last_fed) {
            usbc_last_fed = up;
            usbc_gate_out_t act = {0};
            usbc_gate_observe(&usbc_gate, up, &act);
            if (act.replay) {
                shim_usbc_out_replay = act.replay_value;
                unified_log("shim", LOG_LEVEL_DEBUG,
                            "USB-C out: Move asserted Mic over a stored Main Out (boot) — re-asserting");
            }
            if (act.persist) usbc_out_state_write(act.persist_value);
        }

        /* Defend against Move's sampling page clearing monitoring behind our
         * back. It emits a lone 37 12 to set bit0 (the USB-C input select) and
         * carries bit1 from its own stale "Mic" UI state, which reverts the
         * hardware while 37 14 still reads Main Out — so there is no edge for
         * the observe path above to see. Debounced inside the gate so the
         * leading half of a split 37 12 / 37 14 Mic selection is not mistaken
         * for it. */
        {
            usbc_gate_out_t act = {0};
            usbc_gate_tick_monitor(&usbc_gate, shim_usbc_out_level,
                                   shim_usbc_monitor, &act);
            if (act.replay && usbc_out_persist_enabled) {
                shim_usbc_out_replay = act.replay_value;
                unified_log("shim", LOG_LEVEL_DEBUG,
                            "USB-C out: monitoring cleared by a lone 37 12 — re-asserting Main Out");
            }
        }

        /* Backstop: on a boot where Move never asserts at all, the gate would
         * otherwise stay closed and silently swallow every user change for the
         * rest of the session. Opening it persists nothing by itself. */
        if (tick == 300) usbc_gate_force_settle(&usbc_gate);   /* ~60 s */

        /* Re-assert jack state to Move once, ~5 s after start (Move's firmware
         * is up by then). Prefer the value XMOS actually reported THIS boot
         * (captured at ~f6 into shim_jack_persist) — that's the true current
         * state and handles cables swapped while powered off. Fall back to the
         * persisted file only if XMOS hasn't reported yet this boot. */
        if (!boot_reasserted && tick >= 25) {
            boot_reasserted = 1;
            int v = (shim_jack_persist >= 0) ? shim_jack_persist : boot_jack;
            if (v >= 0) shim_inject_boot_jack = v;

            /* Re-assert the USB-C audio-out source too. The gate puts nothing
             * on the wire when the stored value is Mic — that's Move's own
             * boot default, so there is nothing to correct.
             *
             * With restore switched off in Global Settings the shim drops the
             * replay, so defending would mean guarding a re-assert that never
             * reaches the wire: the gate would spend its whole budget on
             * Move's assert and stay shut meanwhile. Restore-off still
             * *remembers* (the setting governs only whether we re-assert), so
             * open the gate instead and let the ordinary differs-from-stored
             * test run from the start. */
            if (usbc_out_persist_enabled) {
                usbc_gate_out_t act = {0};
                usbc_gate_boot_replay(&usbc_gate, &act);
                if (act.replay) shim_usbc_out_replay = act.replay_value;
            } else {
                usbc_gate_force_settle(&usbc_gate);
            }
        }

        if (tick % 5 == 0) poll_flags();          /* ~1 Hz */
        if (tick % 5 == 0) rt_audit_tick();       /* ~1 Hz, no-op unless armed */
        if (tick % 5 == 0) spi_tally_tick();      /* ~1 Hz, no-op unless armed */
        align_capture_tick();                    /* 5 Hz: arm on trigger, drain when full */
        if (tick % 5 == 0) ext_midi_drop_tick();  /* ~1 Hz, silent unless dropping */
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
