/* The idle-tick handshake between the shim's idle gate and the chain host.
 *
 * Extracted from chain_host.c so the transitions are unit-testable host-side
 * without the full chain host. No side effects, no allocation — this runs on
 * the SPI callback.
 *
 * The shim skips render_block on a silent slot (one probe frame in 172, see
 * schwung_shim.c) and calls "mod:tick" instead, so LFO and MIDI FX timers keep
 * advancing. If a MIDI FX emits a message to the synth on such a frame, that
 * same block must render — otherwise the note reaches a slot that will not be
 * heard until the next probe, up to half a second later. Three facts have to
 * survive between the two calls the shim makes:
 *
 *   mark()    mod:tick advanced both timers this frame, and whether a MIDI FX
 *             delivered anything to the synth.
 *   take()    the shim asks, once, whether to un-park the slot. A one-shot: if
 *             the answer is no there will be no render, so the double-tick
 *             guard is cleared here rather than left standing for whichever
 *             frame renders next.
 *   consume() render_block asks whether it still owes a tick. After a wake the
 *             answer is no — mod:tick already did it, and ticking twice is a
 *             doubled LFO rate and a doubled arp.
 *
 * take() is what makes the pair self-clearing, so a shim that never calls it
 * (an older build with no dlsym for the export) costs at most one skipped tick
 * on the next render rather than leaving the guard latched forever.
 */
#ifndef CHAIN_IDLE_TICK_H
#define CHAIN_IDLE_TICK_H

typedef struct chain_idle_tick {
    int advanced;  /* mod:tick ran this frame; render_block must not re-tick */
    int wake;      /* a MIDI FX delivered to the synth; render this block */
} chain_idle_tick_t;

/* Record that the idle path advanced the timers. `delivered` is non-zero only
 * when a generated message actually reached the synth — a MIDI FX emitting
 * into a slot with no synth loaded has nothing to wake. */
static inline void chain_idle_tick_mark(chain_idle_tick_t *st, int delivered) {
    if (!st) return;
    st->wake = delivered ? 1 : 0;
    st->advanced = 1;
}

/* One-shot read of the wake result. Clears the double-tick guard when the
 * answer is no, because no render will come to consume it. */
static inline int chain_idle_tick_take(chain_idle_tick_t *st) {
    if (!st) return 0;
    int wake = st->wake;
    st->wake = 0;
    if (!wake) st->advanced = 0;
    return wake;
}

/* Returns 1 if render_block still owes a tick, 0 if the idle path already ran
 * it. Consumes the guard either way. */
static inline int chain_idle_tick_consume(chain_idle_tick_t *st) {
    if (!st) return 1;
    if (!st->advanced) return 1;
    st->advanced = 0;
    return 0;
}

#endif /* CHAIN_IDLE_TICK_H */
