/* clip_state.c — see clip_state.h for the three non-obvious rules. */

#include <string.h>
#include "clip_state.h"

int clip_pad_decode(int note, int *out_track, int *out_slot)
{
    if (note < CLIP_PAD_NOTE_MIN || note > CLIP_PAD_NOTE_MAX) return 0;
    /* Rows count DOWN from 92 as the track index goes up (note = 92 - 8t + s),
     * so the track falls out of (99 - note) / 8 -- not (92 - note) / 8, which
     * goes negative for track 1's own upper slots. */
    int t = (CLIP_PAD_NOTE_MAX - note) / 8;
    if (t < 0 || t >= CLIP_TRACKS) return 0;
    int s = note - (92 - 8 * t);
    if (s < 0 || s >= CLIP_SLOTS) return 0;
    if (out_track) *out_track = t;
    if (out_slot)  *out_slot  = s;
    return 1;
}

void clip_state_reset(clip_state_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
    for (int t = 0; t < CLIP_TRACKS; t++) {
        st->tracks[t].clip_slot = -1;
        st->queued_slot[t] = -1;
        st->saw_stop[t] = 0;
    }
}

/* Every playing clip returns to its top in lockstep with the counter
 * (measured 24/24 against the playhead), so anchor = 0 for everything we
 * already know is playing.
 *
 * Tracks whose identity we do NOT have stay unanchored: a clip could equally
 * have been launched after the start, and a refresh arriving later cannot
 * tell the two apart. Guessing 0 there would be confidently wrong. */
void clip_state_on_transport_start(clip_state_t *st)
{
    if (!st) return;
    for (int t = 0; t < CLIP_TRACKS; t++) {
        st->queued_slot[t] = -1;
        st->saw_stop[t] = 0;
        if (st->tracks[t].identity_valid && st->tracks[t].clip_slot >= 0) {
            st->tracks[t].anchor_valid = 1;
            st->tracks[t].anchor_pulse = 0;
        } else {
            /* Anchors from before the reset are in a dead timeline. */
            st->tracks[t].anchor_valid = 0;
        }
    }
}

void clip_state_on_led(clip_state_t *st, uint8_t status, uint8_t d1,
                       uint8_t d2, uint32_t pulses, int running, int ui_mode)
{
    if (!st) return;

    st->last_pulse = pulses;
    st->seen_pulse = 1;

    uint8_t type = status & 0xF0;
    if (type != 0x90 && type != 0x80) return;

    /* Only Session mode paints clips on these notes. See the header. */
    if (ui_mode != CLIP_UI_MODE_SESSION) return;

    int track, slot;
    if (!clip_pad_decode(d1, &track, &slot)) return;

    int ch = status & 0x0F;
    int on = (type == 0x90) && d2 > 0;

    if (ch == CLIP_CH_QUEUED) {
        if (on) st->queued_slot[track] = slot;
        return;
    }
    if (ch != CLIP_CH_PLAYING) return;   /* base colour: carries no state */

    clip_track_state_t *tr = &st->tracks[track];

    if (on) {
        /* RULE 2. Only a launch we saw queued may move the anchor. A bare ON
         * is Move re-emitting the grid for a clip that has been playing all
         * along, and taking it as an anchor corrupts a correct phase. */
        int was_queued = (st->queued_slot[track] == slot);
        int slot_changed = (!tr->identity_valid || tr->clip_slot != slot);

        tr->identity_valid = 1;
        tr->clip_slot = slot;

        /* Anchor only on a transition we WITNESSED: a queued launch, or a
         * clip starting on a track we watched fall silent. Both mean "this
         * began just now", which is what an anchor asserts.
         *
         * A slot that merely DIFFERS from what we remember is not evidence of
         * anything -- we may simply have been blind for a while (the scan is
         * gated during overtake). Anchoring on that would be the same guess
         * the refresh rule exists to refuse. */
        int witnessed = was_queued || st->saw_stop[track];

        if (running && witnessed) {
            tr->anchor_valid = 1;
            tr->anchor_pulse = pulses;
        } else if (slot_changed) {
            /* Identity is now right and the phase is not. Say so. */
            tr->anchor_valid = 0;
        }
        st->queued_slot[track] = -1;
        st->saw_stop[track] = 0;
    } else {
        /* Playing clip stopped. Identity is known (nothing is playing);
         * the anchor is meaningless. */
        if (tr->identity_valid && tr->clip_slot == slot) {
            tr->clip_slot = -1;
            tr->anchor_valid = 0;
            /* Witnessed silence. Whatever starts next on this track, we saw
             * it start. */
            st->saw_stop[track] = 1;
        }
    }
}

int clip_phase_beats(const clip_track_state_t *t, uint32_t pulses,
                     double loop_start, double loop_len, double *out_beats)
{
    if (!t || !out_beats) return 0;
    if (!t->identity_valid || t->clip_slot < 0) return 0;
    if (!t->anchor_valid) return 0;
    if (!(loop_len > 0.0)) return 0;
    if (pulses < t->anchor_pulse) return 0;   /* pre-anchor: not our timeline */

    double elapsed = (double)(pulses - t->anchor_pulse) / 24.0;
    double phase = elapsed - loop_len * (double)(long long)(elapsed / loop_len);
    if (phase < 0.0) phase = 0.0;
    *out_beats = loop_start + phase;
    return 1;
}
