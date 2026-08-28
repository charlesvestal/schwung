#include <string.h>

#include "shadow_midi_filter.h"   /* SHADOW_MIDI_IN_* geometry — the one copy */
#include "shadow_midi_inject_writer.h"
#include "shadow_overtake_midi.h"

static shadow_midi_inject_t overtake_midi_ring;

void shadow_overtake_midi_init(void)
{
    shadow_midi_inject_init(&overtake_midi_ring);
}

int shadow_overtake_midi_send(const uint8_t *msg, int len)
{
    if (!msg || len != 4) return 0;
    return shadow_midi_inject_push(&overtake_midi_ring, msg) == 0 ? 4 : 0;
}

void shadow_overtake_midi_discard(void)
{
    /* Same argument as the ROUTE_EXTERNAL discard in shadow_overtake_dsp_unload:
     * the producer is a destroyed instance and can no longer fire, and the
     * consumer is this same SPI thread, so re-initialising is safe here and
     * nowhere else. Without it the ring is a shim-lifetime static, so the next
     * module to load would drain the previous one's leftovers into Move as if
     * it had played them. */
    shadow_midi_inject_init(&overtake_midi_ring);
}

/* Copy packets into consecutive MIDI_IN slots starting at `first_event`.
 *
 * Bounded by BOTH the event count and the byte length. `max_events` alone was
 * enough while the only caller passed 31, but the rule in CLAUDE.md is flat --
 * bound every 8-stride walk with SHADOW_MIDI_IN_BYTES -- because the RX
 * display-status word sits at +248 and this is precisely how it gets clobbered.
 * A bound a caller can get wrong is not a bound. */
static int drain_ring(shadow_midi_inject_t *ring,
                      uint8_t *midi_in,
                      int first_event,
                      int max_events,
                      int midi_in_bytes)
{
    int copied = first_event;
    uint8_t packet[4];

    if (!ring) return copied;

    int slot_limit = midi_in_bytes / SHADOW_MIDI_IN_STRIDE;
    if (max_events > slot_limit) max_events = slot_limit;

    while (copied < max_events && shadow_midi_inject_peek(ring, packet)) {
        uint8_t *slot = &midi_in[copied * SHADOW_MIDI_IN_STRIDE];
        /* Occupied means a hardware event arrived after the caller's
         * all-slots-empty guard. Injecting alongside one races Move's firmware
         * MIDI read path and aborts, so leave the packet queued and retry next
         * frame rather than looking for a later hole. */
        if (slot[0] != 0) break;
        memcpy(slot, packet, 4);
        memset(slot + SHADOW_MIDI_IN_STRIDE - 4, 0, 4);  /* zero the timestamp */
        shadow_midi_inject_pop(ring);
        copied++;
    }
    return copied;
}

int shadow_overtake_midi_drain(shadow_midi_inject_t *shared,
                               shadow_midi_inject_t *ui,
                               int overtake_active,
                               uint8_t *midi_in,
                               int max_events,
                               int midi_in_bytes)
{
    int copied = 0;

    if (!midi_in || max_events <= 0 || midi_in_bytes < SHADOW_MIDI_IN_STRIDE)
        return 0;

    /* SHARED FIRST, and the order is load-bearing rather than incidental.
     *
     * The shim's overtake-exit releases -- shift-off, volume-touch-off,
     * back-off, jog-click-off -- go into the SHARED ring, and their own comment
     * calls them "the highest-consequence injects: a dropped release leaves
     * Move believing a control is still held". They are queued at exactly the
     * overtake -> 0 edge, i.e. the first frame this branch is reachable. Behind
     * a ringful of the departing DSP's notes they would wait extra frames, on
     * top of the exit hold the caller already applies. Before this feature the
     * shared ring was the only one, so draining it first is also what keeps the
     * old behaviour exactly.
     *
     * Nothing is lost either way -- what does not fit stays queued -- so this
     * is about latency on the packets that can leave Move wedged. */
    if (!overtake_active)
        copied = drain_ring(shared, midi_in, copied, max_events, midi_in_bytes);

    /* The shadow UI's ring drains in BOTH modes, and the ownership rule it
     * expresses is "who PUSHED it", not "what mode are we in".
     *
     * `move_midi_inject_to_move` used to push into the SHARED ring, so from
     * 2026-07-29 -- when the shim started popping that ring during overtake and
     * republishing it to the module as if it were a hardware press -- song-mode
     * lost both halves at once: its pad and Play packets never reached Move,
     * AND they came straight back into its own onMidiMessageInternal. Its Play
     * CC toggled its own playback and re-injected, so it fired pads as fast as
     * the 50ms inject throttle allowed, on a loop that could only be broken by
     * leaving the tool. Two producers cannot share one ring when the consumer
     * is chosen by mode; the test bus keeps `shared`, and the UI gets this. */
    copied = drain_ring(ui, midi_in, copied, max_events, midi_in_bytes);

    /* The dedicated ring drains in both modes. That is the whole point of the
     * split: while overtake owns the shared ring's consumer, an overtake DSP
     * still needs a way to reach Move. */
    copied = drain_ring(&overtake_midi_ring, midi_in, copied, max_events,
                        midi_in_bytes);
    return copied;
}
