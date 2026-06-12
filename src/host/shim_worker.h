/* shim_worker.h - Background housekeeping thread for the shim.
 *
 * The SPI callbacks run at SCHED_FIFO 90 on core 3 with ~900µs/frame and
 * must not touch the filesystem (see docs/REALTIME_SAFETY.md). This worker
 * owns that work instead:
 *
 *   - polls debug flag/trigger files at ~1 Hz and publishes
 *     shim_debug_flags bits the RT path reads instead of calling access()
 *   - consumes one-shot trigger files (unlink + publish a pending bit or
 *     value the RT path picks up with an atomic test-and-clear)
 *   - executes deferred events posted from the RT path via a small SPSC
 *     ring (e.g. overtake exit hooks, which fork/exec — never on RT)
 *   - runs the current-set filesystem scan (shadow_poll_current_set);
 *     the RT path consumes its result via shadow_set_pages_consume()
 */

#ifndef SHIM_WORKER_H
#define SHIM_WORKER_H

#include <stdint.h>

/* Level-triggered flags: bit set while the flag file exists. */
#define SHIM_FLAG_SPI_SNAP       (1u << 0)  /* spi_snap_trigger */
#define SHIM_FLAG_XMOS_LOG       (1u << 1)  /* log_xmos_sysex_on */
#define SHIM_FLAG_SPI_MIDI_LOG   (1u << 2)  /* spi_midi_log_on */

/* One-shot flags: worker unlinks the trigger file and sets the bit; the
 * RT consumer clears it with shim_debug_flag_consume(). */
#define SHIM_FLAG_SLOT_FX_DUMP   (1u << 8)  /* slot_fx_dump_trigger */
#define SHIM_FLAG_ALIGN_DUMP     (1u << 9)  /* align_dump_trigger */
#define SHIM_FLAG_MAIN_FX_DUMP   (1u << 10) /* main_fx_dump_trigger */

extern volatile uint32_t shim_debug_flags;

/* Atomically test-and-clear a one-shot flag. Returns nonzero if it was set. */
static inline int shim_debug_flag_consume(uint32_t bit) {
    return (__sync_fetch_and_and(&shim_debug_flags, ~bit) & bit) != 0;
}

/* Pending SysEx-inject value read from spi_sysex_inject (file content),
 * -1 when none. RT consumer swaps it back to -1. */
extern volatile int shim_pending_sysex_inject;

/* Deferred events (RT-safe to post; worker executes within ~200 ms). */
#define SHIM_EVT_OVERTAKE_EXIT_HOOK 1
#define SHIM_EVT_RESTART_MOVE       2
#define SHIM_EVT_SAMPLER_PREP       3  /* mkdir/fopen/header/writer for an armed recording */
#define SHIM_EVT_SAMPLER_FINALIZE   4  /* join writer, trim preroll, close + header */
#define SHIM_EVT_SAMPLER_CANCEL     5  /* preroll cancel: join writer, unlink file */
#define SHIM_EVT_SKIPBACK_SAVE      6  /* spawn the detached skipback writer */
#define SHIM_EVT_SKIPBACK_RESIZE    7  /* realloc the skipback ring */
#define SHIM_EVT_PREVIEW_PLAY       8  /* read preview cmd path, open + mmap */

void shim_worker_post(uint8_t evt);

/* Hook table for events whose implementations live in schwung_shim.c /
 * shadow_sampler.c (worker can't see their statics). Registered once at
 * shim_spi_init; unset hooks make their events no-ops. */
typedef struct {
    void (*sampler_prepare)(void);
    void (*sampler_finalize)(void);
    void (*sampler_cancel_preroll)(void);
    void (*skipback_save)(void);
    void (*skipback_resize)(void);
    void (*preview_play_pending)(void);
} shim_worker_hooks_t;

void shim_worker_set_hooks(const shim_worker_hooks_t *hooks);

/* Spawn the worker thread (SCHED_OTHER, cores 0-2). Idempotent. */
void shim_worker_start(void);

#endif /* SHIM_WORKER_H */
