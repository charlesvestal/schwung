/* undo_timeline.c -- see undo_timeline.h. */
#include "undo_timeline.h"

#include <stdio.h>
#include <string.h>

static int key_eq(ut_key_t a, ut_key_t b) { return a.node == b.node && a.nbr == b.nbr; }
static int key_set(ut_key_t a) { return a.node != 0; }

void ut_reset(ut_t *u)
{
    memset(u, 0, sizeof *u);
}

static void emit(ut_entry_t *e, int redo, ut_cmd_fn cmd, void *ctx)
{
    char v[32];
    snprintf(v, sizeof v, "%s %u", redo ? "redo" : "undo", e->jid);
    if (cmd) cmd(ctx, e->slot, "lanes:journal", v);
}

/* A new edit -- of either side -- ends every undone Schwung edit's chance of
 * being redone, as a new edit does in Move. Linked takes die with their
 * Move step, which Move has just flushed from its redo list too. */
static void kill_redo_branch(ut_t *u, int keep_linked)
{
    for (int i = 0; i < UT_MAX; i++) {
        ut_entry_t *e = &u->e[i];
        /* A take linked to a Move step lives and dies with Move's own redo
         * list, which only a MOVE edit flushes: a Schwung edit leaves Move's
         * redo intact, so it must leave the take's too, or Move's Redo would
         * bring the notes back without their automation. */
        if (e->used && e->undone && !(keep_linked && key_set(e->link))) e->used = 0;
    }
}

/* Linked takes on one Move step, in the order their undo must run: NEWEST
 * first (a later whole-lane snapshot undone before an earlier one), and the
 * reverse for redo. Several share a step when a mirrored edit split a take. */
static int linked_on(ut_t *u, ut_key_t step, int undone, ut_entry_t **out)
{
    int n = 0;
    for (int i = 0; i < UT_MAX; i++) {
        ut_entry_t *e = &u->e[i];
        if (e->used && e->undone == undone && key_set(e->link) && key_eq(e->link, step)) out[n++] = e;
    }
    for (int a = 1; a < n; a++)                         /* by seq, ascending */
        for (int b = a; b > 0 && out[b - 1]->seq > out[b]->seq; b--) {
            ut_entry_t *t = out[b]; out[b] = out[b - 1]; out[b - 1] = t;
        }
    return n;
}

void ut_on_history(ut_t *u, const ut_hist_t *h, uint64_t t_ms, ut_cmd_fn cmd, void *ctx)
{
    if (!h || !h->valid) return;
    if (!u->last.valid) { u->last = *h; return; }
    const ut_hist_t was = u->last;
    u->last = *h;
    if (key_eq(was.undo, h->undo) && key_eq(was.redo, h->redo)) return;

    if (key_set(was.undo) && key_eq(h->redo, was.undo)) {
        /* Move undid its step `was.undo`: the takes linked to it go too,
         * newest first. */
        ut_entry_t *l[UT_MAX];
        const int n = linked_on(u, was.undo, 0, l);
        for (int k = n - 1; k >= 0; k--) {
            emit(l[k], 0, cmd, ctx);
            l[k]->undone = 1;
            l[k]->undo_seq = ++u->undo_seq;
        }
        return;
    }
    if (key_set(was.redo) && key_eq(h->undo, was.redo)) {
        ut_entry_t *l[UT_MAX];
        const int n = linked_on(u, was.redo, 1, l);
        for (int k = 0; k < n; k++) {                   /* oldest first */
            emit(l[k], 1, cmd, ctx);
            l[k]->undone = 0;
        }
        return;
    }

    /* A NEW Move step. */
    kill_redo_branch(u, 0);
    u->move_step = h->undo;
    u->move_step_ms = t_ms;
    /* A take still inside its window rides on it. Re-linking on every new
     * step in the window follows Move squashing its recording into a fresh
     * top node; the take always rides the step that is on top now. */
    for (int i = 0; i < UT_MAX; i++) {
        ut_entry_t *e = &u->e[i];
        if (e->used && !e->undone && e->kind == UT_TAKE && t_ms <= e->link_until && key_set(h->undo))
            e->link = h->undo;
    }
}

void ut_on_arm(ut_t *u, int armed, uint64_t t_ms)
{
    if (armed && !u->armed) u->arm_ms = t_ms;
    u->armed = armed ? 1 : 0;
}

