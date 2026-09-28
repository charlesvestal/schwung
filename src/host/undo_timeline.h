/* undo_timeline.h -- ONE undo history for Move's edits and Schwung's.
 *
 * Move's Undo button undoes Move's last edit. A take or p-lock made in
 * Schwung is not one, so without this Undo after recording automation undid
 * whatever NOTE edit came before it -- the automation stayed, and a note the
 * user wanted went. The user sees one instrument; it has one history.
 *
 * Move's side is READ, not modelled: move_model.c watches the top of flip's
 * undo stack (History<HistoryStoreMemory>: the last-undo node and the
 * first-redo node, each with its transaction id). Every transition of that
 * pair is one of three things:
 *
 *   Move UNDO   the new first-redo is the old last-undo;
 *   Move REDO   the new last-undo is the old first-redo;
 *   a NEW step  anything else (an edit, or Move squashing one into the top).
 *
 * Schwung's side is reported by the chain as it journals each of its own
 * edits (a take, a p-lock, a clear -- lane_edit.h), and each is ANCHORED to
 * Move's last-undo node at that moment. So the combined stack is known
 * exactly, and an Undo press is decided by one comparison:
 *
 *   the latest live Schwung edit is anchored at Move's CURRENT last-undo
 *   => nothing of Move's came after it => this Undo is ours.
 *
 * Otherwise the press goes to Move untouched. Redo mirrors it: an undone
 * Schwung edit anchored at the current last-undo is redone before Move's
 * next step, most recently undone first.
 *
 * A TAKE MADE WHILE MOVE RECORDED NOTES is one gesture, so it is one step: if
 * Move pushed a step between arm and (shortly after) the take's end, the take
 * is LINKED to that step instead of anchored after it. Undo then goes to Move
 * as usual, and the take follows Move's undo and redo of that step.
 *
 * Pure: no threads, no I/O. move_model_sync.c feeds it; tests/host drives it. */
#pragma once
#include <stdint.h>

typedef struct { uint64_t node, nbr; } ut_key_t;      /* {0,0} = none */

typedef struct {
    int      valid;
    ut_key_t undo;     /* Move's last-undo step, {0,0} when nothing is undoable */
    ut_key_t redo;     /* Move's first-redo step, {0,0} when nothing is redoable */
} ut_hist_t;

enum { UT_PLOCK = 1, UT_TAKE, UT_CLEAR, UT_EDIT,
       UT_RESET   /* the slot's whole store was replaced (a restore, the menu Undo):
                   * every edit journaled before it is void */ };

#define UT_MAX          24
#define UT_SLOT_DEPTH   8      /* == LANE_SJOURNAL_DEPTH: the chain keeps this many per slot */
#define UT_LINK_MS      600    /* after a take ends, how long a Move step may still claim it */

typedef struct {
    uint32_t jid;
    int      slot, kind;
    ut_key_t anchor;
    ut_key_t link;             /* non-zero: undone/redone WITH this Move step */
    int      used, undone;
    uint32_t seq, undo_seq;
    uint64_t t_ms;
    uint64_t link_until;       /* a take still accepting a link, until then */
} ut_entry_t;

typedef struct {
    ut_entry_t e[UT_MAX];
    ut_hist_t  last;
    uint32_t   seq, undo_seq;
    int        armed;
    uint64_t   arm_ms;
    uint64_t   move_step_ms;   /* the last NEW Move step */
    ut_key_t   move_step;
} ut_t;

/* Lane command out: ("lanes:journal", "undo <jid>") to a slot. */
typedef void (*ut_cmd_fn)(void *ctx, int slot, const char *key, const char *val);

void ut_reset(ut_t *u);

/* Move's history, every model tick. Linked takes follow Move's undo/redo. */
void ut_on_history(ut_t *u, const ut_hist_t *h, uint64_t t_ms, ut_cmd_fn cmd, void *ctx);

/* Record arm (Move's Record) -- a take's window starts here. */
void ut_on_arm(ut_t *u, int armed, uint64_t t_ms);

/* The chain journaled one of Schwung's own edits. */
void ut_on_schwung_edit(ut_t *u, int slot, uint32_t jid, int kind, uint64_t t_ms);

/* What Undo / Redo would take right now: 1 and (slot, jid) if it is a
 * Schwung edit, 0 if the press belongs to Move. */
int  ut_undo_target(const ut_t *u, int *slot, uint32_t *jid);
int  ut_redo_target(const ut_t *u, int *slot, uint32_t *jid);

/* The press was taken: issue the command and mark it. Returns 1 if it was
 * still the target (a stale claim does nothing). */
int  ut_take_undo(ut_t *u, int slot, uint32_t jid, ut_cmd_fn cmd, void *ctx);
int  ut_take_redo(ut_t *u, int slot, uint32_t jid, ut_cmd_fn cmd, void *ctx);
