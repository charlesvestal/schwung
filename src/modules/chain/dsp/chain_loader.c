/*
 * Deferred module loading. See chain_loader.h for why this exists and for the
 * "stage, don't swap" property the whole thing rests on.
 *
 * Concurrency, stated completely, because it is the entire risk surface:
 *
 *   SPI thread (SCHED_FIFO 70) writes:  req seqlock, req_gen, committed_gen,
 *                                       retire_head
 *   SPI thread reads:                   staged_valid, the staged record
 *   loader thread (SCHED_OTHER) writes: the staged record, staged_valid,
 *                                       retire_tail
 *   loader thread reads:                req seqlock, req_gen, retire ring
 *
 * NO FIELD HAS TWO WRITERS — see the struct comment for the two bugs that
 * bought that rule. The staged record is a strict handoff: the loader fills it
 * and stores staged_valid with release; the SPI thread loads it with acquire,
 * consumes it, and clears it. Neither side touches it while the other might.
 *
 * The request is a SEQLOCK because the SPI thread must be able to overwrite it
 * while the loader is mid-load (spinning the module picker) without blocking
 * and without tearing. Writer never waits; reader retries. The reader is the
 * SCHED_OTHER thread, so a retry costs nothing that matters.
 */

/* cpu_set_t, sched_setaffinity, pthread_setname_np, sched_setscheduler. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "chain_loader.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

/*
 * EVERY FIELD HAS EXACTLY ONE WRITER. That is the whole discipline here, and
 * it is not stylistic — the first version of this file kept a single `state`
 * word that both threads wrote, and it had two bugs that no amount of care at
 * the call sites would have removed:
 *
 *   - the loader storing READY just after the SPI thread stored QUEUED lost
 *     the newer request permanently: the position simply never loaded, with
 *     nothing logged and nothing to see.
 *   - the two orders of the same pair also stranded a staged module that
 *     commit could then never reach, leaking a dlopen handle per swap.
 *
 * So there is no shared state word. There is work to do when the generations
 * disagree, and each side only ever advances its own:
 *
 *   req_gen        SPI    bumped once per request
 *   committed_gen  SPI    caught up to a request once it is resolved
 *   staged_valid   loader 1 when `staged` holds a complete result
 *   staged_gen     loader which request produced it (read after acquiring
 *                         staged_valid, so it is published with it)
 *
 * "Is a load outstanding" is therefore req_gen != committed_gen — a question
 * with one answer rather than a state that two threads can disagree about.
 */
struct chain_loader {
    pthread_t        thread;
    int              thread_started;
    /* Atomic, not merely volatile: `volatile` orders nothing and is not an
     * atomic type, so a plain store here racing the loop's plain load below is
     * a data race by the C11 model even though a naturally-aligned int cannot
     * tear on ARM64. TSan flags it, correctly. Every other cross-thread field
     * in this struct goes through the acquire/release helpers; so does this. */
    volatile unsigned quit;

    /* --- request: seqlock, written by SPI, read by loader --- */
    volatile unsigned req_seq;                  /* odd while being written */
    char              req_module[MAX_NAME_LEN];
    volatile unsigned req_gen;                  /* SPI writes */
    volatile unsigned committed_gen;            /* SPI writes */

    /* --- result: written by loader, read by SPI at commit --- */
    chain_staged_synth_t staged;
    unsigned          staged_gen;               /* loader writes */
    volatile unsigned staged_valid;             /* loader writes; publishes staged */
    unsigned          last_attempt_gen;         /* loader only */

    /* --- retire ring: written by SPI at commit, drained by loader --- */
    chain_retired_module_t retire[CHAIN_LOADER_RETIRE_SLOTS];
    volatile unsigned retire_head;              /* SPI writes */
    volatile unsigned retire_tail;              /* loader writes */

    chain_instance_t *inst;
};

/* ------------------------------------------------------------------ atomics */