void ut_on_schwung_edit(ut_t *u, int slot, uint32_t jid, int kind, uint64_t t_ms)
{
    if (kind == UT_RESET) {
        /* The chain replaced the slot's whole store: whatever it had
         * journaled describes lanes that are no longer there, and undoing one
         * would splice pre-restore content into the restored state. */
        for (int i = 0; i < UT_MAX; i++)
            if (u->e[i].used && u->e[i].slot == slot) u->e[i].used = 0;
        return;
    }
    if (!jid) return;
    kill_redo_branch(u, 1);
    /* The chain keeps UT_SLOT_DEPTH of a slot's edits; an older one's journal
     * is gone, and an entry that cannot be undone must not claim a press. */
    for (int i = 0; i < UT_MAX; i++) {
        ut_entry_t *e = &u->e[i];
        if (e->used && e->slot == slot && jid - e->jid >= UT_SLOT_DEPTH) e->used = 0;
    }
    ut_entry_t *slotp = NULL;
    for (int i = 0; i < UT_MAX && !slotp; i++) if (!u->e[i].used) slotp = &u->e[i];
    if (!slotp) {                                   /* full: the oldest goes */
        slotp = &u->e[0];
        for (int i = 1; i < UT_MAX; i++) if (u->e[i].seq < slotp->seq) slotp = &u->e[i];
    }
    memset(slotp, 0, sizeof *slotp);
    slotp->used = 1;
    slotp->jid = jid;
    slotp->slot = slot;
    slotp->kind = kind;
    slotp->anchor = u->last.undo;
    slotp->seq = ++u->seq;
    slotp->t_ms = t_ms;
    if (kind == UT_TAKE) {
        slotp->link_until = t_ms + UT_LINK_MS;
        /* Move stepped while the take was being recorded: they are one. */
        if (u->arm_ms && u->move_step_ms >= u->arm_ms && key_set(u->last.undo))
            slotp->link = u->last.undo;
    }
}

/* The latest live, unlinked Schwung edit. */
static const ut_entry_t *latest_live(const ut_t *u)
{
    const ut_entry_t *best = NULL;
    for (int i = 0; i < UT_MAX; i++) {
        const ut_entry_t *e = &u->e[i];
        if (!e->used || e->undone || key_set(e->link)) continue;
        if (!best || e->seq > best->seq) best = e;
    }
    return best;
}

int ut_undo_target(const ut_t *u, int *slot, uint32_t *jid)
{
    /* NOTHING IS CLAIMED WHILE A TAKE IS OPEN. The take is not in the
     * timeline until Record goes out, so an Undo now would restore an earlier
     * edit's whole-lane snapshot over the take being recorded -- erasing it,
     * and bringing the undone edit back when the take is later undone. */
    if (!u->last.valid || u->armed) return 0;
    const ut_entry_t *e = latest_live(u);
    if (!e || !key_eq(e->anchor, u->last.undo)) return 0;   /* Move's step is later */
    *slot = e->slot;
    *jid = e->jid;
    return 1;
}

int ut_redo_target(const ut_t *u, int *slot, uint32_t *jid)
{
    if (!u->last.valid || u->armed) return 0;
    const ut_entry_t *best = NULL;
    for (int i = 0; i < UT_MAX; i++) {
        const ut_entry_t *e = &u->e[i];
        if (!e->used || !e->undone || key_set(e->link) || !key_eq(e->anchor, u->last.undo)) continue;
        if (!best || e->undo_seq > best->undo_seq) best = e;
    }
    if (!best) return 0;
    *slot = best->slot;
    *jid = best->jid;
    return 1;
}

static ut_entry_t *find(ut_t *u, int slot, uint32_t jid)
{
    for (int i = 0; i < UT_MAX; i++)
        if (u->e[i].used && u->e[i].slot == slot && u->e[i].jid == jid) return &u->e[i];
    return NULL;
}

int ut_take_undo(ut_t *u, int slot, uint32_t jid, ut_cmd_fn cmd, void *ctx)
{
    int s; uint32_t j;
    if (!ut_undo_target(u, &s, &j) || s != slot || j != jid) return 0;
    ut_entry_t *e = find(u, slot, jid);
    emit(e, 0, cmd, ctx);
    e->undone = 1;
    e->undo_seq = ++u->undo_seq;
    return 1;
}

int ut_take_redo(ut_t *u, int slot, uint32_t jid, ut_cmd_fn cmd, void *ctx)
{
    int s; uint32_t j;
    if (!ut_redo_target(u, &s, &j) || s != slot || j != jid) return 0;
    ut_entry_t *e = find(u, slot, jid);
    emit(e, 1, cmd, ctx);
    e->undone = 0;
    return 1;
}
