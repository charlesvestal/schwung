#ifndef PARAM_SLOW_H
#define PARAM_SLOW_H

/*
 * Attribution for a param serve that ate the frame.
 *
 * WHY THIS EXISTS. Module entry points ARE the SPI callback, and the ecosystem
 * does not know it — the 2026-08 audit found ~150 confirmed violations across
 * 113 catalogued modules, several carrying comments asserting the opposite in
 * so many words. So a module doing disk I/O inside set_param is not an
 * anomaly to be fixed once; it is the steady state we have to be able to SEE.
 *
 * Twice now the same defect has been diagnosed from scratch:
 *
 *   overtake dlopen + create_instance   param stage 7us typical -> 11513us
 *   dr32 kit load inside synth:state    param stage 7us typical -> 20051us
 *
 * Both times the shape was identical and both times finding it took a
 * differential experiment against the user's ears, because the only thing the
 * logs said was `param=7/20051` — a number with no name attached. The key is
 * RIGHT THERE in the request being served. Recording it turns an afternoon
 * into one grep.
 *
 * WHY A RING AND NOT A SINGLE SLOT. A burst reports its worst member and
 * nothing else if you keep one slot, and "the worst" is exactly the wrong
 * summary when the interesting fact is WHICH KEYS were slow — a recall walks
 * several components and only one of them is the culprit. Four entries is
 * enough to name the offenders in a gesture and small enough to sit in BSS.
 *
 * WHY IT IS LOSSY BY CONSTRUCTION. The producer is the SPI callback. It may
 * not block, allocate, or log, so it cannot wait for a full ring to drain. It
 * overwrites the oldest entry and counts the loss; `dropped` being non-zero is
 * itself a finding (something is slow at a rate faster than 1 Hz) and is
 * reported rather than hidden.
 *
 * REALTIME CONTRACT. The producer does two vDSO clock reads (~340ns each,
 * measured — not the ~1.8us syscall) and, only past the threshold, one bounded
 * string copy. The consumer is the shim worker (SCHED_OTHER), which is the
 * only side allowed to format or log.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Independent of SHADOW_PARAM_KEY_LEN on purpose: this header is driven by
 * tests/host without the shadow headers, and a key is truncated for a log
 * line, never used to address anything. */
#define PARAM_SLOW_KEY_LEN 64
#define PARAM_SLOW_ENTRIES 4

/*
 * The default threshold, in microseconds.
 *
 * NOT the frame period (2902us) and not the measured slack (~2370us). A serve
 * that eats a third of the budget has not glitched yet but is one unlucky
 * neighbour away from it, and the whole point is to hear about it BEFORE a
 * user does. A normal serve is ~10us, so there are two and a half orders of
 * magnitude between "fine" and this line — nothing lands here by accident.
 */
#define PARAM_SLOW_THRESHOLD_US 1000u

typedef struct {
    char     key[PARAM_SLOW_KEY_LEN];
    uint32_t us;
    uint8_t  slot;
    uint8_t  is_set;      /* 1 = set_param, 0 = get_param */
} param_slow_entry_t;

typedef struct {
    param_slow_entry_t entries[PARAM_SLOW_ENTRIES];
    uint32_t write;       /* monotonic; slot = write % PARAM_SLOW_ENTRIES */
    uint32_t read;        /* monotonic; consumer-owned */
    uint32_t dropped;     /* entries overwritten before the consumer saw them */
    uint32_t threshold_us;
} param_slow_t;

static inline void param_slow_init(param_slow_t *p, uint32_t threshold_us) {
    memset(p, 0, sizeof(*p));
    p->threshold_us = threshold_us ? threshold_us : PARAM_SLOW_THRESHOLD_US;
}

/*
 * Producer. Call once per served request with its measured duration.
 *
 * Returns 1 if the serve was recorded, 0 if it was under the threshold. The
 * return value is what lets a test assert the threshold is honoured without
 * reaching into the ring.
 *
 * A NULL or empty key is recorded as "?" rather than dropped: a serve that
 * blew the budget is worth reporting even when we cannot say which key it was,
 * and silently discarding it would make the rate look lower than it is.
 */
static inline int param_slow_record(param_slow_t *p, const char *key,
                                    uint8_t slot, uint8_t is_set, uint32_t us) {
    if (!p || us < p->threshold_us) return 0;

    /* Overwrite-oldest. The producer never blocks; see the header comment. */
    if (p->write - p->read >= PARAM_SLOW_ENTRIES) {
        p->dropped++;
        p->read++;
    }

    param_slow_entry_t *e = &p->entries[p->write % PARAM_SLOW_ENTRIES];
    if (key && key[0]) {
        size_t n = strlen(key);
        if (n >= PARAM_SLOW_KEY_LEN) n = PARAM_SLOW_KEY_LEN - 1;
        memcpy(e->key, key, n);
        e->key[n] = '\0';
    } else {
        e->key[0] = '?';
        e->key[1] = '\0';
    }
    e->us     = us;
    e->slot   = slot;
    e->is_set = is_set ? 1 : 0;

    /* Publish last: the consumer keys on `write`, so the entry must be
     * complete before the index that makes it visible moves. */
    __atomic_store_n(&p->write, p->write + 1, __ATOMIC_RELEASE);
    return 1;
}

/* Consumer. Copies the next pending entry into `out`; returns 0 when drained. */
static inline int param_slow_take(param_slow_t *p, param_slow_entry_t *out) {
    if (!p || !out) return 0;
    uint32_t w = __atomic_load_n(&p->write, __ATOMIC_ACQUIRE);
    if (p->read == w) return 0;
    *out = p->entries[p->read % PARAM_SLOW_ENTRIES];
    p->read++;
    return 1;
}

/* Consumer. Number of overwritten-before-seen entries, cleared by the read. */
static inline uint32_t param_slow_take_dropped(param_slow_t *p) {
    if (!p) return 0;
    uint32_t d = p->dropped;
    p->dropped = 0;
    return d;
}

/*
 * Format one entry. Kept here beside the producer so the wording is pinned by
 * the same test that pins the mechanism — a log line nobody can grep for is
 * the failure this whole header exists to prevent.
 */
static inline int param_slow_format(const param_slow_entry_t *e,
                                    char *buf, int len) {
    if (!e || !buf || len <= 0) return 0;
    int n = 0;
    const char *verb = e->is_set ? "set" : "get";
    /* snprintf is the consumer's (SCHED_OTHER); never call this from the
     * callback. */
    n = snprintf(buf, (size_t)len,
                 "param-slow: %s slot %u %s took %u.%03u ms on the SPI callback "
                 "— the module is doing blocking work in its entry point",
                 verb, (unsigned)e->slot, e->key,
                 e->us / 1000u, e->us % 1000u);
    return (n < 0) ? 0 : n;
}

#endif /* PARAM_SLOW_H */
