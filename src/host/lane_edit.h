/* lane_edit.h -- automation that travels with Move's own edits.
 *
 * A Schwung lane lives beside Move's clip, and the user must never be able to
 * see the two disagree. So every edit Move makes to a clip's notes has a lane
 * counterpart, with the semantics MOVE'S OWN automation was measured to have
 * (2026-09-28, firmware 2.1.0, by watching the clip's envelopes in the live
 * model while driving each gesture -- docs/MOVE_MODEL.md):
 *
 *   step / page paste   the destination span's automation is REPLACED by the
 *                       source span's: every parameter, including ones only
 *                       the destination had locked (those are cleared);
 *   an "empty" source   (no notes, even if it carries automation) is a no-op
 *                       in Move, so it is never mirrored -- the caller only
 *                       calls this after the model CONFIRMED a paste;
 *   Delete + step       removes notes and KEEPS automation: nothing to do;
 *   Undo / Redo         restores notes and automation exactly: the journal.
 *
 * And for a clip that is deleted: its lanes are STASHED, not orphaned. Move's
 * Undo restores the very same clip object, and the stash is keyed by it.
 *
 * Pure: no chain, no host. tests/host drives it. */
#pragma once
#include <stdint.h>
#include "lane_store.h"

#define LANE_JOURNAL_LANES 16      /* lanes one paste can touch */
#define LANE_JOURNAL_DEPTH 8       /* pastes remembered for undo/redo */
#define LANE_STASH_DEPTH   4       /* deleted clips remembered for undo */

typedef struct {
    char target[16];
    char param[32];
    int  track, slot;
    int  nb, na;
    lane_point_t before[LANE_POINTS_MAX];   /* the destination span, before */
    lane_point_t after[LANE_POINTS_MAX];    /* ...and after the paste */
} lane_span_rec_t;

typedef struct {
    uint32_t id;                   /* 0 = empty */
    double   lo, len;              /* the destination span */
    int      nrec;
    lane_span_rec_t rec[LANE_JOURNAL_LANES];
} lane_journal_entry_t;

typedef struct {
    uint32_t id;                   /* 0 = empty */
    int      n;
    lane_t   lanes[LANE_MAX];
} lane_stash_t;

/* Which lanes a paste moves. NULL means all of the clip's -- a melodic paste,
 * where Move copies every note on the step. On a drum rack Move copies only
 * the SELECTED voice's notes, so the chain passes a scope admitting only that
 * voice's parameters (lane_voice_scope below). */
typedef int (*lane_scope_fn)(const lane_t *ln, void *ctx);

/* Replace [dst, dst+len) of every in-scope lane on (track, slot) with the points of
 * [src, src+len), shifted. A lane with nothing in either span is untouched.
 * The spans may overlap. Records before/after into `je` (id set by caller) so
 * lane_journal_apply can undo and redo it. Returns the lanes changed, or -1
 * if more than LANE_JOURNAL_LANES would be (refused whole, never partially:
 * a half-applied paste is exactly the desync this exists to prevent). */
int lane_paste_span(lane_store_t *st, int track, int slot, double src, double dst,
                    double len, lane_journal_entry_t *je, lane_scope_fn scope, void *scope_ctx);

/* A voice scope over the wire map lane_voice_map.mjs builds:
 *     "<note>:<key>,<key>,...;<note>:..."
 * Parsed once per paste (the map is ~10 KB for a 32-pad rack; only the
 * pasted notes' segments are kept), then asked once per lane. A lane is in
 * scope when its target is "synth" and its param is listed under one of the
 * notes. Keys under no voice -- the track's -- are out of scope: a voice paste
 * leaves them where they are, as it leaves every other voice's notes. */
#define LANE_VOICE_SCOPE_NOTES 16
typedef struct {
    int n;
    const char *lo[LANE_VOICE_SCOPE_NOTES], *hi[LANE_VOICE_SCOPE_NOTES];
} lane_voice_scope_t;

/* Fill `vs` from `map` for the notes in `notes` ("36,38"). Returns 1 if the
 * scope applies (a map, and at least one note parsed), 0 if the paste should
 * be whole-step -- no map, i.e. not a rack. A note the map does not list
 * contributes nothing, so a paste of only that voice moves no lanes. */
int lane_voice_scope_init(lane_voice_scope_t *vs, const char *map, const char *notes);
int lane_voice_scope(const lane_t *ln, void *vs);

/* Put each recorded lane's span back to `before` (to_after = 0, an undo) or
 * `after` (1, a redo). A lane that was since cleared is re-created. Returns
 * the lanes restored. */
int lane_journal_apply(lane_store_t *st, const lane_journal_entry_t *je, int to_after);

/* Move every lane of (track, slot) into `sh` and out of the store. */
int lane_stash_row(lane_store_t *st, int track, int slot, lane_stash_t *sh);

/* Bring a stash back, onto (track, slot) -- the clip may have come back in a
 * different slot. Returns the lanes restored; the stash is emptied. */
int lane_unstash_row(lane_store_t *st, lane_stash_t *sh, int track, int slot);

/* Points of `ln` inside [lo, lo+len). */
static inline int lane_point_in_span(double phase, double lo, double len)
{
    return phase >= lo - 1e-9 && phase < lo + len - 1e-9;
}
