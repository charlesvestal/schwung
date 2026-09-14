/*
 * lane_trace.h — a lane's state over time, recorded WITHOUT the param channel.
 *
 * WHY THIS EXISTS, because the obvious instrument is the wrong one and cost a
 * user his UI twice in one session.
 *
 * The natural way to watch a lane is to poll `lanes:diag` from schwung-testd.
 * It does not work while a human is playing, and not because of its RATE:
 * `/schwung-param` has ONE request slot and shadow_ui is the other producer on
 * it, so every testd read TAKES THE SLOT THE UI DRAWS THROUGH and holds it for
 * up to TESTD_PARAM_TIMEOUT_MS (5 s). The daemon says so itself -- "tests run
 * with shadow_ui mostly idle (no human interaction), so contention is rare".
 * Dropping 100 reads/s to 10 changed nothing, because the cost is MUTUAL
 * EXCLUSION, not throughput. An instrument a human cannot play alongside
 * cannot measure a gesture.
 *
 * So this samples in-process instead. The shim already calls the chain's
 * get_param directly on the SPI callback -- an ordinary C call, no SHM, no
 * channel, no second producer -- and the WORKER (SCHED_OTHER) does the file
 * I/O. That is the same split align_capture.{c,h} uses, and for the same
 * reason: an instrument that perturbs the thing it measures reports its own
 * behaviour.
 *
 * RT: the producer side is memcpy into a preallocated ring. No allocation, no
 * I/O, no locks. The consumer side opens a file and must never be called from
 * the callback.
 *
 * Armed by /data/UserData/schwung/lanes_trace_on; silent and nearly free
 * otherwise (one access() per second on the worker, nothing on the callback).
 */
#ifndef LANE_TRACE_H
#define LANE_TRACE_H

#include <stdint.h>
#include <string.h>

#include "lane_store.h"   /* LANE_MAX -- the trace is sized from it */

/* One sample is a formatted `lanes:diag` answer plus the frame it was taken
 * on. Text rather than a parsed struct because the getter already formats it
 * and re-parsing on the callback would be work with nothing to show for it.
 *
 * SIZED FROM LANE_MAX, not guessed. The first cut was 384 bytes because five
 * lanes "looked like enough", and the very first capture cut a lane in half
 * mid-name -- a trace that silently drops the lane you are chasing is the
 * failure this whole instrument exists to avoid. The worst case is every lane
 * used with the longest target and param the store allows (16 + 32), which is
 * ~150 chars a row; 160 leaves room for the numbers to be wider than expected.
 * It must stay >= what lane_param_get's "diag" branch can produce, or the
 * truncation simply moves one layer down. */
#define LANE_TRACE_LINE_MAX   (LANE_MAX * 160 + 128)
/* The ring only has to cover the gap between worker drains (5 Hz) -- about 4
 * samples per slot -- and the rest is headroom so a stalled worker loses
 * nothing. 96 entries is ~1.2 s of slack, six drain periods, for ~500 KB of
 * BSS in the shim. */
#define LANE_TRACE_ENTRIES    96
/* Sample every N SPI frames. 344 Hz / 17 = ~20 Hz: ~36 samples per 4-beat loop
 * at 132 BPM, against a question ("one playthrough or three") that is coarse. */
#define LANE_TRACE_EVERY_FRAMES 17

typedef struct {
    uint32_t frame;
    uint32_t slot;
    char     line[LANE_TRACE_LINE_MAX];
} lane_trace_entry_t;

typedef struct {
    /* write_idx is the producer's alone, read_idx the consumer's alone; each
     * is published to the other with a release/acquire pair. A single slot of
     * slack is left unused so full and empty cannot look alike. */
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    volatile uint32_t dropped;      /* producer laps consumer: reported, never hidden */
    lane_trace_entry_t e[LANE_TRACE_ENTRIES];
} lane_trace_t;

/* PRODUCER — SPI callback. */
static inline void lane_trace_push(lane_trace_t *t, uint32_t frame,
                                   uint32_t slot, const char *line) {
    if (!t || !line) return;
    const uint32_t w = t->write_idx;
    const uint32_t nxt = (w + 1u) % LANE_TRACE_ENTRIES;
    if (nxt == __atomic_load_n(&t->read_idx, __ATOMIC_ACQUIRE)) {
        /* FULL. Drop the NEW sample rather than the old ones: the take is
         * what was recorded first, and overwriting it to keep the idle tail
         * would throw away the only part that matters. */
        t->dropped++;
        return;
    }
    lane_trace_entry_t *e = &t->e[w];
    e->frame = frame;
    e->slot  = slot;
    /* strncpy-with-explicit-terminator rather than strlcpy, which is not
     * available everywhere this builds. */
    size_t n = strlen(line);
    if (n >= LANE_TRACE_LINE_MAX) n = LANE_TRACE_LINE_MAX - 1;
    memcpy(e->line, line, n);
    e->line[n] = '\0';
    __atomic_store_n(&t->write_idx, nxt, __ATOMIC_RELEASE);
}

/* CONSUMER — worker. Returns 1 and fills *out, or 0 when empty. */
static inline int lane_trace_pop(lane_trace_t *t, lane_trace_entry_t *out) {
    if (!t || !out) return 0;
    const uint32_t r = t->read_idx;
    if (r == __atomic_load_n(&t->write_idx, __ATOMIC_ACQUIRE)) return 0;
    *out = t->e[r];
    __atomic_store_n(&t->read_idx, (r + 1u) % LANE_TRACE_ENTRIES,
                     __ATOMIC_RELEASE);
    return 1;
}

static inline int lane_trace_should_sample(uint32_t frame) {
    return (frame % LANE_TRACE_EVERY_FRAMES) == 0;
}

/* THE RING AND ITS ARM LIVE IN THE SHIM, and the callback reads the arm as a
 * plain int rather than calling access() -- the SPI callback does no file I/O,
 * and a diagnostic that breaks that rule measures its own damage. The worker
 * polls the file and publishes the flag; same shape as shim_touch_trace_on. */
lane_trace_t *lane_trace_ring(void);
int  lane_trace_armed(void);
void lane_trace_set_armed(int on);

#endif /* LANE_TRACE_H */
