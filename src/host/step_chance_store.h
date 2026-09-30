/*
 * The per-slot Chance store: which of Move's notes carry a condition.
 *
 * An entry names a note the way Move does -- its flip id, on a clip ROW of
 * this slot's track -- and caches the note's pitch and start so the SPI
 * callback can find it from bare MIDI. A note-on carries no id, only a pitch
 * and the moment it landed; Move's notes arrive stamped exactly on their start
 * phase (measured 2026-09-17), so pitch + phase is enough, within a tolerance
 * well under the finest grid (1/64 = 0.0625 q).
 *
 * 100% is the ABSENCE of an entry, so a slot that never had chance costs
 * nothing, and the store never carries an entry that means "do nothing".
 *
 * Pure, header-only, fixed-size: lives on the chain instance and is read on
 * the SPI callback. No allocation, no I/O. Serialize/parse run off-callback.
 */
#ifndef STEP_CHANCE_STORE_H
#define STEP_CHANCE_STORE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "step_chance.h"

#define SC_STORE_MAX 256
/* Half the finest grid step would be 0.031 q; a frame at 300 BPM is ~0.015. */
#define SC_MATCH_TOL 0.03
/* Rows at and above this are PARKING (a deleted clip's conditions waiting for
 * Move's Undo -- step_chance_follow.h), never a clip: not serialized. */
#define SC_ROW_PARK 200

typedef struct {
    uint8_t used;
    uint8_t row;       /* clip slot on the track, 0..7 */
    uint8_t pitch;
    uint8_t cond;      /* step_chance.h index; never SC_ALWAYS while used */
    double  start;     /* clip time, quarters -- Move's own coordinate */
    int64_t id;        /* Move's note id (flip) */
    double  grp;       /* the STEP it was set on (clip time): notes sharing it
                        * are one trig and roll once. A chord played in live
                        * starts a few ms apart per note, so its starts cannot
                        * be the key (measured: 17.425 / 17.428 / 17.434). */
} sc_entry_t;

typedef struct {
    sc_entry_t e[SC_STORE_MAX];
    uint32_t   rev;    /* bumps on every change */
} sc_store_t;

static inline int sc_store_count(const sc_store_t *st)
{
    int n = 0;
    for (int i = 0; i < SC_STORE_MAX; i++) n += st->e[i].used ? 1 : 0;
    return n;
}

static inline int sc__find(const sc_store_t *st, int row, int64_t id)
{
    for (int i = 0; i < SC_STORE_MAX; i++)
        if (st->e[i].used && st->e[i].row == row && st->e[i].id == id) return i;
    return -1;
}

/* Condition of one note; SC_ALWAYS if it has none. */
static inline int sc_store_get(const sc_store_t *st, int row, int64_t id)
{
    int i = sc__find(st, row, id);
    return i < 0 ? SC_ALWAYS : st->e[i].cond;
}

/* Set a note's condition; SC_ALWAYS removes it. 1 = done, 0 = refused (an
 * invalid condition, or the store is full). A full store never overwrites. */
static inline int sc_store_set_grp(sc_store_t *st, int row, int64_t id, int pitch,
                                   double start, int cond, double grp);
static inline int sc_store_set(sc_store_t *st, int row, int64_t id, int pitch,
                               double start, int cond)
{
    return sc_store_set_grp(st, row, id, pitch, start, cond, start);
}

/* Set a note's condition as part of a step's trig: `grp` is the step. */
static inline int sc_store_set_grp(sc_store_t *st, int row, int64_t id, int pitch,
                                   double start, int cond, double grp)
{
    if (!sc_valid(cond) || row < 0 || row > 255 || pitch < 0 || pitch > 127) return 0;
    int i = sc__find(st, row, id);
    if (cond == SC_ALWAYS) {
        if (i >= 0) { st->e[i].used = 0; st->rev++; }
        return 1;
    }
    if (i < 0) {
        for (int k = 0; k < SC_STORE_MAX; k++) if (!st->e[k].used) { i = k; break; }
        if (i < 0) return 0;
    }
    st->e[i] = (sc_entry_t){ 1, (uint8_t)row, (uint8_t)pitch, (uint8_t)cond, start, id, grp };
    st->rev++;
    return 1;
}

/* Move moved or re-pitched a note: its condition follows it. */
static inline void sc_store_relocate(sc_store_t *st, int row, int64_t id, int pitch, double start)
{
    int i = sc__find(st, row, id);
    if (i < 0 || pitch < 0 || pitch > 127) return;
    if (st->e[i].pitch == pitch && st->e[i].start == start) return;
    st->e[i].grp += start - st->e[i].start;   /* a nudged note keeps its offset in the trig */
    st->e[i].pitch = (uint8_t)pitch;
    st->e[i].start = start;
    st->rev++;
}

/* Move deleted notes: drop every entry on `row` whose start lies in
 * [lo, hi) and whose id is NOT among `live` -- the notes Move shows there
 * now. Only inside the window: outside it we have not looked, and absence
 * from a page we did not read says nothing. Returns how many went.
 *
 * Without this, a note deleted and another placed on the same step at the
 * same pitch inherits the old one's condition -- the match is by position. */