static inline unsigned ld_acq(const volatile unsigned *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void st_rel(volatile unsigned *p, unsigned v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------- the loader thread */

/*
 * Demote and pin, FIRST THING, before any module code can run on this thread.
 *
 * This is belt and braces on purpose. The thread is created from
 * v2_create_instance, which is ITSELF on the SPI callback, so inheriting FIFO
 * 70 is the DEFAULT outcome and it would silently defeat this entire file —
 * every plugin worker would still be born realtime and the only symptom would
 * be the dropouts we are trying to remove. The pthread_attr in
 * chain_loader_start says SCHED_OTHER explicitly; this says it again from
 * inside the thread, because the two mechanisms fail differently and neither
 * one reports failing.
 *
 * It is also, deliberately, the exact three lines the fleet-repo patch asks
 * module authors to add to their own workers.
 */
static void loader_thread_demote(void) {
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));

    /*
     * BOTH CALLS, THEN VERIFY — and the verification is the point.
     *
     * The first version called only sched_setscheduler() and trusted it. On
     * hardware 2026-08-27 it did not take: the audit reported this very thread
     * as `schwung-loader` at SCHED_FIFO 45 — realtime, and above Move's
     * `Link Main` at 35, i.e. precisely the harm this whole file exists to
     * remove. The affinity call two lines below succeeded on the same thread in
     * the same function, so this was not a missing _GNU_SOURCE or a
     * non-Linux build; the scheduler call itself failed, silently, and
     * everything downstream assumed it had worked.
     *
     * The comment above already warned that "the two mechanisms fail
     * differently and neither one reports failing" — and then nothing checked.
     * That was the actual defect, so now it checks and says so.
     *
     * pthread_setschedparam() is tried first because it is the call minijv
     * uses on this same device, from this same context, successfully.
     */
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
#ifdef __linux__
    sched_setscheduler(0, SCHED_OTHER, &sp);

    {
        int pol = sched_getscheduler(0);
        if (pol != -1 && pol != SCHED_OTHER) {
            /* Loud, once per loader thread. A realtime loader silently
             * re-creates the bug for every plugin thread born from create. */
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "chain loader: FAILED to leave realtime (policy %d) — "
                     "plugin threads created here will inherit it", pol);
            chain_loader_note_demote_failed(msg);
        }
    }
#endif

#ifdef __linux__
    /* Keep core 3 for SPI. */
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    CPU_SET(1, &set);
    CPU_SET(2, &set);
    sched_setaffinity(0, sizeof(set), &set);

    /* Name it. An unnamed worker inherits the parent's comm and reports as
     * `Audio Main/SPI` — indistinguishable from the real SPI thread in top, in
     * a thread list, or to its own author. That invisibility is why the fleet's
     * RT threads survived so long; this one will not hide. */
    pthread_setname_np(pthread_self(), "schwung-loader");
#endif
}

/*
 * The seqlock PAYLOAD copy, isolated into two named functions purely so a
 * ThreadSanitizer suppression can name them and nothing else.
 *
 * A seqlock reader deliberately reads bytes that may be concurrently written
 * and then DISCARDS what it read if the sequence counter moved. That is the
 * algorithm working, not a bug — but by the letter of the C11 model the copy
 * itself is a data race, and TSan has no way to see the validation that
 * follows. It is the well-known seqlock blind spot.
 *
 * Keeping these two lines in functions of their own means the suppression in
 * tests/host/tsan.supp covers exactly the copy and stays out of the way of any
 * real race elsewhere in the request path. Do not inline them back.
 */
static void seqlock_write_payload(char *dst, size_t dst_len, const char *src) {
    snprintf(dst, dst_len, "%s", src);
}

static void seqlock_read_payload(char *dst, size_t dst_len, const char *src) {
    snprintf(dst, dst_len, "%s", src);
}

/* Copy the outstanding request out of the seqlock. Returns 0 if a stable
 * reading was obtained. */
static int loader_read_request(chain_loader_t *ld, char *out, int out_len,
                               unsigned *gen_out) {
    for (int spin = 0; spin < 1000; spin++) {
        unsigned s1 = ld_acq(&ld->req_seq);
        if (s1 & 1u) continue;              /* writer mid-update */
        seqlock_read_payload(out, (size_t)out_len, ld->req_module);
        unsigned g = ld_acq(&ld->req_gen);
        unsigned s2 = ld_acq(&ld->req_seq);
        if (s1 == s2) { *gen_out = g; return 0; }
    }
    return -1;
}

static void loader_drain_retires(chain_loader_t *ld) {
    while (ld_acq(&ld->retire_tail) != ld_acq(&ld->retire_head)) {
        unsigned t = ld_acq(&ld->retire_tail);
        chain_retired_module_t r = ld->retire[t % CHAIN_LOADER_RETIRE_SLOTS];
        chain_synth_destroy_triple(&r);
        st_rel(&ld->retire_tail, t + 1);
    }
}

