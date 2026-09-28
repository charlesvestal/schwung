/* edit_follow.h -- make the automation lanes follow what MOVE did to a clip.
 *
 * Two inputs, deliberately independent:
 *   INTENTS  -- what the user asked for, read off the buttons (edit_gesture.h):
 *               a paste from one span to another, Undo, Redo;
 *   the MODEL -- what Move actually did (move_model.h): each clip's content
 *               hash (notes + Move's own automation), clip identities, and the
 *               edited clip's notes with their per-note ids.
 *
 * A lane edit is issued only where they AGREE:
 *   - a paste intent is mirrored when the notes that APPEARED in the
 *     destination span (ids new since the pre-state) are exactly copies of
 *     notes in the source span -- pitch, relative start, length, velocity.
 *     Anything else (Move declined an "empty" source, a range selection, an
 *     armed source Move had cleared) is dropped;
 *   - a Double Loop intent (a paste of the loop onto the new half) is
 *     mirrored when the loop actually doubled -- by geometry, since Move
 *     doubles a clip with no notes too;
 *   - Undo is mirrored when the clip returns EXACTLY to the state it had
 *     before a mirrored paste; Redo when it returns exactly to after.
 * And from the model alone, because clip identity needs no intent:
 *   - a deleted clip's lanes are STASHED under its id; the same id coming
 *     back (Move's Undo restores the very object) brings them back;
 *   - a new clip with the notes and geometry of one on its track is Move's
 *     Copy: the lanes are copied.
 *
 * Output is chain commands ("lanes:<verb>", value) through a callback.
 * Pure: tests/host drives it with synthetic models. */
#pragma once
#include <stdint.h>
#include "move_model.h"

enum { EF_PASTE = 1, EF_UNDO, EF_REDO, EF_DOUBLE };

typedef struct {
    int      kind;
    int      track, slot;          /* the clip it addressed */
    uint64_t clip_id;
    double   src, dst, len;        /* clip time, beats (EF_PASTE) */
    uint32_t pre_hash;             /* the clip's STATE (mm_clip_state_hash) when pressed */
    uint64_t t_ms;
} ef_intent_t;

typedef void (*ef_cmd_fn)(void *ctx, int slot, const char *key, const char *val);

typedef struct {
    const mm_note_t *notes; int n; mm_clip_ref_t ref;   /* one content state */
} ef_notes_t;

#define EF_PENDING   8
#define EF_JOURNAL   8     /* == LANE_JOURNAL_DEPTH on the chain side */
#define EF_STASH     4     /* == LANE_STASH_DEPTH */
#define EF_TIMEOUT_MS 600

void edit_follow_reset(void);
void edit_follow_intent(const ef_intent_t *in);

/* On every published model change. `now_notes` / `prev_notes` are the edited
 * clip's current and previous content states (move_model_edited_notes). */
void edit_follow_on_change(const move_model_t *now, const move_model_t *prev,
                           const ef_notes_t *now_notes, const ef_notes_t *prev_notes,
                           uint64_t t_ms, ef_cmd_fn cmd, void *ctx);

/* Every model tick, changed or not: settle pending intents against the
 * current state, and drop the ones the model never answered. */
void edit_follow_tick(const move_model_t *now, const ef_notes_t *now_notes, const ef_notes_t *prev_notes,
                      uint64_t t_ms, ef_cmd_fn cmd, void *ctx);

/* Diagnostics: counts since reset. */
typedef struct { int pasted, declined, undone, redone, stashed, restored, copied, expired; } ef_stats_t;
ef_stats_t edit_follow_stats(void);

/* The confirmation rule, exported for tests: do the notes that appeared in
 * [dst, dst+len) copy notes of [src, src+len)? 1 yes, 0 no (something else
 * happened), -1 nothing new has appeared yet. */
int edit_follow_is_paste(const ef_notes_t *pre, const ef_notes_t *post, double src, double dst, double len);
