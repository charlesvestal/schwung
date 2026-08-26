#ifndef CHAIN_LOADER_H
#define CHAIN_LOADER_H

/*
 * Deferred module loading — the SPI callback asks, a normal-priority thread
 * does the work, the SPI callback publishes the result.
 *
 * TWO defects share one call site, and this fixes both.
 *
 * 1. `create_instance` runs on the SPI callback, which is SCHED_FIFO 70 on
 *    core 3. POSIX defaults to PTHREAD_INHERIT_SCHED, so a plugin that calls
 *    pthread_create from create_instance gets a worker at FIFO 70 — and Move's
 *    own Link Audio publisher, `Link Main`, is FIFO 35. Measured on hardware
 *    2026-08-22: five modules do this (sfz x5 threads, osirus, minijv,
 *    po32-drum, fork). Loading from a SCHED_OTHER thread makes every inherited
 *    worker SCHED_OTHER, fleet-wide, without touching a single module repo.
 *
 *    It does NOT fix a module that sets its own priority — minijv asks for
 *    FIFO 45 explicitly. That one can only be fixed in its own repo.
 *
 * 2. The load BLOCKS the callback. Measured 672.9 ms for minijv: ~232
 *    consecutive dropped frames against a 2902 us block period.
 *
 * STAGE, DON'T SWAP — this is the property everything else rests on.
 *
 * The loader thread never touches a live position. It builds the whole
 * instance into a staging record no render path can reach; the SPI thread
 * publishes it by swapping pointers, at the same point the synchronous load
 * publishes today. So "only the SPI thread ever mutates a chain instance"
 * survives verbatim, a module's own state is never reached from two threads,
 * and NEITHER SIDE TAKES A LOCK.
 *
 * That last point is not a micro-optimisation. An RT thread blocking on a
 * mutex held by a SCHED_OTHER thread is unbounded priority inversion — the
 * exact defect the 2026-08-22 audit flagged in minijv's ring_mutex. So the
 * SPI-thread side of this file is atomic stores and nothing else: no mutex, no
 * condvar, no semaphore, no syscall.
 *
 * The loader is woken by POLLING, not signalled. sem_post/cond_signal from the
 * SPI thread would be a futex syscall on the audio callback to save latency on
 * an operation that takes hundreds of milliseconds. The loader sleeps
 * CHAIN_LOADER_POLL_MS between checks; that is the entire cost of the choice.
 */

#include "chain_internal.h"

/* Idle poll interval. Bounds how long a request waits before the loader
 * notices it. Deliberately coarse: it is added to a load measured in hundreds
 * of milliseconds, and it buys a zero-syscall RT path. */
#define CHAIN_LOADER_POLL_MS 20

/*
 * The loader thread's realtime priority — and picking the RUNG is the whole
 * design, so read this before changing it.
 *
 * Threads and forked children INHERIT this. That is not a side effect to be
 * tolerated, it is the mechanism: a plugin that spawns a worker or forks a DSP
 * process from create_instance gets whatever the loader has, and most of the
 * fleet does exactly that without knowing it.
 *
 * So the number decides the fleet's behaviour:
 *
 *   70  what it used to inherit (Schwung's SPI callback). ABOVE Move's own
 *       Link Audio publisher `Link Main` at 35, so a module doing sustained
 *       work starves Move's audio device-wide. This is the bug.
 *    0  SCHED_OTHER. Fixes the starvation and BREAKS the modules that were
 *       silently relying on inherited realtime to keep up — measured on
 *       hardware 2026-08-27, osirus's forked DSP child audibly underran.
 *   20  realtime, so those modules still keep up, but BELOW 35 so none of
 *       them can outrank Move's publisher. Nothing in the fleet needs a patch.
 *
 * Do not raise this to 35 or above: that is the starvation regime the whole
 * file exists to leave. Anything a module genuinely needs above its own
 * loader, it must ask for explicitly.
 */
#define CHAIN_LOADER_RT_PRIORITY 20

/* Move's Link Audio publisher. Documented here because it is the ceiling the
 * value above is chosen against, and it is not otherwise visible from this
 * file. Measured on hardware 2026-08-22 and again 2026-08-27. */
#define CHAIN_LOADER_LINK_MAIN_PRIORITY 35

/* How many retired module triples can await teardown before we leak one.
 * Commits are bounded by load completions, which are slow, so this is never
 * reached in practice — it exists so the overflow path is "leak and say so"
 * rather than "destroy_instance on the audio thread". A leak is recoverable;
 * a use-after-free in the audio path is not. */
#define CHAIN_LOADER_RETIRE_SLOTS 4

