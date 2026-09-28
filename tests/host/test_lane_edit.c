/* lane_edit: automation follows Move's step/page paste, its undo/redo, and a
 * deleted clip's undo -- with the semantics Move's OWN automation was measured
 * to have (docs/MOVE_MODEL.md, "Automation follows Move's edits"). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "lane_edit.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static lane_t *lock(lane_store_t *st, const char *param, double at, float v)
{
    lane_t *ln = lane_alloc(st, "synth", param, 1, 0, NULL);
    lane_write_span(ln, at, v, 1, 0.25);
    return ln;
}
static int count_in(const lane_t *ln, double lo, double len)
{
    int n = 0;
    for (int i = 0; i < ln->n; i++) if (lane_point_in_span(ln->pts[i].phase, lo, len)) n++;
    return n;
}
static float value_at(const lane_t *ln, double at)
{
    for (int i = 0; i < ln->n; i++) if (fabs(ln->pts[i].phase - at) < 1e-9) return ln->pts[i].value;
    return -1.0f;
}

int main(void)
{
    static lane_store_t st;
    static lane_journal_entry_t je;

    /* Step 1 -> step 9 (empty): the lock arrives, one step long. */
    lane_store_reset(&st);
    lane_t *a = lock(&st, "cutoff", 0.0, 0.8f);
    je.id = 1;
    CHECK(lane_paste_span(&st, 1, 0, 0.0, 2.0, 0.25, &je, NULL, NULL) == 1, "one lane changed");
    CHECK(value_at(a, 2.0) == 0.8f && value_at(a, 0.0) == 0.8f, "copied, source kept");
    CHECK(a->pts[1].span == 0.25f && a->pts[1].hold == 1, "still a one-step rectangle");

    /* Onto a step where ANOTHER parameter is locked: Move clears it (measured:
     * knob 2's lock on step 13 vanished when step 1, locked on knob 1 only,
     * was pasted over it). */
    lane_t *b = lock(&st, "bright", 3.0, 0.3f);
    je.id = 2;
    CHECK(lane_paste_span(&st, 1, 0, 0.0, 3.0, 0.25, &je, NULL, NULL) == 2, "both lanes recorded");
    CHECK(count_in(b, 3.0, 0.25) == 0, "the destination-only lock is cleared");
    CHECK(value_at(a, 3.0) == 0.8f, "the source lock is there");

    /* Undo puts the destination back exactly; redo re-applies. */
    CHECK(lane_journal_apply(&st, &je, 0) == 2, "undo touched both");
    CHECK(value_at(b, 3.0) == 0.3f && count_in(a, 3.0, 0.25) == 0, "undo restored");
    CHECK(lane_journal_apply(&st, &je, 1) == 2, "redo touched both");
    CHECK(count_in(b, 3.0, 0.25) == 0 && value_at(a, 3.0) == 0.8f, "redo re-applied");

    /* A lane with nothing in either span is not touched or recorded. */
    lane_t *c = lock(&st, "noise", 1.5, 0.5f);
    je.id = 3;
    lane_paste_span(&st, 1, 0, 0.0, 2.5, 0.25, &je, NULL, NULL);
    CHECK(je.nrec == 1 && c->n == 1, "an unrelated lane is left alone (nrec=%d)", je.nrec);

    /* Other clips are never touched. */
    lane_t *other = lane_alloc(&st, "synth", "cutoff", 1, 4, NULL);
    lane_write_span(other, 2.0, 0.1f, 1, 0.25);
    je.id = 4;
    lane_paste_span(&st, 1, 0, 0.0, 2.0, 0.25, &je, NULL, NULL);
    CHECK(value_at(other, 2.0) == 0.1f, "a different clip's lane kept its point");

    /* Page copy: 0..4 -> 4..8, sweeps and locks alike, shifted. */
    lane_store_reset(&st);
    lane_t *sw = lane_alloc(&st, "synth", "cutoff", 1, 0, NULL);
    lane_write(sw, 0.5, 0.1f, 0); lane_write(sw, 2.0, 0.9f, 0); lane_write(sw, 5.0, 0.4f, 0);
    je.id = 5;
    lane_paste_span(&st, 1, 0, 0.0, 4.0, 4.0, &je, NULL, NULL);
    CHECK(sw->n == 4 && value_at(sw, 4.5) == 0.1f && value_at(sw, 6.0) == 0.9f && value_at(sw, 5.0) < 0,
          "page replaced by the shifted source (n=%d)", sw->n);
    for (int i = 1; i < sw->n; i++) CHECK(sw->pts[i].phase > sw->pts[i - 1].phase, "sorted");

    /* Overlapping spans are safe. */
    je.id = 6;
    lane_paste_span(&st, 1, 0, 0.0, 2.0, 4.0, &je, NULL, NULL);
    CHECK(value_at(sw, 2.5) == 0.1f && value_at(sw, 4.0) == 0.9f, "overlap copied from the original");

    /* Refused whole when a lane would overflow -- never half-applied. */
    lane_store_reset(&st);
    lane_t *full = lane_alloc(&st, "synth", "cutoff", 1, 0, NULL);
    for (int i = 0; i < LANE_POINTS_MAX; i++) lane_write(full, 1.0 + i * 0.05, 0.5f, 0);
    lane_t *small = lane_alloc(&st, "synth", "bright", 1, 0, NULL);
    lane_write(small, 0.0, 0.2f, 0);
    int before_n = full->n;
    je.id = 7;
    CHECK(lane_paste_span(&st, 1, 0, 1.0, 10.0, 4.0, &je, NULL, NULL) == -1, "overflow refused");
    CHECK(full->n == before_n && small->n == 1, "nothing changed");

    /* A deleted clip's lanes are stashed and come back -- even in another slot. */
    lane_store_reset(&st);
    lane_t *d = lock(&st, "cutoff", 1.0, 0.7f);
    d->driving = 1;
    static lane_stash_t sh;
    CHECK(lane_stash_row(&st, 1, 0, &sh) == 1, "stashed");
    CHECK(!lane_find(&st, "synth", "cutoff", 1, 0), "gone from the store");
    CHECK(lane_unstash_row(&st, &sh, 1, 3) == 1, "unstashed");
    lane_t *e = lane_find(&st, "synth", "cutoff", 1, 3);
    CHECK(e && value_at(e, 1.0) == 0.7f && !e->driving && !e->orphaned, "same points, runtime state reset");
    CHECK(sh.n == 0, "the stash is spent");

    /* A DRUM paste is voice-scoped. Step 1 carries a kick lock (pad1_tune), a
     * snare lock (pad2_tune) and a track-level lock (master_cutoff); Move pastes
     * only the selected voice's notes, so only the kick's lock may follow. */
    {
        static lane_voice_scope_t vs;
        const char *map = "36:pad1_tune,pad1_decay;37:pad2_tune;38:pad3_tune";
        lane_store_reset(&st);
        lane_t *kick = lock(&st, "pad1_tune", 0.0, 0.3f);
        lane_t *snare = lock(&st, "pad2_tune", 0.0, 0.6f);
        lane_t *trk = lock(&st, "master_cutoff", 0.0, 0.9f);
        lane_t *dsnare = lock(&st, "pad2_tune", 2.0, 0.1f);   /* the destination's snare lock */
        (void)dsnare;
        CHECK(lane_voice_scope_init(&vs, map, "36") == 1 && vs.n == 1, "scope parsed");
        je.id = 9;
        CHECK(lane_paste_span(&st, 1, 0, 0.0, 2.0, 0.25, &je, lane_voice_scope, &vs) == 1,
              "only the kick lane moved");
        CHECK(value_at(kick, 2.0) == 0.3f, "kick lock followed its note");
        CHECK(value_at(snare, 0.0) == 0.6f, "snare's source lock stayed home");
        CHECK(value_at(lane_find(&st, "synth", "pad2_tune", 1, 0), 2.0) == 0.1f,
              "the destination's own snare lock survives a kick paste");
        CHECK(count_in(trk, 2.0, 0.25) == 0, "a track-level lock is no voice's");
        CHECK(lane_journal_apply(&st, &je, 0) == 1 && count_in(kick, 2.0, 0.25) == 0, "undo scoped too");

        CHECK(lane_voice_scope_init(&vs, "", "36") == 0, "no map: whole-step");
        CHECK(lane_voice_scope_init(&vs, map, "") == 0, "no notes: whole-step");
        CHECK(lane_voice_scope_init(&vs, map, "36,38") == 1 && vs.n == 2, "two voices");
        lane_t probe; memset(&probe, 0, sizeof probe);
        snprintf(probe.target, sizeof probe.target, "synth");
        snprintf(probe.param, sizeof probe.param, "pad3_tune");
        CHECK(lane_voice_scope(&probe, &vs), "second voice's key in scope");
        snprintf(probe.param, sizeof probe.param, "pad1_tun");
        CHECK(!lane_voice_scope(&probe, &vs), "a key PREFIX is not the key");
        snprintf(probe.param, sizeof probe.param, "pad1_decay");
        snprintf(probe.target, sizeof probe.target, "fx1");
        CHECK(!lane_voice_scope(&probe, &vs), "an FX lane is no voice's");
        CHECK(lane_voice_scope_init(&vs, map, "40") == 1 && vs.n == 0, "an unmapped note moves nothing");
    }

    if (fails) { printf("test_lane_edit: %d FAILED\n", fails); return 1; }
    printf("test_lane_edit: PASS\n");
    return 0;
}