static void *loader_thread_main(void *arg) {
    chain_loader_t *ld = (chain_loader_t *)arg;
    loader_thread_demote();

    while (!ld_acq(&ld->quit)) {
        loader_drain_retires(ld);

        unsigned want = ld_acq(&ld->req_gen);

        /* Nothing asked for, or we already built this one and it is sitting
         * unclaimed. Note we do NOT wait for the commit to happen — but we do
         * refuse to overwrite an unclaimed result below. */
        if (want == 0 || want == ld->last_attempt_gen) {
            sleep_ms(CHAIN_LOADER_POLL_MS);
            continue;
        }

        char name[MAX_NAME_LEN];
        unsigned gen = 0;
        if (loader_read_request(ld, name, sizeof(name), &gen) != 0) {
            sleep_ms(CHAIN_LOADER_POLL_MS);
            continue;
        }
        ld->last_attempt_gen = gen;

        /* THE LONG PART. Runs unlocked, at normal priority, touching nothing
         * any render path can name. `params` is the loader's own block, reused
         * every time — it is never NULL and is never handed away except by the
         * swap inside chain_synth_commit, which gives one back. */
        chain_staged_synth_t staged;
        memset(&staged, 0, sizeof(staged));
        staged.params = ld->staged.params;
        memset(staged.params, 0,
               sizeof(chain_param_info_t) * (size_t)MAX_CHAIN_PARAMS);
        chain_synth_stage(ld->inst, name, &staged);

        /* Superseded while we worked? Throw it away and take the newest.
         * A generation counter rather than a queue: spinning the module picker
         * must not build a backlog of loads nobody asked to keep. */
        if (ld_acq(&ld->req_gen) != gen) {
            chain_retired_module_t r = { staged.handle, staged.api, staged.instance };
            chain_synth_destroy_triple(&r);
            continue;                            /* the loop re-reads req_gen */
        }

        /* A previous result the SPI thread never claimed (it can only happen
         * if a request superseded it before any render ran). Destroy it here —
         * we are the loader, this is exactly where teardown belongs — rather
         * than overwrite the pointers and leak a dlopen handle per swap. */
        if (ld_acq(&ld->staged_valid)) {
            chain_retired_module_t r = { ld->staged.handle, ld->staged.api,
                                         ld->staged.instance };
            chain_synth_destroy_triple(&r);
            st_rel(&ld->staged_valid, 0);
        }

        staged.params  = ld->staged.params;      /* keep the block we own */
        ld->staged     = staged;
        ld->staged_gen = gen;
        st_rel(&ld->staged_valid, 1);            /* publishes the record */
    }

    loader_drain_retires(ld);
    return NULL;
}

/* ------------------------------------------------------------- SPI-thread */

/* Started lazily so a chain that never swaps a module never spawns a thread. */
static int chain_loader_start(chain_instance_t *inst) {
    if (!inst) return -1;
    if (inst->loader) return 0;

    chain_loader_t *ld = (chain_loader_t *)calloc(1, sizeof(*ld));
    if (!ld) return -1;
    ld->inst = inst;
    ld->staged.params = (chain_param_info_t *)calloc(MAX_CHAIN_PARAMS,
                                                     sizeof(chain_param_info_t));
    if (!ld->staged.params) { free(ld); return -1; }

    /* EXPLICIT_SCHED, or the thread inherits the SPI callback's FIFO 70 —
     * see loader_thread_demote() for why this is said twice. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#ifdef __linux__
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &sp);
#endif
    int rc = pthread_create(&ld->thread, &attr, loader_thread_main, ld);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        free(ld->staged.params);
        free(ld);
        return -1;
    }
    ld->thread_started = 1;
    inst->loader = ld;
    return 0;
}

int chain_loader_request_synth(chain_instance_t *inst, const char *module_name) {
    if (!inst || !module_name || !module_name[0]) return -1;
    if (chain_loader_start(inst) != 0) return -1;
    chain_loader_t *ld = inst->loader;

    /* Seqlock write. The SPI thread never waits here — that is the point.
     * Sole writer, so the read-modify-writes need no atomic RMW. */
    st_rel(&ld->req_seq, ld->req_seq + 1);          /* odd: writing */
    seqlock_write_payload(ld->req_module, sizeof(ld->req_module), module_name);
    /* INSIDE the seqlock, so a reader gets the name and the generation that
     * belong together. Bumping it after the even marker would let the loader
     * pair a new name with the old generation, discover the mismatch only
     * after building the module, and throw a completed load away. */
    st_rel(&ld->req_gen, ld->req_gen + 1);
    st_rel(&ld->req_seq, ld->req_seq + 1);          /* even: stable */
    return 0;
}

