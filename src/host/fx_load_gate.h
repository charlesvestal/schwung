/*
 * fx_load_gate.h — the three-state answer a Master FX / send FX position gives
 * while its module is being loaded off the SPI callback.
 *
 * Header-only and dependency-free so tests/host can run it, for the same
 * reason master_fx_key.h and send_fx_key.h are.
 *
 * ---------------------------------------------------------------------------
 * THE GATE IS A SEQUENCE NUMBER, NOT A FLAG — copied deliberately from
 * chain_bus.c's `fx_ready`, which is where this shape was worked out and where
 * the boolean version's failure is written up. A boolean makes "close" a store
 * by one thread and "open" a store by another guarded by a separate load: a
 * worker preempted between its check and its store re-opens a gate the RT
 * thread closed in between, and the next teardown then runs dlclose() against
 * a pointer the render path still believes in.
 *
 * Here the two counters are:
 *
 *   req_seq   THE CLOSE. Bumped by the RT thread, and by nobody else, on every
 *             new request. From that store on, any realisation the worker is
 *             carrying is STALE — it is stamped with an older seq and the
 *             install refuses it.
 *
 *   done_seq  THE OPEN. Stored by the RT thread at INSTALL, carrying the seq
 *             it just installed. The worker never writes it, so there is no
 *             store by a preemptible thread that can resurrect a closed gate.
 *
 * A third counter, req_pub, exists because the REQUEST has a payload (the DSP
 * path) and the gate does not: the path is written between the close and the
 * publish, so a worker must not read it until req_pub has caught up with
 * req_seq. That is the same "close before you post" ordering
 * tests/host/test_bus_gate_ordering.sh pins for buses, and
 * tests/host/test_fx_load_gate_ordering.sh pins it here.
 *
 * All three start at 0, which is why 0 is skipped on wrap: a freshly zeroed
 * position must read as SETTLED (nothing has ever been asked of it), not as
 * "loading forever".
 */

#ifndef FX_LOAD_GATE_H
#define FX_LOAD_GATE_H

/* What the UI is told, and what the param surface answers. */
#define FX_LOAD_SETTLED  0   /* nothing in flight: the position is what it says */
#define FX_LOAD_LOADING  1   /* a request is in flight; the position is not yet itself */
#define FX_LOAD_FAILED   2   /* the last settled request did not produce a module */

/*
 * `failed` describes the outcome of `done_seq` and is meaningless while a newer
 * request is in flight — a failure the user has already replaced must not keep
 * reporting itself, or a retry that is still running reads as an error.
 */
static inline int fx_load_state(unsigned req_seq, unsigned done_seq, int failed) {
    if (req_seq != 0u && req_seq != done_seq) return FX_LOAD_LOADING;
    if (failed) return FX_LOAD_FAILED;
    return FX_LOAD_SETTLED;
}

/* Next value of a gate counter. 0 is skipped so it keeps meaning "never". */
static inline unsigned fx_load_next_seq(unsigned seq) {
    unsigned n = seq + 1u;
    return n == 0u ? 1u : n;
}

/*
 * May the worker read this position's request payload yet?
 *
 * `pub` lagging `req` means the RT thread has closed the gate but has not
 * finished writing the path behind it. Answering "yes" there is the one way a
 * worker can dlopen a torn string.
 */
static inline int fx_load_request_readable(unsigned req_seq, unsigned req_pub,
                                           unsigned done_seq) {
    return req_pub != 0u && req_pub == req_seq && req_pub != done_seq;
}

/*
 * Is a realisation stamped `seq` still the one that was asked for?
 *
 * Called at install, on the RT thread, against the live req_seq. False means
 * the request moved on while the worker worked: the realisation is retired
 * rather than installed, and done_seq is NOT advanced, so the worker comes
 * back for the newer one.
 */
static inline int fx_load_stage_current(unsigned stage_seq, unsigned req_seq) {
    return stage_seq != 0u && stage_seq == req_seq;
}

#endif /* FX_LOAD_GATE_H */
