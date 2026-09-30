/*
 * Step chance FOLLOWS Move's edits: paste, Double Loop, clip copy on the
 * same track, clip delete + Undo.
 *
 * The events are the automation lanes' (host/edit_follow.h): each is issued
 * only after the live model confirms Move really made the edit, and reaches
 * the chain as a "lanes:" verb. Chance rides the SAME verbs rather than
 * detecting anything itself, so the two can never disagree about what Move
 * did.
 *
 * The one thing a copy cannot carry is Move's NOTE ID: pasted and copied
 * notes are new notes, and the chain never learns their ids. So a copied
 * condition gets a SYNTHETIC id (negative -- Move's are not) and still plays at
 * once, because playback matches by pitch + position (step_chance_store.h).
 * The shim ADOPTS it -- re-keys it to Move's real id -- the first time the
 * page it sits on is shown (step_menu.c), after which nudges follow it again.
 *
 * Rows >= SC_ROW_STASH are parking, never a clip: a deleted clip's conditions
 * wait there for Move's Undo. They never match at playback (rows are 0..7)
 * and are not serialized.
 *
 * Pure, fixed-size, RT-safe: runs on the SPI callback via v2_set_param.
 */
#ifndef STEP_CHANCE_FOLLOW_H
#define STEP_CHANCE_FOLLOW_H

#include "step_chance_store.h"

#define SC_ROW_STASH SC_ROW_PARK  /* 200..203: the four clip stashes */
#define SC_STASHES   4
#define SC_JOURNAL   8           /* == EF_JOURNAL */
#define SC_JREC      32

typedef struct {
    uint32_t   jid;              /* edit_follow's journal id; 0 = empty */
    int        row;
    int        nrem, nadd;
    int        overflow;         /* too big to journal: undo is refused, not partial */
    sc_entry_t rem[SC_JREC];     /* what the paste replaced in the destination */
    sc_entry_t add[SC_JREC];     /* what it wrote there */
} sc_journal_t;

static inline int sc__in(double x, double lo, double len)
{
    return x >= lo - 1e-9 && x < lo + len - 1e-9;
}

/* A RESTORED document's ids are HINTS. Move renumbers its notes when it
 * loads a set, so the id a condition was saved under can belong to a
 * different note now -- and the follow, which trusts ids, relocated the
 * condition onto it and pruned the rest of the chord (hardware, 2026-10-01).
 * Every restored entry takes a synthetic id instead, so the page follow
 * ADOPTS it by pitch + position, exactly as it does a copy. Playback never
 * needed the id: matching is by pitch + phase. Returns how many. */
static inline int sc_store_unbind_ids(sc_store_t *st)
{
    int n = 0;
    for (int i = 0; i < SC_STORE_MAX; i++)
        if (st->e[i].used) st->e[i].id = -(int64_t)(++n);
    if (n) st->rev++;
    return n;
}

/* A fresh synthetic id: one below the lowest in the store. */
static inline int64_t sc__synth_id(const sc_store_t *st)
{
    int64_t lo = 0;
    for (int i = 0; i < SC_STORE_MAX; i++)
        if (st->e[i].used && st->e[i].id < lo) lo = st->e[i].id;
    return lo - 1;
}

static inline int sc__add(sc_store_t *st, sc_entry_t e)
{
    for (int i = 0; i < SC_STORE_MAX; i++)
        if (!st->e[i].used) { e.used = 1; st->e[i] = e; st->rev++; return 1; }
    return 0;
}

/* Remove the entry on `row` at (pitch, start); any id. 1 if one went. */
static inline int sc__remove_at(sc_store_t *st, int row, int pitch, double start)
{
    for (int i = 0; i < SC_STORE_MAX; i++) {
        sc_entry_t *e = &st->e[i];
        if (e->used && e->row == row && e->pitch == pitch && fabs(e->start - start) < SC_MATCH_TOL) {
            e->used = 0; st->rev++; return 1;
        }
    }
    return 0;
}

/* PASTE / DOUBLE LOOP: [src, src+len) onto [dst, dst+len) on `row`. The
 * destination's conditions are REPLACED, as Move replaces its notes. `pitches`
 * (128 flags, or NULL for every pitch) scopes it to the notes Move actually
 * pasted -- a drum paste moves one pad. Journaled into `j` for Move's Undo.
 * Returns the number of conditions written. */
