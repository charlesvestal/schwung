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
        st->pending_start[t] = 0;
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
        st->pending_start[t] = 0;
        if (st->tracks[t].identity_valid && st->tracks[t].clip_slot >= 0) {
            st->tracks[t].anchor_valid = 1;
            st->tracks[t].anchor_pulse = 0;
            st->tracks[t].anchor_source = CLIP_ANCHOR_START;
            st->saw_stop[t] = 0;
        } else {
            /* Nothing playing here yet. Anchors from before the reset are in
             * a dead timeline, but whatever comes up next began at THIS
             * start -- so leave saw_stop alone and remember the start.
             * Clearing saw_stop here is what stranded a track that fell
             * silent moments before the Start: the clip returned and had no
             * witnessed transition left to justify an anchor. */
            st->tracks[t].anchor_valid = 0;
            st->pending_start[t] = 1;
        }
    }
}

void clip_state_on_led(clip_state_t *st, uint8_t status, uint8_t d1,
                       uint8_t d2, uint32_t pulses, int running, int ui_mode)
{
    if (!st) return;

    st->last_pulse = pulses;
    st->seen_pulse = 1;

    st->last_ui_mode = ui_mode;

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

        if (running && st->pending_start[track] && !was_queued &&
            pulses <= CLIP_START_GRACE_PULSES) {
            /* A Start said everything begins together; this track simply had
             * no identity at the time. Anchor at the START, not at the pulse
             * we happened to notice -- the repaint can lag a beat or two, and
             * anchoring where we noticed puts the lane that far out. */
            tr->anchor_valid = 1;
            tr->anchor_pulse = 0;
            tr->anchor_source = CLIP_ANCHOR_START;
        } else if (running && witnessed) {
            tr->anchor_valid = 1;
            tr->anchor_pulse = pulses;
            tr->anchor_source = CLIP_ANCHOR_LAUNCH;
        } else if (slot_changed) {
            /* Identity is now right and the phase is not. Say so. */
            tr->anchor_valid = 0;
        }
        st->queued_slot[track] = -1;
        st->saw_stop[track] = 0;
        st->pending_start[track] = 0;
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

void clip_state_anchor_pending(clip_state_t *st, uint32_t pulses, int running)
{
    if (!st || !running) return;
    if (pulses > CLIP_START_GRACE_PULSES) return;
    for (int t = 0; t < CLIP_TRACKS; t++) {
        clip_track_state_t *tr = &st->tracks[t];
        if (!st->pending_start[t]) continue;
        if (!tr->identity_valid || tr->clip_slot < 0) continue;
        if (tr->anchor_valid) { st->pending_start[t] = 0; continue; }
        /* The Start said everything begins together; this track's identity
         * merely arrived afterwards. */
        tr->anchor_valid = 1;
        tr->anchor_pulse = 0;
        tr->anchor_source = CLIP_ANCHOR_START;
        st->pending_start[t] = 0;
    }
}

int clip_state_derive_anchor(clip_track_state_t *tr, uint32_t pulses,
                             int bar, int playhead_idx, double step_beats,
                             double loop_start, double loop_len)
{
    (void)loop_start;
    if (!tr) return 0;
    if (!tr->identity_valid || tr->clip_slot < 0) return 0;
    if (tr->anchor_valid) return 0;          /* never overwrite real evidence */
    if (bar < 1 || playhead_idx < 0 || playhead_idx > 15) return 0;
    if (!(step_beats > 0.0) || !(loop_len > 0.0)) return 0;

    double pos = ((double)(bar - 1) * 16.0 + (double)playhead_idx) * step_beats;
    /* A page past the end of the loop means the bar and the clip disagree --
     * a stale page, or the wrong track. Refuse rather than fold it. */
    if (pos >= loop_len) return 0;

    double back = pos * 24.0;
    if (back > (double)pulses) {
        /* The clip has looped since the transport started. Wind the anchor
         * forward by whole loops rather than producing a negative one. */
        double loop_pulses = loop_len * 24.0;
        if (!(loop_pulses > 0.0)) return 0;
        while (back > (double)pulses) back -= loop_pulses;
        if (back < 0.0) return 0;
    }
    tr->anchor_pulse = (uint32_t)(pulses - (uint32_t)(back + 0.5));
    tr->anchor_valid = 1;
    tr->anchor_source = CLIP_ANCHOR_DERIVED;
    return 1;
}

int clip_state_solve_common_start(uint32_t pulses, double pos_beats,
                                  double loop_len, uint32_t coarse_start,
                                  uint32_t bracket_pulses,
                                  uint32_t *out_start)
{
    if (!out_start) return 0;
    if (!(loop_len > 0.0) || pos_beats < 0.0) return 0;

    long loop_pulses = (long)(loop_len * 24.0 + 0.5);
    if (loop_pulses <= 0) return 0;

    /* Ambiguous if the bracket spans a whole loop or more: two candidates sit
     * inside it and nothing chooses between them. Answering anyway would be a
     * coin flip presented as a measurement. */
    if ((long)(2u * bracket_pulses) >= loop_pulses) return 0;

    long at = (long)pulses - (long)(pos_beats * 24.0 + 0.5);   /* == start mod loop */
    if (at < 0) return 0;

    /* Walk candidates back to the one nearest the coarse estimate. */
    long best = at;
    long bestd = best - (long)coarse_start; if (bestd < 0) bestd = -bestd;
    for (long c = at - loop_pulses; c >= 0; c -= loop_pulses) {
        long dd = c - (long)coarse_start; if (dd < 0) dd = -dd;
        if (dd < bestd) { bestd = dd; best = c; }
        else break;          /* moving away; candidates are monotonic */
    }
    /* The winner must actually lie inside the bracket, or our coarse estimate
     * and the sighting disagree and neither should be trusted. */
    if (bestd > (long)bracket_pulses) return 0;
    *out_start = (uint32_t)best;
    return 1;
}

int clip_state_share_anchor(clip_state_t *st, int src_track,
                            const double *loop_len)
{
    if (!st || !loop_len) return 0;
    if (src_track < 0 || src_track >= CLIP_TRACKS) return 0;
    const clip_track_state_t *src = &st->tracks[src_track];
    if (!src->anchor_valid) return 0;
    double ls = loop_len[src_track];
    if (!(ls > 0.0)) return 0;

    int n = 0;
    for (int t = 0; t < CLIP_TRACKS; t++) {
        if (t == src_track) continue;
        clip_track_state_t *tr = &st->tracks[t];
        if (!tr->identity_valid || tr->clip_slot < 0) continue;
        if (tr->anchor_valid) continue;          /* never overwrite */
        double lt = loop_len[t];
        if (!(lt > 0.0)) continue;

        /* Does this track's loop divide the source's? Compared in
         * SIXTEENTHS as integers: loop lengths are multiples of a step, and
         * fmod on doubles would make 16.0 / 4.0 a question about floating
         * point rather than about music. */
        long a = (long)(ls * 4.0 + 0.5);
        long b = (long)(lt * 4.0 + 0.5);
        if (b <= 0 || (a % b) != 0) continue;    /* unsound: leave unknown */

        tr->anchor_pulse = src->anchor_pulse;
        tr->anchor_valid = 1;
        tr->anchor_source = CLIP_ANCHOR_DERIVED;
        n++;
    }
    return n;
}

/* Apply one known common start to every track that lacks an anchor. No
 * divisibility question arises: this is the real start, not a loop boundary. */
int clip_state_apply_common_start(clip_state_t *st, uint32_t start)
{
    if (!st) return 0;
    int n = 0;
    for (int t = 0; t < CLIP_TRACKS; t++) {
        clip_track_state_t *tr = &st->tracks[t];
        if (!tr->identity_valid || tr->clip_slot < 0) continue;
        if (tr->anchor_valid) continue;       /* never overwrite evidence */
        tr->anchor_pulse = start;
        tr->anchor_valid = 1;
        tr->anchor_source = CLIP_ANCHOR_DERIVED;
        n++;
    }
    return n;
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

/* ---- playhead ring (see clip_state.h) ---------------------------------- */
static clip_playhead_ev_t ph_ring[CLIP_PH_RING];
static volatile unsigned  ph_head;   /* SPI callback */
static unsigned           ph_tail;   /* worker        */

void clip_playhead_record(uint8_t idx, uint32_t pulses)
{
    unsigned h = ph_head;
    ph_ring[h % CLIP_PH_RING].idx = idx;
    ph_ring[h % CLIP_PH_RING].pulses = pulses;
    __atomic_store_n(&ph_head, h + 1, __ATOMIC_RELEASE);
}

int clip_playhead_take(clip_playhead_ev_t *out, int max)
{
    unsigned h = __atomic_load_n(&ph_head, __ATOMIC_ACQUIRE);
    int n = 0;
    /* Lapped: drop to the newest window rather than report stale events as
     * if they were current. */
    if (h - ph_tail > CLIP_PH_RING) ph_tail = h - CLIP_PH_RING;
    while (ph_tail != h && n < max) {
        out[n++] = ph_ring[ph_tail % CLIP_PH_RING];
        ph_tail++;
    }
    return n;
}