static inline int sc_store_prune_window(sc_store_t *st, int row, double lo, double hi,
                                        const int64_t *live, int n_live)
{
    int gone = 0;
    for (int i = 0; i < SC_STORE_MAX; i++) {
        sc_entry_t *e = &st->e[i];
        if (!e->used || e->row != row || e->start < lo || e->start >= hi) continue;
        int found = 0;
        for (int k = 0; k < n_live && !found; k++) found = (live[k] == e->id);
        if (!found) { e->used = 0; gone++; }
    }
    if (gone) st->rev++;
    return gone;
}

/* The condition for a note-on of `pitch` at clip phase `phase` on `row`.
 * A note on the window's first beat can be seen a hair under its END (the
 * frame fell before the wrap), so with a known window the distance wraps. */
/* `*wrap_out` is +1 when the note was matched ACROSS the wrap -- the phase
 * read a hair under the window's end for a note on its first beat. That note
 * belongs to the pass that is STARTING, and a caller counting passes from the
 * same clock is still on the one ending: A:B on the loop's first step flipped
 * parity whenever a frame landed there (13 drops in 30 passes of 1:2, on
 * hardware). -1 for the mirror case, 0 for a direct match. */
static inline int sc_store_match_ex(const sc_store_t *st, int row, int pitch, double phase,
                                    double loop_start, double loop_len, double *grp_out,
                                    int *wrap_out)
{
    (void)loop_start;
    for (int i = 0; i < SC_STORE_MAX; i++) {
        const sc_entry_t *e = &st->e[i];
        if (!e->used || e->row != row || e->pitch != pitch) continue;
        double d = fabs(phase - e->start);
        int wrap = 0;
        if (loop_len > 0.0) {
            double w = fabs(phase - loop_len - e->start);
            if (w < d) { d = w; wrap = 1; }
            w = fabs(phase + loop_len - e->start);
            if (w < d) { d = w; wrap = -1; }
        }
        if (d < SC_MATCH_TOL) {
            if (grp_out) *grp_out = e->grp;
            if (wrap_out) *wrap_out = wrap;
            return e->cond;
        }
    }
    return SC_ALWAYS;
}

static inline int sc_store_match(const sc_store_t *st, int row, int pitch, double phase,
                                 double loop_start, double loop_len)
{
    return sc_store_match_ex(st, row, pitch, phase, loop_start, loop_len, NULL, NULL);
}

/* "SC 1\n" then one "row id pitch start cond\n" per entry. Starts are %.17g,
 * so a triplet position is bit-exact after a reload. -1 if it does not fit --
 * never a truncated document. */
static inline int sc_store_serialize(const sc_store_t *st, char *buf, int len)
{
    int n = snprintf(buf, (size_t)len, "SC 1\n");
    if (n < 0 || n >= len) return -1;
    for (int i = 0; i < SC_STORE_MAX; i++) {
        const sc_entry_t *e = &st->e[i];
        if (!e->used || e->row >= SC_ROW_PARK) continue;
        int w = snprintf(buf + n, (size_t)(len - n), "%d %lld %d %.17g %d %.17g\n",
                         e->row, (long long)e->id, e->pitch, e->start, e->cond, e->grp);
        if (w < 0 || w >= len - n) return -1;
        n += w;
    }
    return n;
}

/* Replace the store with a document. Returns the entry count, or -1 with the
 * store UNTOUCHED if any line does not parse -- a half-applied document would
 * silently drop conditions. "" is a valid empty store. */
static inline int sc_store_parse(sc_store_t *st, const char *doc)
{
    static sc_store_t tmp;   /* off-callback only; keeps 6 KB off the stack */
    memset(&tmp, 0, sizeof tmp);
    tmp.rev = st->rev + 1;
    if (!doc || !*doc) { *st = tmp; return 0; }
    if (strncmp(doc, "SC 1\n", 5) != 0) return -1;
    const char *p = doc + 5;
    int k = 0;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll == 0) { p += eol ? 1 : 0; if (!eol) break; continue; }
        char line[96];
        if (ll >= sizeof line || k >= SC_STORE_MAX) return -1;
        memcpy(line, p, ll); line[ll] = 0;
        int row, pitch, cond, used = 0; long long id; double start, grp;
        if (sscanf(line, "%d %lld %d %lf %d%n", &row, &id, &pitch, &start, &cond, &used) != 5 ||
            row < 0 || row > 255 || pitch < 0 || pitch > 127 ||
            !sc_valid(cond) || cond == SC_ALWAYS || !isfinite(start))
            return -1;
        /* The group is an optional 6th field: a document written before it
         * existed groups each note by its own start, as it always did. */
        grp = start;
        if (line[used] != 0) {
            int used2 = 0;
            if (sscanf(line + used, " %lf%n", &grp, &used2) != 1 || line[used + used2] != 0 ||
                !isfinite(grp))
                return -1;
        }
        tmp.e[k++] = (sc_entry_t){ 1, (uint8_t)row, (uint8_t)pitch, (uint8_t)cond, start, (int64_t)id, grp };
        p += ll + (eol ? 1 : 0);
    }
    *st = tmp;
    return k;
}

#endif /* STEP_CHANCE_STORE_H */
