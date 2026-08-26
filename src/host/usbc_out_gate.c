/* usbc_out_gate.c - Boot arbitration for the USB-C audio-out preference.
 * See usbc_out_gate.h for the model and why it is causal rather than timed. */

#include "usbc_out_gate.h"

static void gate_persist(usbc_gate_t *g, usbc_gate_out_t *out, int v)
{
    out->persist = 1;
    out->persist_value = (int8_t)v;
    /* Track what we wrote. Without this a value would be written on every
     * repeat, and a change back to the original would compare equal to a
     * stale `stored` and be dropped as "no change". */
    g->stored = (int8_t)v;
}

static void gate_replay(usbc_gate_t *g, usbc_gate_out_t *out)
{
    g->replays_left--;
    out->replay = 1;
    out->replay_value = 1;   /* we only ever re-assert Main Out */
}

void usbc_gate_init(usbc_gate_t *g, int stored)
{
    g->stored = (int8_t)stored;
    g->phase = USBC_GATE_PHASE_PRE_REPLAY;
    g->replays_left = USBC_GATE_MAX_REPLAYS;
    g->saw_mic_assert = 0;
}

void usbc_gate_boot_replay(usbc_gate_t *g, usbc_gate_out_t *out)
{
    if (g->phase != USBC_GATE_PHASE_PRE_REPLAY) return;

    /* Only a stored Main Out needs defending. A stored Mic agrees with Move's
     * own boot default, so there is nothing to put on the wire and nothing to
     * arbitrate — open the gate and let the ordinary differs-from-stored test
     * do the rest. Same for an absent preference. */
    if (g->stored != 1) {
        g->phase = USBC_GATE_PHASE_SETTLED;
        return;
    }

    g->phase = USBC_GATE_PHASE_DEFENDING;
    gate_replay(g, out);
}

void usbc_gate_observe(usbc_gate_t *g, int observed, usbc_gate_out_t *out)
{
    int v = observed ? 1 : 0;

    switch (g->phase) {
    case USBC_GATE_PHASE_PRE_REPLAY:
        /* Nothing before our re-assert can be attributed to the user with any
         * confidence, so nothing here is persisted. We do record whether Move
         * has asserted its Mic default yet — that is the fact the defending
         * phase needs, and the whole reason a fast boot behaves differently
         * from a slow one. */
        if (v == 0) g->saw_mic_assert = 1;
        return;

    case USBC_GATE_PHASE_DEFENDING:
        if (v == 0) {
            /* We never replay Mic, so during the boot window a 0 can only have
             * come from Move. This is the observation the old wall-clock gate
             * mistook for a user choice and wrote to the file. Counter it. */
            g->saw_mic_assert = 1;
            if (g->replays_left > 0) {
                gate_replay(g, out);
            } else {
                /* Out of attempts. Open the gate rather than keep defending —
                 * staying shut would swallow every user change for the rest of
                 * the session, which is a worse failure than losing the
                 * argument about the boot default.
                 *
                 * Adopt Mic as the in-memory value but do NOT write it. That
                 * is the whole point: the file keeps the user's Main Out, so
                 * the next boot tries again, while we stop re-writing a value
                 * we never agreed with. Settling without this line would let
                 * the very next observation persist the Mic we just failed to
                 * override — reintroducing the bug at the bottom of the
                 * fallback path. Cost, in a path that needs Move to assert its
                 * default USBC_GATE_MAX_REPLAYS times in one boot: a genuine
                 * user Mic choice later in this session compares equal and is
                 * not written. */
                g->phase = USBC_GATE_PHASE_SETTLED;
                g->stored = 0;
            }
        } else {
            /* Main Out is live. Settle only once Move has actually asserted
             * its default this boot — otherwise this is just our own replay
             * echoing back on a slow boot, and settling on it would leave us
             * open to exactly the late assert we are here to defend against. */
            if (g->saw_mic_assert) g->phase = USBC_GATE_PHASE_SETTLED;
        }
        return;

    case USBC_GATE_PHASE_SETTLED:
    default:
        if (v != g->stored) gate_persist(g, out, v);
        return;
    }
}

void usbc_gate_force_settle(usbc_gate_t *g)
{
    /* Permits future writes; deliberately performs none. Whatever we were
     * defending stays the stored preference. */
    if (g->phase != USBC_GATE_PHASE_SETTLED)
        g->phase = USBC_GATE_PHASE_SETTLED;
}