static inline int sc_store_paste(sc_store_t *st, int row, double src, double dst, double len,
                                 const uint8_t *pitches, sc_journal_t *j)
{
    sc_entry_t copy[SC_JREC];
    int ncopy = 0, over = 0;
    for (int i = 0; i < SC_STORE_MAX; i++) {
        const sc_entry_t *e = &st->e[i];
        if (!e->used || e->row != row || !sc__in(e->start, src, len)) continue;
        if (pitches && !pitches[e->pitch]) continue;
        if (ncopy < SC_JREC) copy[ncopy++] = *e; else over = 1;
    }
    if (j) { j->row = row; j->nrem = j->nadd = 0; j->overflow = over; }
    for (int i = 0; i < SC_STORE_MAX; i++) {
        sc_entry_t *e = &st->e[i];
        if (!e->used || e->row != row || !sc__in(e->start, dst, len)) continue;
        if (pitches && !pitches[e->pitch]) continue;
        if (j) { if (j->nrem < SC_JREC) j->rem[j->nrem++] = *e; else j->overflow = 1; }
        e->used = 0; st->rev++;
    }
    int added = 0;
    for (int k = 0; k < ncopy; k++) {
        sc_entry_t e = copy[k];
        e.start = e.start - src + dst;
        e.grp = e.grp - src + dst;
        e.id = sc__synth_id(st);
        if (!sc__add(st, e)) break;
        added++;
        if (j) { if (j->nadd < SC_JREC) j->add[j->nadd++] = e; else j->overflow = 1; }
    }
    return added;
}

/* Move's Undo (redo = 0) or Redo (1) of a journaled paste. Positions, not
 * ids: an adopted entry has Move's real id by now. 1 if applied. */
static inline int sc_journal_apply(sc_store_t *st, sc_journal_t *j, int redo)
{
    if (!j || !j->jid || j->overflow) return 0;
    const sc_entry_t *out = redo ? j->rem : j->add;
    const int nout = redo ? j->nrem : j->nadd;
    const sc_entry_t *in = redo ? j->add : j->rem;
    const int nin = redo ? j->nadd : j->nrem;
    for (int k = 0; k < nout; k++) sc__remove_at(st, j->row, out[k].pitch, out[k].start);
    for (int k = 0; k < nin; k++) {
        sc_entry_t e = in[k];
        sc__remove_at(st, j->row, e.pitch, e.start);
        if (redo) e.id = sc__synth_id(st);     /* Move's redo makes new notes */
        sc__add(st, e);
    }
    return 1;
}

/* Every condition on `from` moves to `to`, replacing whatever `to` held.
 * A clip's delete (to = a stash row) and its Undo (from = that stash row). */
static inline int sc_store_move_row(sc_store_t *st, int from, int to)
{
    int n = 0;
    for (int i = 0; i < SC_STORE_MAX; i++)
        if (st->e[i].used && st->e[i].row == to) { st->e[i].used = 0; st->rev++; }
    for (int i = 0; i < SC_STORE_MAX; i++)
        if (st->e[i].used && st->e[i].row == from) { st->e[i].row = (uint8_t)to; n++; st->rev++; }
    return n;
}

/* Move's clip COPY: `from`'s conditions onto `to`, as synthetic ids -- the
 * copy's notes are new notes. Replaces whatever `to` held. */
static inline int sc_store_copy_row(sc_store_t *st, int from, int to)
{
    if (from == to) return 0;
    for (int i = 0; i < SC_STORE_MAX; i++)
        if (st->e[i].used && st->e[i].row == to) { st->e[i].used = 0; st->rev++; }
    static sc_entry_t copy[SC_STORE_MAX];   /* 8 KB: never on the SPI callback's stack */
    int n = 0;
    for (int i = 0; i < SC_STORE_MAX; i++)
        if (st->e[i].used && st->e[i].row == from) copy[n++] = st->e[i];
    int added = 0;
    for (int k = 0; k < n; k++) {
        copy[k].row = (uint8_t)to;
        copy[k].id = sc__synth_id(st);
        if (!sc__add(st, copy[k])) break;
        added++;
    }
    return added;
}

/* The shim saw Move's note `id` at (pitch, start) on `row`: if a SYNTHETIC
 * condition sits there, it becomes this note's. 1 if adopted. */
static inline int sc_store_adopt(sc_store_t *st, int row, int64_t id, int pitch, double start)
{
    for (int i = 0; i < SC_STORE_MAX; i++)
        if (st->e[i].used && st->e[i].row == row && st->e[i].id == id) return 0;
    for (int i = 0; i < SC_STORE_MAX; i++) {
        sc_entry_t *e = &st->e[i];
        if (e->used && e->row == row && e->id < 0 && e->pitch == pitch &&
            fabs(e->start - start) < SC_MATCH_TOL) {
            e->id = id; e->start = start; st->rev++; return 1;
        }
    }
    return 0;
}

#endif /* STEP_CHANCE_FOLLOW_H */
