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
#include "ui_midi_out_carry.h" /* UI_MIDI_CARRY_PACKETS */
#include "perf_snapshot.h"
#include "shadow_shm_util.h"
#include "shadow_chain_mgmt.h"  /* shadow_fx_load_worker_tick */

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

/* ---- /schwung-perf publish -------------------------------------------- */

/* The CPU page's frame-budget panel reads this. Retried until it succeeds
 * rather than attempted once: /dev/shm may not be writable at the instant the
 * shim initialises, and a single silent failure would leave the page reporting
 * "shim not running" forever with nothing to say why.
 *
 * ALWAYS ON — no arming flag. The timing it publishes is already collected
 * unconditionally, so this costs one mmap and two stores per ~1000 frames.
 * Arming it would make the page blank by default, which reads as a broken
 * build (see docs/DIAGNOSTICS.md on the SPI tally's 20 s silence). */
extern void shim_perf_publish_to(volatile schwung_perf_snapshot_t *dst);

void perf_shm_attach_tick(void)
{
    static schwung_perf_snapshot_t *shm = NULL;
    static int moaned = 0;
    if (shm) return;

    shm = (schwung_perf_snapshot_t *)shadow_shm_map(
        SHM_SCHWUNG_PERF, SCHWUNG_PERF_SHM_SIZE, 1, 1);
    if (!shm) {
        if (!moaned) {
            moaned = 1;
            unified_log("shim", LOG_LEVEL_WARN,
                        "perf: could not create " SHM_SCHWUNG_PERF
                        " - the manager's CPU page will report no shim");
        }
        return;
    }

    /* Fault every page in before the SPI callback ever writes here. A first
     * touch from the callback is a page fault on the realtime thread. */
    memset(shm, 0, SCHWUNG_PERF_SHM_SIZE);

    shim_perf_publish_to((volatile schwung_perf_snapshot_t *)shm);
    unified_log("shim", LOG_LEVEL_INFO,
                "perf: publishing to " SHM_SCHWUNG_PERF " (v%u, %zu bytes)",
                SCHWUNG_PERF_VERSION, sizeof(schwung_perf_snapshot_t));
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

/* Clip-state readout. Diagnostic: prints the decoded table once a second so
 * the decode can be checked against what the device is visibly doing -- which
 * is the only way to find out whether the LED protocol was read correctly.
 *   arm:  touch /data/UserData/schwung/clip_state_on
 *   read: /data/UserData/schwung/clip_state.log
 */
#include "clip_state.h"
#include <sys/stat.h>
#include "clip_regions.h"
#include "shadow_led_queue.h"
extern int shadow_transport_pulses;
extern int sampler_transport_playing;
/* Is this thread alive at all? Three separate worker-driven diagnostics went
 * quiet at once and I argued about the cause instead of measuring it. This
 * answers it in one deploy: it needs no arming file and touches nothing. */
static void worker_heartbeat(void)
{
    static unsigned n = 0;
    if (n++ % 25) return;              /* ~5 s */
    FILE *f = fopen("/data/UserData/schwung/worker_alive.txt", "w");
    if (!f) return;
    fprintf(f, "worker tick %u\n", n);
    fclose(f);
}

/* Seed clip identity and loop geometry from Song.abl when the set changes.
 *
 * Without this, nothing is known until the user visits Session mode -- and if
 * MIDI Start arrives first, every track stays unanchored PERMANENTLY, because
 * there was no identity for the Start to anchor. Seeding before 0xFA is the
 * whole point: it puts identity in place so the Start can do its job.
 *
 * Worker thread only: this reads and parses a file over 1 MB. */
static void clip_phase_check_reset(void);   /* defined below; used by the region reload */
static clip_regions_t g_regions;

/* Read by the SPI callback (shadow_slot_clip_phase) as well as by this
 * worker. Returns the table itself rather than a copy: copying 1 MB of parse
 * output per block is not realtime, and the caller only ever reads a handful
 * of doubles out of it. */
const clip_regions_t *shadow_clip_regions(void) { return &g_regions; }

/* WHICH CLIPS WERE DELETED BY THE LAST RE-PARSE, and a counter saying it is
 * news. The worker publishes; the SPI callback's per-slot loop reads and pushes
 * it into each chain instance through chain_set_clip_deleted().
 *
 * The worker must NOT push it itself. v2_set_param is a module entry point --
 * i.e. the SPI callback -- and the chain instance is only safe because RT is
 * its single writer; reaching in from here races every reader the callback
 * owns. This is the same crossing g_regions already uses, in the same
 * direction.
 *
 * A GENERATION COUNTER, NOT A FLAG. chain_bus.c records why: a flag can be
 * resurrected by a worker preempted between writing it and the consumer
 * clearing it, and there is no clearing at all on this side. A counter is
 * monotonic, so "have I seen this?" is a comparison the consumer answers out
 * of its own state and the producer never has to unwrite.
 *
 * The mask is ASSIGNED, not accumulated, and the generation is bumped LAST:
 * a generation the callback can see always has its own mask already in place.
 * That ordering is also what makes the un-orphan rule sound -- g_regions no
 * longer holds the deleted clip by the time the deletion is visible, so the
 * position cannot report a fingerprint that would immediately un-orphan the
 * lane. Deploying only means a batch is lost if two re-parses land inside one
 * audio block; the re-parse poll is ~1.4 s and a block is ~2.9 ms. */
static volatile uint32_t g_clip_deleted_mask;
static volatile uint32_t g_clip_deleted_gen;

uint32_t shadow_clip_deleted_generation(void) { return g_clip_deleted_gen; }
uint32_t shadow_clip_deleted_mask(void) { return g_clip_deleted_mask; }

/* Phase check tallies, per track. The step editor shows ONE track, so only
 * one of these should score highly -- which track it is falls out of the
 * result rather than having to be known in advance. A track that is simply
 * wrong scores near zero; a track a beat out scores near zero too, which is
 * the point (it would look perfect on any count-and-wrap test). */
static unsigned g_ph_total, g_ph_hit[CLIP_TRACKS], g_ph_seen[CLIP_TRACKS];
/* Bar-level tallies. The step comparison above is mod 16 steps = mod ONE BAR,
 * so it scores 100% on a lane anchored exactly a bar out. Comparing our
 * computed page against Move's announced "Bar N" is what actually catches
 * that -- and it needs an announcement, so it only runs once the user has
 * changed page at least once. */
static unsigned g_bar_seen[CLIP_TRACKS], g_bar_hit[CLIP_TRACKS];
static int      g_bar_lastdiff[CLIP_TRACKS];

/* The last few DISAGREEMENTS, kept so a 3% miss rate can be explained rather
 * than assumed. The standing hypothesis is a boundary race: the step LED and
 * the pulse counter are not sampled together, so an event landing within a
 * pulse or two of a step boundary can be read one step either side.
 *
 * That predicts three things, all visible here: the step difference is
 * ALWAYS +/-1, never more; the distance to the nearest step boundary is small
 * (a step is 6 pulses at 1/16); and the same event fails both columns. Any of
 * those breaking refutes it. */
#define PH_MISS_RING 16
typedef struct {
    uint32_t pulses;
    int  idx;          /* the lit step button                 */
    int  step;         /* the step we computed                */
    int  diff;         /* computed - lit, in steps            */
    int  to_boundary;  /* pulses to the nearest step boundary */
    int  bar_missed;   /* did the bar column miss the same event */
    int  bar_scored;   /* was the bar column even scoring it     */
} ph_miss_t;
static ph_miss_t g_ph_miss[PH_MISS_RING];
static unsigned  g_ph_miss_n;
extern volatile int shadow_editor_bar;
extern volatile unsigned shadow_editor_bar_seq;

/* Move's step-editor page PER TRACK, 1-based, 0 = unknown.
 *
 * The page belongs to the CLIP, not to the device: each clip has its own loop
 * length, so its own page count, so its own remembered page. Switching track
 * shows that track's clip at the page it was left on, with no announcement.
 * A single global bar therefore describes whichever track was last paged
 * while the playhead being scored belongs to the track on screen now -- which
 * is why a global made bar-level agreement collapse to ~65%, the rate at
 * which two unrelated pages happen to coincide. */
static int      g_editor_bar[CLIP_TRACKS];
/* Pulse at which we NOTICED the set change, and how wide that guess is. The
 * detection is a ~1.4 s poll, so it brackets the real start rather than
 * naming it -- which is exactly what clip_state_solve_common_start needs
 * alongside a playhead sighting. */
static uint32_t g_set_change_pulse;
static int      g_set_change_valid;
#define SET_CHANGE_BRACKET_PULSES 84   /* ~1.75 s at 120 BPM, poll + slack */
static unsigned g_editor_bar_seq_seen;
static int      g_ph_lastdiff[CLIP_TRACKS];
static char g_set_name[128];
static char g_set_uuid[128];
static void clip_regions_tick(void)
{
    static char last_set[320];    /* set + mtime + size: geometry changed  */
    static char last_ident[256];  /* set alone: WHICH set we are looking at */
    static unsigned n = 0;
    if (n++ % 7) return;                   /* ~1.4 s, matching the set poll */

    char uuid[128] = {0}, name[128] = {0};
    FILE *f = fopen("/data/UserData/schwung/active_set.txt", "r");
    if (!f) return;
    if (!fgets(uuid, sizeof(uuid), f)) { fclose(f); return; }
    if (!fgets(name, sizeof(name), f))  { fclose(f); return; }
    fclose(f);
    uuid[strcspn(uuid, "\r\n")] = 0;
    name[strcspn(name, "\r\n")] = 0;
    if (!uuid[0] || !name[0]) return;

    char path[512];
    snprintf(path, sizeof(path),
             "/data/UserData/UserLibrary/Sets/%s/%s/Song.abl", uuid, name);

    /* Key on the FILE, not just the set name. Editing a set -- changing a
     * track's instrument, adding a clip -- leaves the name identical while
     * the geometry changes underneath, and a name-only check served stale
     * loop lengths until the next set change. Observed on hardware as a
     * track silently losing its phase after being edited.
     *
     * A brand-new clip may still be absent: Move holds it in memory until it
     * saves. That is a missing loop length, which reads as "no phase" -- the
     * honest answer -- not as a wrong one. */
    struct stat sb;
    if (stat(path, &sb) != 0) return;
    char key[320];
    snprintf(key, sizeof(key), "%s/%s|%lld|%lld", uuid, name,
             (long long)sb.st_mtime, (long long)sb.st_size);
    if (strcmp(key, last_set) == 0) return;  /* unchanged */
    clip_regions_t rg;
    if (!clip_regions_parse_file(path, &rg)) return;   /* leave the old one */

    char ident[256];
    snprintf(ident, sizeof(ident), "%s/%s", uuid, name);
    int set_changed = (strcmp(ident, last_ident) != 0);

    snprintf(last_set, sizeof(last_set), "%s", key);
    snprintf(last_ident, sizeof(last_ident), "%s", ident);
    snprintf(g_set_name, sizeof(g_set_name), "%s", name);
    snprintf(g_set_uuid, sizeof(g_set_uuid), "%s", uuid);
    clip_regions_t before = g_regions;
    g_regions = rg;

    clip_state_t *st = clip_state_mutable();
    if (!st) return;

    /* A DIFFERENT SET INVALIDATES EVERYTHING. Without this the previous
     * set's identities and anchors survive into the new one -- and because
     * seed_state deliberately skips tracks that already have identity, the
     * file could not correct them. Observed on hardware twice: a set with no
     * clips still showing the old set's anchors, and a freshly loaded set
     * disagreeing with its own file until Session mode was visited.
     *
     * A mere EDIT of the same set must NOT reset: the geometry changed, what
     * is playing did not, and wiping identity there would throw away a live
     * observation in favour of a file that may not have been saved yet. */
    /* A clip deleted out from under us leaves identity asserting a clip that
     * no longer exists. Observed: T4 kept reporting clip 6 after it was
     * deleted. Compared against the PREVIOUS parse so a newly copied clip --
     * also absent from the file until Move saves -- is not mistaken for one
     * that was removed. */
    if (!set_changed) {
        uint32_t deleted = 0;
        clip_regions_forget_deleted(&before, &g_regions, st, &deleted);
        /* Only publish when something actually went away. A generation bumped
         * on every re-parse would have the callback walk 32 bits on each of
         * Move's periodic saves to discover nothing, and -- worse -- would make
         * "a new generation" stop meaning "a clip was deleted", which is the
         * only thing the consumer can act on.
         *
         * Mask first, generation last: see the declaration. */
        if (deleted) {
            g_clip_deleted_mask = deleted;
            g_clip_deleted_gen++;
        }
    }

    /* Only a REAL geometry change invalidates earlier samples. Resetting on
     * every re-parse wiped the tally on each of Move's periodic saves, so it
     * never accumulated past a handful of events -- the instrument looked
     * broken and was in fact measuring nothing. */
    if (clip_regions_geometry_differs(&before, &g_regions))
        clip_phase_check_reset();

    if (set_changed) {
        clip_state_reset(st);
        g_set_change_pulse = (uint32_t)shadow_transport_pulses;
        g_set_change_valid = sampler_transport_playing ? 1 : 0;
        clip_phase_check_reset();
        memset(g_editor_bar, 0, sizeof(g_editor_bar));
    }

    /* The file SEEDS; the LEDs OVERRIDE. seed_state skips any track we have
     * already observed and never sets an anchor. */
    clip_regions_seed_state(&g_regions, st);

    /* ...and if a Start is still pending for a track we have only just
     * identified, honour it now. A set load restarts the transport at once
     * while this poll runs ~1.4 s later, so without this every track sits at
     * "phase unknown" until the user presses Play again. */
    clip_state_anchor_pending(st, (uint32_t)shadow_transport_pulses,
                              sampler_transport_playing);

    /* Seed each track's remembered page from its current clip. Song.abl keeps
     * stepEditorScrollPosition PER CLIP, which is the same fact as the page
     * being per track -- and it means a track we have never heard a "Bar N"
     * for still has a page. Only seeds where we do not already know one from
     * an announcement, which is live and therefore better. */
    for (int t = 0; t < CLIP_TRACKS; t++) {
        if (g_editor_bar[t] > 0) continue;
        const clip_track_state_t *tr = &st->tracks[t];
        if (!tr->identity_valid || tr->clip_slot < 0) continue;
        const clip_region_t *r = &g_regions.slots[t][tr->clip_slot];
        if (!r->exists || !r->have_scroll) continue;
        g_editor_bar[t] = (int)(r->scroll_beats / 4.0) + 1;
    }
}

/* Zero the tallies. A score is only meaningful over a run with FIXED
 * geometry: editing a clip's loop mid-run makes our length wrong until Move
 * saves, and a wrong length is itself a bar-level error -- so the samples
 * either side of an edit measure different things and averaging them answers
 * nothing. */
static void clip_phase_check_reset(void)
{
    g_ph_total = 0;
    memset(g_ph_hit, 0, sizeof(g_ph_hit));
    memset(g_ph_seen, 0, sizeof(g_ph_seen));
    memset(g_ph_lastdiff, 0, sizeof(g_ph_lastdiff));
    memset(g_bar_seen, 0, sizeof(g_bar_seen));
    memset(g_bar_hit, 0, sizeof(g_bar_hit));
    memset(g_bar_lastdiff, 0, sizeof(g_bar_lastdiff));
    g_ph_miss_n = 0;
    memset(g_ph_miss, 0, sizeof(g_ph_miss));
}

/* Apply a new "Bar N" to the track it describes: the selected one. Keyed on
 * the sequence number rather than the value, so paging away and back to the
 * same bar still counts as an announcement. */
static void clip_editor_bar_tick(void)
{
    unsigned seq = shadow_editor_bar_seq;
    if (seq == g_editor_bar_seq_seen) return;
    g_editor_bar_seq_seen = seq;
    int t = clip_selected_track();
    int bar = shadow_editor_bar;
    if (t >= 0 && bar > 0) g_editor_bar[t] = bar;
}

static void clip_phase_check_tick(void)
{
    clip_editor_bar_tick();

    /* A reset requested from the debug page. */
    if (access("/data/UserData/schwung/clip_check_reset", F_OK) == 0) {
        clip_phase_check_reset();
        remove("/data/UserData/schwung/clip_check_reset");
    }

    const clip_state_t *cs = clip_state_current();
    if (!cs || !g_regions.valid) return;
    double res = g_regions.step_resolution > 0 ? g_regions.step_resolution : 0.25;

    clip_playhead_ev_t ev[32];
    int n;
    while ((n = clip_playhead_take(ev, 32)) > 0) {
        for (int i = 0; i < n; i++) {
            g_ph_total++;
            /* Before scoring: if the SELECTED track has identity but no
             * anchor, solve it from this very sighting. Loading a set while
             * the transport keeps running produces no Start and no witnessed
             * launch, so without this the track is stuck at "phase unknown"
             * indefinitely. Only the selected track, because only its page is
             * the one the playhead belongs to. */
            {
                int sel = clip_selected_track();
                clip_state_t *mst = clip_state_mutable();
                if (sel >= 0 && mst && g_editor_bar[sel] > 0) {
                    clip_track_state_t *str = &mst->tracks[sel];
                    if (str->identity_valid && str->clip_slot >= 0 &&
                        !str->anchor_valid) {
                        const clip_region_t *sr =
                            &g_regions.slots[sel][str->clip_slot];
                        /* Every clip in a set begins together, so there is
                         * ONE start. A sighting gives it modulo this track's
                         * loop; the set-change poll brackets it. Together
                         * they name it, and then EVERY track can use it
                         * whatever its loop length. */
                        double pos = ((double)(g_editor_bar[sel] - 1) * 16.0
                                      + (double)ev[i].idx) * res;
                        uint32_t start;
                        if (g_set_change_valid &&
                            clip_state_solve_common_start(
                                ev[i].pulses, pos, sr->loop_len,
                                g_set_change_pulse, SET_CHANGE_BRACKET_PULSES,
                                &start)) {
                            clip_state_apply_common_start(mst, start);
                        } else {
                            /* No usable bracket (short loop, or the set change
                             * was not observed while running): fall back to
                             * anchoring just this track from the sighting. */
                            clip_state_derive_anchor(str, ev[i].pulses,
                                                     g_editor_bar[sel], ev[i].idx,
                                                     res, sr->loop_start,
                                                     sr->loop_len);
                        }
                    }
                }
            }

            for (int t = 0; t < CLIP_TRACKS; t++) {
                const clip_track_state_t *tr = &cs->tracks[t];
                if (!tr->identity_valid || tr->clip_slot < 0) continue;
                const clip_region_t *r = &g_regions.slots[t][tr->clip_slot];
                double ph;
                if (!clip_phase_beats(tr, ev[i].pulses, r->loop_start,
                                      r->loop_len, &ph))
                    continue;
                g_ph_seen[t]++;
                int step = (int)((ph - r->loop_start) / res + 0.5);
                int pred = ((step % 16) + 16) % 16;
                int diff = pred - (int)ev[i].idx;
                if (diff > 8) diff -= 16;
                if (diff < -8) diff += 16;
                g_ph_lastdiff[t] = diff;
                if (diff == 0) g_ph_hit[t]++;

                /* Record a disagreement, for the selected track only -- it is
                 * the one whose playhead this is. */
                if (diff != 0 && t == clip_selected_track()) {
                    double step_pulses = res * 24.0;
                    double into = (ph - r->loop_start) / res;   /* in steps */
                    double frac = into - (double)(long)into;    /* 0..1      */
                    int to_b = (int)((frac > 0.5 ? (1.0 - frac) : frac)
                                     * step_pulses + 0.5);
                    ph_miss_t *m = &g_ph_miss[g_ph_miss_n % PH_MISS_RING];
                    m->pulses = ev[i].pulses;
                    m->idx = ev[i].idx;
                    m->step = step;
                    m->diff = diff;
                    m->to_boundary = to_b;
                    m->bar_scored = 0;
                    m->bar_missed = 0;
                    g_ph_miss_n++;
                }

                /* Bar level, and ONLY for the track the step editor is
                 * showing -- a bar describes one clip's page, so comparing it
                 * against another track's phase measures nothing. */
                int bar = (t == clip_selected_track()) ? g_editor_bar[t] : 0;
                /* A DERIVED anchor was computed from this same playhead, so
                 * scoring it here measures the solver's arithmetic, not the
                 * phase. Excluded, or the bar column would read 100% by
                 * construction and stop being evidence. */
                if (tr->anchor_source == CLIP_ANCHOR_DERIVED) bar = 0;
                if (bar > 0) {
                    int page = step / 16;
                    g_bar_seen[t]++;
                    int bdiff = page - (bar - 1);
                    g_bar_lastdiff[t] = bdiff;
                    if (bdiff == 0) g_bar_hit[t]++;
                    /* Tie the bar outcome to the step miss just recorded for
                     * this same event, so "did both columns fail together"
                     * is a fact rather than an inference from two rates. */
                    if (diff != 0 && t == clip_selected_track() && g_ph_miss_n) {
                        ph_miss_t *m = &g_ph_miss[(g_ph_miss_n - 1) % PH_MISS_RING];
                        if (m->pulses == ev[i].pulses) {
                            m->bar_scored = 1;
                            m->bar_missed = (bdiff != 0);
                        }
                    }
                }
            }
        }
        if (n < 32) break;
    }
}

static void clip_state_tick(void)
{
    if (access("/data/UserData/schwung/clip_state_on", F_OK) != 0) return;
    /* The worker ticks at 200 ms; one line a second is enough to read. */
    static unsigned n = 0;
    if (n++ % 5) return;
    /* Opened and closed per line, deliberately. A static FILE* held across a
     * `rm` of the log sends every later write to an unlinked inode, and the
     * reopen was gated on an arming transition that never came -- so the
     * readout goes silent and looks exactly like a dead worker or a broken
     * decode. At 1 Hz the open costs nothing and cannot lie. */
    FILE *fp = fopen("/data/UserData/schwung/clip_state.log", "a");
    if (!fp) return;
    const clip_state_t *cs = clip_state_current();
    if (!cs) { fprintf(fp, "(no cable-0 scan yet)\n"); fclose(fp); return; }
    uint32_t pul = (uint32_t)shadow_transport_pulses;
    /* Move's Record button. Without this, "the arm never fired" and "the lane
     * never recorded" are the same silence, and neither is distinguishable
     * from the other by ear. SOLID is the only state that records; FLASH is
     * armed or counting in; `?` means the button has never reported, which is
     * not the same zero as off. */
    const char *rec = !shadow_rec_arm_seen()  ? "?"     :
                      shadow_rec_arm_recording() ? "SOLID" :
                      shadow_rec_arm_flashing()  ? "FLASH" : "off";
    fprintf(fp, "pul=%-7u rec=%-5s", pul, rec);
    for (int t = 0; t < CLIP_TRACKS; t++) {
        const clip_track_state_t *tr = &cs->tracks[t];
        if (!tr->identity_valid)      fprintf(fp, " | T%d ?        ", t + 1);
        else if (tr->clip_slot < 0)   fprintf(fp, " | T%d -        ", t + 1);
        else if (!tr->anchor_valid)   fprintf(fp, " | T%d c%d ph?   ", t + 1, tr->clip_slot + 1);
        else {
            double ph = 0.0;
            const clip_region_t *r = g_regions.valid
                ? &g_regions.slots[t][tr->clip_slot] : 0;
            if (r && clip_phase_beats(tr, pul, r->loop_start, r->loop_len, &ph))
                fprintf(fp, " | T%d c%d @%-6.2f", t + 1, tr->clip_slot + 1,
                        ph - r->loop_start);
            else {
                /* Anchored but no loop length: elapsed, never a phase. An
                 * invented length would agree with itself and with nothing
                 * on the device. */
                double el = (double)(pul - tr->anchor_pulse) / 24.0;
                fprintf(fp, " | T%d c%d +%-6.2f", t + 1, tr->clip_slot + 1, el);
            }
        }
    }
    fprintf(fp, "\n");
    fclose(fp);

    /* JSON snapshot for the web manager's debug page. Truncated each time --
     * it is a STATE, not a log. Written beside the log rather than into SHM
     * because the manager is a separate process with no mapping for this and
     * a 1 Hz file is plenty for a human watching along. */
    FILE *jf = fopen("/data/UserData/schwung/clip_state.json", "w");
    if (!jf) return;
    fprintf(jf, "{\"pulses\":%u,\"beat\":%.2f,\"record\":\"%s\",\"tracks\":[",
            pul, pul / 24.0, rec);
    for (int t = 0; t < CLIP_TRACKS; t++) {
        const clip_track_state_t *tr = &cs->tracks[t];
        double el = tr->anchor_valid ? (double)(pul - tr->anchor_pulse) / 24.0 : 0.0;
        const clip_region_t *r = (g_regions.valid && tr->identity_valid &&
                                  tr->clip_slot >= 0)
                               ? &g_regions.slots[t][tr->clip_slot] : 0;
        double ph = 0.0;
        int have_ph = r && clip_phase_beats(tr, pul, r->loop_start, r->loop_len, &ph);
        fprintf(jf, "%s{\"track\":%d,\"known\":%s,\"clip\":%d,"
                    "\"anchored\":%s,\"anchor_pulse\":%u,\"anchor_src\":%d,\"elapsed_beats\":%.2f,"
                    "\"loop_len\":%.2f,\"loop_start\":%.2f,"
                    "\"has_phase\":%s,\"phase\":%.2f,\"pos\":%.2f}",
                t ? "," : "", t + 1,
                tr->identity_valid ? "true" : "false",
                tr->identity_valid ? tr->clip_slot + 1 : 0,
                tr->anchor_valid ? "true" : "false",
                tr->anchor_pulse, tr->anchor_source, el,
                r ? r->loop_len : 0.0, r ? r->loop_start : 0.0,
                have_ph ? "true" : "false", ph,
                have_ph ? ph - (r ? r->loop_start : 0.0) : 0.0);
    }
    fprintf(jf, "],\"set\":\"%s\",\"ui_mode\":%d,\"regions_valid\":%s,\"grid\":[",
            g_set_name, cs->last_ui_mode, g_regions.valid ? "true" : "false");
    /* The grid AS WE BELIEVE IT: what the file says exists, what the file
     * restored as selected, and which slot we currently think is live. Shown
     * side by side on purpose -- when the readout disagrees with the device,
     * the useful question is which of the two sources is wrong. */
    for (int t = 0; t < CLIP_TRACKS; t++) {
        for (int s2 = 0; s2 < CLIP_SLOTS; s2++) {
            const clip_region_t *r = g_regions.valid ? &g_regions.slots[t][s2] : 0;
            int live = (cs->tracks[t].identity_valid &&
                        cs->tracks[t].clip_slot == s2);
            fprintf(jf, "%s{\"t\":%d,\"s\":%d,\"exists\":%s,\"file_sel\":%s,\"live\":%s,\"len\":%.2f}",
                    (t || s2) ? "," : "", t + 1, s2 + 1,
                    (r && r->exists) ? "true" : "false",
                    (r && r->is_playing) ? "true" : "false",
                    live ? "true" : "false",
                    r ? r->loop_len : 0.0);
        }
    }
    fprintf(jf, "],\"selected_track\":%d,\"editor_bars\":[%d,%d,%d,%d],\"editor_bar\":%d,\"phase_check\":{\"events\":%u,\"tracks\":[",
            clip_selected_track() + 1,
            g_editor_bar[0], g_editor_bar[1], g_editor_bar[2], g_editor_bar[3],
            shadow_editor_bar, g_ph_total);
    for (int t = 0; t < CLIP_TRACKS; t++)
        fprintf(jf, "%s{\"track\":%d,\"seen\":%u,\"hit\":%u,\"last_diff\":%d,"
                    "\"bar_seen\":%u,\"bar_hit\":%u,\"bar_diff\":%d}",
                t ? "," : "", t + 1, g_ph_seen[t], g_ph_hit[t], g_ph_lastdiff[t],
                g_bar_seen[t], g_bar_hit[t], g_bar_lastdiff[t]);
    fprintf(jf, "],\"misses\":[");
    {
        unsigned n = g_ph_miss_n < PH_MISS_RING ? g_ph_miss_n : PH_MISS_RING;
        unsigned first = g_ph_miss_n - n;
        for (unsigned k = 0; k < n; k++) {
            const ph_miss_t *m = &g_ph_miss[(first + k) % PH_MISS_RING];
            fprintf(jf, "%s{\"pulses\":%u,\"idx\":%d,\"step\":%d,\"diff\":%d,"
                        "\"to_boundary\":%d,\"bar_scored\":%d,\"bar_missed\":%d}",
                    k ? "," : "", m->pulses, m->idx, m->step, m->diff,
                    m->to_boundary, m->bar_scored, m->bar_missed);
        }
    }
    fprintf(jf, "],\"miss_total\":%u}}\n", g_ph_miss_n);
    fclose(jf);
}
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
        case SHIM_EVT_BOOT_HEALTHY:
            if (worker_hooks.boot_healthy) worker_hooks.boot_healthy();
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

/* The same shape for the shadow_ui MIDI ring. A large SysEx burst -- a Yamaha
 * 5F bulk dump is 53 USB-MIDI packets per message -- can outrun the drain, and
 * a dropped packet leaves the message still well-framed and still parseable,
 * so nothing downstream can tell. Only the dump's own declared byte count
 * disagrees. Reporting the rate is what turns that into something a user can
 * see without a capture rig (#358). */
static void ui_midi_drop_tick(void)
{
    static int last_total = 0;
    int total = shim_ui_midi_drops;
    int delta = total - last_total;
    if (delta <= 0) { last_total = total; return; }
    last_total = total;

    char msg[160];
    snprintf(msg, sizeof(msg),
             "ui-midi: %d shadow_ui ring drop(s) this window (%d total) - "
             "MIDI to tools is being lost; a large SysEx burst is outrunning "
             "the %d-packet ring",
             delta, total, SHADOW_UI_MIDI_BYTES / 4);
    LOG_DEBUG("shim", msg);
}

/* And the outbound direction, which had no counter at all until the carry gave
 * the condition a name. This one firing means a module is queueing MIDI faster
 * than the 20-slot mailbox can carry it for long enough to fill a 256-packet
 * backlog — i.e. it is ignoring the `false` that move_midi_external_send()
 * returns once backpressure engages, not merely sending something large. */
static void ui_midi_out_drop_tick(void)
{
    static int last_total = 0;
    int total = shim_ui_midi_out_drops;
    int delta = total - last_total;
    if (delta <= 0) { last_total = total; return; }
    last_total = total;

    char msg[160];
    snprintf(msg, sizeof(msg),
             "ui-midi-out: %d carry drop(s) this window (%d total) - MIDI from "
             "a module is being lost on the way OUT; it is outrunning the "
             "%d-packet carry and ignoring the send() return",
             delta, total, UI_MIDI_CARRY_PACKETS);
    LOG_DEBUG("shim", msg);
}

/*
 * Drain the slow-param ring.
 *
 * WHY IT IS WORTH A LOG LINE OF ITS OWN. `param=7/20051` in the spi_timing
 * block already said a serve took 20 ms; what it could not say is WHICH KEY,
 * and without that the only way forward is a differential experiment against
 * the user's ears. Twice now that has cost a full session (overtake dlopen,
 * then dr32's kit load inside synth:state). The key is in the request; this
 * carries it out.
 *
 * WARN, not DEBUG: unlike the drop counters above, this fires only when
 * something has already overrun the audio budget, so it is never noise.
 */
static void param_slow_tick(void)
{
    param_slow_entry_t e;
    char msg[256];
    int n = 0;
    while (n < PARAM_SLOW_ENTRIES && param_slow_take(&shim_param_slow, &e)) {
        if (param_slow_format(&e, msg, sizeof(msg)) > 0)
            unified_log("shim", LOG_LEVEL_WARN, "%s", msg);
        n++;
    }

    /* Loss is by construction (the callback may not wait for us) but must not
     * be silent: a non-zero count means slow serves are arriving faster than
     * 1 Hz, which is a different and worse finding than any single line above. */
    uint32_t dropped = param_slow_take_dropped(&shim_param_slow);
    if (dropped)
        unified_log("shim", LOG_LEVEL_WARN,
                    "param-slow: %u further slow serve(s) not recorded — they "
                    "are arriving faster than this report drains",
                    (unsigned)dropped);
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
        /* Master FX / send FX module loading: the dlopen, create_instance and
         * module.json parse of a picked module, plus the destroy_instance and
         * dlclose of the one it replaced. All of it used to run on the SPI
         * callback — one 7.7 MB CLAP bundle cost ~708 dropped frames. The RT
         * side only records the request; see shadow_fx_load_request. This
         * thread is already SCHED_OTHER on cores 0-2 and is created from shim
         * init, which is why the loader needs no thread of its own: a
         * pthread_create from a module entry point inherits SCHED_FIFO 70 and
         * starves Move's own Link Main at 35. */
        shadow_fx_load_worker_tick();
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
            if (act.replay) {
                /* REPORT IT EVEN THOUGH WE NO LONGER ACT ON IT (1.3.2).
                 *
                 * This is the exact signature of the field complaint "USB-C
                 * went back to the microphone": 37 14 still reads Main Out, so
                 * Move's own Settings screen still SAYS Main Out, while bit1 —
                 * which is how Main Out actually reaches USB-C — is gone. The
                 * failure is silent at every layer, which is why the reports
                 * arrive as "it needs a reboot" with nothing to go on.
                 *
                 * The gate's verdict is the right trigger rather than a raw
                 * edge: it is already debounced over USBC_GATE_MONITOR_DEBOUNCE
                 * ticks, so the leading half of a split Mic selection cannot
                 * raise a false alarm in the very log a reporter sends us, and
                 * it is already bounded by monitor_replays_left, so one loss
                 * event costs at most USBC_GATE_MAX_REPLAYS lines.
                 *
                 * Logging is still gated on debug_log_on like everything else —
                 * this does not make the failure self-reporting, it makes a
                 * reporter's capture contain the one line that names it. */
                unified_log("shim", LOG_LEVEL_INFO,
                            "USB-C out: monitoring (37 12 bit1) cleared while the source still "
                            "reads Main Out — USB-C is now carrying the mic, and Move's screen "
                            "does not show it");
                if (usbc_out_persist_enabled) {
                    shim_usbc_out_replay = act.replay_value;
                    unified_log("shim", LOG_LEVEL_DEBUG,
                                "USB-C out: monitoring cleared by a lone 37 12 — re-asserting Main Out");
                }
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
        if (tick % 5 == 0) perf_shm_attach_tick();/* ~1 Hz until attached */
        if (tick % 5 == 0) rt_audit_tick();       /* ~1 Hz, no-op unless armed */
        if (tick % 5 == 0) spi_tally_tick();      /* ~1 Hz, no-op unless armed */
        clip_regions_tick();
        clip_phase_check_tick();
        clip_state_tick();
        worker_heartbeat();
        align_capture_tick();                    /* 5 Hz: arm on trigger, drain when full */
        if (tick % 5 == 0) {
            ext_midi_drop_tick();
            ui_midi_drop_tick();
            ui_midi_out_drop_tick();
            param_slow_tick();        /* always on; silent unless one overran */
        }
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