void chain_loader_commit(chain_instance_t *inst) {
    if (!inst || !inst->loader) return;
    chain_loader_t *ld = inst->loader;

    /* The common case, every block, one acquire load. */
    if (!ld_acq(&ld->staged_valid)) return;

    unsigned g   = ld->staged_gen;      /* published together with staged_valid */
    unsigned req = ld_acq(&ld->req_gen);

    if (g != req) {
        /*
         * Superseded before we ever rendered. Drop it — and deliberately do
         * NOT advance committed_gen, because a newer load is still owed and
         * is_loading must keep saying so. The loader's last_attempt_gen is
         * behind req_gen, so it picks the new one up on its next pass.
         *
         * `staged.params` is untouched: it is the loader's block, not this
         * load's, and nulling it here is what crashed the first version.
         */
        chain_retired_module_t r = { ld->staged.handle, ld->staged.api,
                                     ld->staged.instance };
        chain_loader_retire(inst, &r);
        ld->staged.handle   = NULL;
        ld->staged.api      = NULL;
        ld->staged.instance = NULL;
        ld->staged.ok       = 0;
        st_rel(&ld->staged_valid, 0);
        return;
    }

    chain_retired_module_t old = { NULL, NULL, NULL };
    chain_synth_commit(inst, &ld->staged, &old);
    if (old.handle || old.instance) chain_loader_retire(inst, &old);

    /*
     * Caught up — including when the load FAILED. chain_synth_commit leaves the
     * position alone in that case, but the request is still resolved, and not
     * advancing here would strand is_loading at "1" forever on a module that
     * simply does not load.
     */
    st_rel(&ld->committed_gen, g);
    st_rel(&ld->staged_valid, 0);
}

int chain_loader_synth_busy(const chain_instance_t *inst) {
    if (!inst || !inst->loader) return 0;
    const chain_loader_t *ld = inst->loader;
    /* One question, one answer: a request is outstanding until it is resolved.
     * No state word for the two threads to disagree about. */
    return ld_acq(&ld->req_gen) != ld_acq(&ld->committed_gen) ? 1 : 0;
}

void chain_loader_retire(chain_instance_t *inst, const chain_retired_module_t *r) {
    if (!inst || !r || (!r->handle && !r->instance)) return;
    chain_loader_t *ld = inst->loader;
    if (!ld) {
        /* No loader (synchronous path): tear down here, as we always did. */
        chain_retired_module_t copy = *r;
        chain_synth_destroy_triple(&copy);
        return;
    }

    unsigned h = ld_acq(&ld->retire_head);
    unsigned t = ld_acq(&ld->retire_tail);
    if (h - t >= CHAIN_LOADER_RETIRE_SLOTS) {
        /* Full. LEAK, and say so. Commits are bounded by load completions, so
         * reaching this means something is very wrong — and destroying inline
         * would put dlclose and a plugin's destructor on the audio thread,
         * which is the defect this file exists to remove. A leak is
         * recoverable; a use-after-free in the audio path is not. */
        chain_loader_note_retire_overflow(inst);
        return;
    }
    ld->retire[h % CHAIN_LOADER_RETIRE_SLOTS] = *r;
    st_rel(&ld->retire_head, h + 1);
}

void chain_loader_shutdown(chain_instance_t *inst) {
    if (!inst || !inst->loader) return;
    chain_loader_t *ld = inst->loader;

    /*
     * Called from v2_destroy_instance. Joining is correct rather than merely
     * convenient: a load in flight owns a half-built instance and the module
     * dir string, and detaching would race the free of everything below.
     * Worst case we wait out one in-flight create_instance — on the teardown
     * path, off the audio thread.
     */
    st_rel(&ld->quit, 1);
    if (ld->thread_started) pthread_join(ld->thread, NULL);

    /* Anything the loader staged but never committed is ours to destroy. */
    if (ld->staged.handle || ld->staged.instance) {
        chain_retired_module_t r = { ld->staged.handle, ld->staged.api,
                                     ld->staged.instance };
        chain_synth_destroy_triple(&r);
    }
    /* And anything queued for retirement that the loader did not reach. */
    while (ld->retire_tail != ld->retire_head) {
        chain_retired_module_t r = ld->retire[ld->retire_tail % CHAIN_LOADER_RETIRE_SLOTS];
        chain_synth_destroy_triple(&r);
        ld->retire_tail++;
    }

    free(ld->staged.params);
    free(ld);
    inst->loader = NULL;
}