/*
 * A staged synth load. Everything here is produced on the loader thread and
 * consumed, once, by the SPI thread at commit.
 *
 * `params` is an OWNED BUFFER, allocated once for the life of the loader and
 * swapped with the instance's block at commit — never memcpy'd. One
 * chain_param_info_t is ~4 KB (128 enum option strings), so the block is
 * ~1.1 MB and copying it would cost more than the frame it lands in.
 */
typedef struct {
    void            *handle;
    plugin_api_v2_t *api;
    void            *instance;
    char             module_name[MAX_NAME_LEN];
    char             synth_path[MAX_PATH_LEN];
    chain_param_info_t *params;          /* owned; MAX_CHAIN_PARAMS entries */
    int              param_count;
    int              default_forward_channel;
    int              consumes_line_input;
    char             load_error[256];
    int              ok;                 /* 1 = usable, 0 = load failed */
} chain_staged_synth_t;

/* A module triple detached from a live position, awaiting teardown off-thread. */
typedef struct {
    void            *handle;
    plugin_api_v2_t *api;
    void            *instance;
} chain_retired_module_t;

/*
 * Lazily started on the first deferred load, joined at destroy. At most five
 * exist (4 chain slots + Master FX), each parked in a poll sleep — which,
 * per the audit, is a thread that costs nothing. "Existence is not the harm."
 */
typedef struct chain_loader chain_loader_t;

/* --- SPI-thread entry points. None of these blocks, allocates, or logs. --- */

/* Ask for `module_name` to be loaded into the synth position. Returns 0 if the
 * request was accepted (the caller must NOT also load synchronously), or -1 if
 * deferred loading is unavailable, in which case the caller should fall back to
 * the synchronous path. Supersedes any request not yet committed. */
/*
 * All of these are CHAIN_INTERNAL (hidden visibility), like every other
 * cross-TU function in the chain host. dsp.so exports exactly the five
 * chain entry points plus the unified_log family, and nothing else — a
 * dlopen'd sub-plugin that happened to define `chain_loader_commit` would
 * otherwise bind to ours, or ours to theirs.
 *
 * tests/host/test_chain_host_file_split.sh pins that exported set, but it
 * is guarded on the built dsp.so existing — which CI's host-tests job does
 * not produce. So it only fires for someone who has run a local ARM build,
 * and these shipped exported until one did.
 */
CHAIN_INTERNAL int  chain_loader_request_synth(chain_instance_t *inst, const char *module_name);

/* Publish a completed load, if there is one. Call from the render path. Cheap
 * (one atomic load) when nothing is pending. */
CHAIN_INTERNAL void chain_loader_commit(chain_instance_t *inst);

/* 1 while a request is outstanding — i.e. the honest answer to
 * `<prefix>:is_loading`. Must be reported as exactly "1" or "0": any other
 * answer latches the component as not-implementing-it in the shadow UI. */
CHAIN_INTERNAL int  chain_loader_synth_busy(const chain_instance_t *inst);

/* Hand a detached module triple to the loader for teardown. Safe to call with
 * no loader running, in which case it tears down inline — that is the
 * synchronous path's existing behaviour, unchanged. */
CHAIN_INTERNAL void chain_loader_retire(chain_instance_t *inst, const chain_retired_module_t *r);

/* --- lifecycle (create/destroy of the chain instance itself) --- */

CHAIN_INTERNAL void chain_loader_shutdown(chain_instance_t *inst);

/* --- provided by chain_host.c, called from both threads --- */

/* Build a synth instance into `out`. LOADER THREAD (or the synchronous
 * fallback). Touches nothing reachable from a render path. */
CHAIN_INTERNAL void chain_synth_stage(chain_instance_t *inst, const char *module_name,
                       chain_staged_synth_t *out);

/* Publish a staged load into the live position, handing back whatever it
 * displaced. SPI THREAD ONLY. Pointer swaps; no large copies. */
CHAIN_INTERNAL void chain_synth_commit(chain_instance_t *inst, chain_staged_synth_t *staged,
                        chain_retired_module_t *retired_out);

/* destroy_instance + dlclose. Never call from the SPI thread. */
CHAIN_INTERNAL void chain_synth_destroy_triple(chain_retired_module_t *r);

/* Retire ring overflow — logs. Not on any hot path. */
CHAIN_INTERNAL void chain_loader_note_retire_overflow(chain_instance_t *inst);

/* The loader thread could not leave realtime. Logs. Takes no instance because
 * it is called from the loader thread before it has one in hand. */
CHAIN_INTERNAL void chain_loader_note_demote_failed(const char *msg);

#endif /* CHAIN_LOADER_H */
