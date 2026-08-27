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
#define SHIM_FLAG_RT_AUDIT       (1u << 3)  /* rt_thread_audit_on */
#define SHIM_FLAG_SPI_TALLY      (1u << 4)  /* spi_tally_on */

/* One-shot flags: worker unlinks the trigger file and sets the bit; the
 * RT consumer clears it with shim_debug_flag_consume(). */
#define SHIM_FLAG_SLOT_FX_DUMP   (1u << 8)  /* slot_fx_dump_trigger */
/* (1u << 9) was SHIM_FLAG_ALIGN_DUMP. The align dump moved off the SPI
 * callback entirely — the worker arms and drains it (align_capture.h), so
 * there is nothing for the RT side to consume. Left as a hole on purpose:
 * reusing the bit would make a stale build read one trigger as another. */
#define SHIM_FLAG_MAIN_FX_DUMP   (1u << 10) /* main_fx_dump_trigger */

extern volatile uint32_t shim_debug_flags;

/* Atomically test-and-clear a one-shot flag. Returns nonzero if it was set. */
static inline int shim_debug_flag_consume(uint32_t bit) {
    return (__sync_fetch_and_and(&shim_debug_flags, ~bit) & bit) != 0;
}

/* Pending SysEx-inject value read from spi_sysex_inject (file content),
 * -1 when none. RT consumer swaps it back to -1. */
extern volatile int shim_pending_sysex_inject;

/* Boot jack-state re-assert: worker sets this to a CC 115 value (0=speaker,
 * 127=jack) ~5 s after start; RT consumer injects it into Move's MIDI_IN and
 * swaps it back to -1. Works around XMOS not reporting jack state at boot, so
 * Move's enhancer stays on "speaker" with headphones already plugged. */
extern volatile int shim_inject_boot_jack;

/* Last raw CC 115 value seen by the RT path (0=speaker, 127=jack), -1 until
 * any jack event. Worker persists it to /data on change and re-asserts it at
 * boot via shim_inject_boot_jack. */
extern volatile int shim_jack_persist;

/* Last USB-C audio-out source seen by the RT path (0 = Mic, 1 = Main Out),
 * -1 until observed. Worker persists it on change and re-asserts it at boot —
 * Move's firmware reverts this to Mic on every reboot. */
extern volatile int shim_usbc_out_persist;

/* Boot re-assert of the USB-C audio-out source: worker sets this to 0 or 1
 * ~5 s after start (Move's firmware is up and has sent its own default by
 * then); the RT consumer emits the SysEx pair and swaps it back to -1. */
extern volatile int shim_usbc_out_replay;

/* Live view of the two bits that together decide whether Main Out actually
 * reaches USB-C, republished by the RT path every frame (-1 until observed).
 * Unlike shim_usbc_out_persist these are levels, not edges: the worker polls
 * them to notice Move's sampling page clearing monitoring behind our back with
 * a lone 37 12. See usbc_gate_tick_monitor. */
extern volatile int shim_usbc_out_level;   /* 37 14: 0 = Mic, 1 = Main Out */
extern volatile int shim_usbc_monitor;     /* 37 12 bit1: monitoring engaged */

/* Deferred events (RT-safe to post; worker executes within ~200 ms). */
#define SHIM_EVT_OVERTAKE_EXIT_HOOK 1
#define SHIM_EVT_RESTART_MOVE       2
#define SHIM_EVT_SAMPLER_PREP       3  /* mkdir/fopen/header/writer for an armed recording */
#define SHIM_EVT_SAMPLER_FINALIZE   4  /* join writer, trim preroll, close + header */
#define SHIM_EVT_SAMPLER_CANCEL     5  /* preroll cancel: join writer, unlink file */
#define SHIM_EVT_SKIPBACK_SAVE      6  /* spawn the detached skipback writer */
#define SHIM_EVT_SKIPBACK_RESIZE    7  /* realloc the skipback ring */
#define SHIM_EVT_PREVIEW_PLAY       8  /* read preview cmd path, open + mmap */
#define SHIM_EVT_OVERTAKE_DSP_LOAD  9  /* dlopen + create_instance for an overtake module */
#define SHIM_EVT_OVERTAKE_DSP_FREE  10 /* destroy_instance + dlclose a retired overtake module */

void shim_worker_post(uint8_t evt);

/* Name the module currently being loaded, so the RT-thread audit can say WHICH
 * module a newly-realtime thread appeared behind.
 *
 * shadow_chain_mgmt.c calls this, and the host tests compile that file on its
 * own without the shim worker — a hard reference made
 * test_master_fx_cache_ownership fail to link. It carries a WEAK no-op
 * definition next to that call site, which this strong one overrides whenever
 * the worker is in the link. A weak *declaration* would have been the obvious
 * fix and only works on ELF: Darwin does not resolve an undefined weak symbol
 * to null, so the local suite failed to link where CI passed. A weak
 * definition works on both.
 *
 * Called from the SPI callback (module loading runs there), so it must stay
 * RT-safe: a bounded copy into a static buffer, no allocation, no lock. A torn
 * read at worst mislabels one log line, which is why the audit reports the
 * name as context rather than as proof. Pass NULL when the load finishes. */
void shim_rt_audit_note_module(const char *id);

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
    void (*overtake_dsp_load_pending)(void);
    void (*overtake_dsp_free_pending)(void);
} shim_worker_hooks_t;

void shim_worker_set_hooks(const shim_worker_hooks_t *hooks);

/* Spawn the worker thread (SCHED_OTHER, cores 0-2). Idempotent. */
void shim_worker_start(void);

#endif /* SHIM_WORKER_H */
