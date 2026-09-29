/*
 * The automation-lane defects the 1.6 review confirmed, each driven against
 * the REAL chain_lanes.c, chain_mod.c, chain_reorder.c and lane_*.c, with
 * assertions on what the fake module actually RECEIVED -- never on the chain's
 * own tables alone, because ':effective' is the chain's own number and reading
 * it back proves nothing about the plugin (see test_chain_lanes_playback.c).
 *
 *   1. LONG KEYS. The mod bus stored a lane's source id "lane:<target>:<param>"
 *      in 32 bytes, so a key of ~21+ characters never matched itself again:
 *      every block allocated a new source until all eight were full, the
 *      parameter froze, and the release could not find the source -- the knob
 *      stayed dead until the module was unloaded. ~400 minijv keys qualify.
 *   2. REMOVE. fx:remove orphaned the lane, and the very next block's
 *      fingerprint match un-orphaned it, so it drove whatever module slid into
 *      the position.
 *   3. MOVE. fx:move renamed the mod entry but not the lane's source id inside
 *      it, so the next tick added a SECOND override and the release only ever
 *      removed one: the parameter stuck.
 *   4. UNDO ACROSS A SET CHANGE. A restore that cleared the store left the
 *      outgoing set's lanes in the undo buffer, so "Undo automation" in the
 *      new set swapped the previous set's lanes in.
 *   6. "{}". An empty snapshot marker was refused by the parser, so a recall
 *      never took automation away.
 *   7. lanes:rev -- a content revision so the autosave can skip re-serialising
 *      an unchanged store on the SPI callback.
 *   8. The undo / stash / journal copies follow a permutation too.
 *   9. Adoption that frees a DRIVING twin releases it first.
 *  11. `:modulated` honours the kill switch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "chain_internal.h"

/* ------------------------------------------------------------------ stubs */
void chain_log(const char *msg) { (void)msg; }
void parse_debug_log(const char *msg) { (void)msg; }
void v2_chain_log(chain_instance_t *inst, const char *msg) { (void)inst; (void)msg; }
void v2_synth_panic(chain_instance_t *inst) { (void)inst; }
int v2_load_synth(chain_instance_t *inst, const char *m) { (void)inst; (void)m; return 0; }
void v2_unload_synth(chain_instance_t *inst) { (void)inst; }
int v2_load_audio_fx(chain_instance_t *inst, const char *m) { (void)inst; (void)m; return 0; }
void v2_unload_all_audio_fx(chain_instance_t *inst) { inst->fx_count = 0; }
int v2_load_midi_fx(chain_instance_t *inst, const char *m) { (void)inst; (void)m; return 0; }
void v2_unload_all_midi_fx(chain_instance_t *inst) { inst->midi_fx_count = 0; }
/* The shipped unloaders' two relevant effects (test_chain_reorder_routing.sh
 * pins that correspondence): drop the position's mod entries WITHOUT a base
 * restore, and forget the plugin. */
void v2_unload_audio_fx_slot(chain_instance_t *inst, int slot) {
    char t[16];
    chain_fx_component_id(t, sizeof(t), "fx", slot);
    chain_mod_clear_target_entries(inst, t, 0);
    inst->fx_plugins_v2[slot] = NULL;
    inst->fx_instances[slot] = NULL;
    inst->fx_is_v2[slot] = 0;
    inst->current_fx_modules[slot][0] = '\0';
}
void v2_unload_midi_fx_slot(chain_instance_t *inst, int slot) { (void)inst; (void)slot; }

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------ fake synth, one key */
static char SKEY[64];
static char SVAL[64];
static int  SWRITES;
static void s_set(void *i, const char *k, const char *v) {
    (void)i;
    if (!strcmp(k, SKEY)) { snprintf(SVAL, sizeof(SVAL), "%s", v); SWRITES++; }
}
static int s_get(void *i, const char *k, char *b, int n) {
    (void)i;
    if (!strcmp(k, SKEY)) return snprintf(b, n, "%s", SVAL);
    return 0;
}
static plugin_api_v2_t s_api = { .api_version = 2, .set_param = s_set, .get_param = s_get };

/* ------------------------------------------------------ fake FX, "mix" each */
typedef struct { char val[64]; } fx_t;
static fx_t FX[4];
static void f_set(void *i, const char *k, const char *v) {
    if (!strcmp(k, "mix")) snprintf(((fx_t *)i)->val, 64, "%s", v);
}
static int f_get(void *i, const char *k, char *b, int n) {
    if (!strcmp(k, "mix")) return snprintf(b, n, "%s", ((fx_t *)i)->val);
    return 0;
}
static audio_fx_api_v2_t f_api = { .api_version = AUDIO_FX_API_VERSION_2,
                                   .set_param = f_set, .get_param = f_get };

static const lane_fingerprint_t FP = { 0.0, 8.0, 3, 60 };

static chain_instance_t *new_inst(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    if (!inst || !chain_alloc_position_storage(inst)) { printf("alloc failed\n"); exit(2); }
    inst->lanes_enabled = 1;
    inst->lane_track = 0;
    inst->lane_clip_slot = 0;
    inst->lane_last_known_slot = -1;
    inst->lane_new_row = -1;
    return inst;
}

static void play(chain_instance_t *inst, double phase) {
    inst->clip_phase_valid = 1;
    inst->clip_loop_len = 8.0;
    inst->clip_loop_start = 0.0;
    inst->clip_fp_valid = 1;
    inst->clip_fp = FP;
    inst->clip_phase_beats = phase;
    lane_tick(inst);
}
static void stop(chain_instance_t *inst) {
    inst->clip_phase_valid = 0;
    inst->clip_loop_len = NAN;
    lane_tick(inst);
}

static int active_sources(chain_instance_t *inst, const char *t, const char *p) {
    mod_target_state_t *e = chain_mod_find_target_entry(inst, t, p);
    int n = 0;
    if (e && e->active)
        for (int i = 0; i < MAX_MOD_SOURCES_PER_TARGET; i++) n += e->sources[i].active;
    return n;
}

static void add_synth(chain_instance_t *inst, const char *key) {
    snprintf(SKEY, sizeof(SKEY), "%s", key);
    snprintf(SVAL, sizeof(SVAL), "10");
    SWRITES = 0;
    inst->synth_plugin_v2 = &s_api;
    inst->synth_instance = (void *)1;
    inst->synth_param_count = 1;
    chain_param_info_t *p = &inst->synth_params[0];
    snprintf(p->key, sizeof(p->key), "%s", key);
    p->type = KNOB_TYPE_FLOAT;
    p->min_val = 0; p->max_val = 127; p->default_val = 10;
}

static void add_fx(chain_instance_t *inst, int n) {
    inst->fx_count = n;
    for (int i = 0; i < n; i++) {
        snprintf(inst->current_fx_modules[i], MAX_NAME_LEN, "afx%d", i + 1);
        inst->fx_is_v2[i] = 1;
        inst->fx_plugins_v2[i] = &f_api;
        inst->fx_instances[i] = &FX[i];
        snprintf(FX[i].val, 64, "0.5");
        inst->fx_param_counts[i] = 1;
        snprintf(inst->fx_params[i][0].key, 32, "mix");
        inst->fx_params[i][0].type = KNOB_TYPE_FLOAT;
        inst->fx_params[i][0].min_val = 0;
        inst->fx_params[i][0].max_val = 1;
    }
}

/* ------------------------------------------------------------------ 1 */
static void long_key(const char *key) {
    printf("-- 1. a lane on a %zu-character key: %s\n", strlen(key), key);
    chain_instance_t *inst = new_inst();
    add_synth(inst, key);
    lane_t *ln = lane_alloc(&inst->lanes, "synth", key, 0, 0, &FP);
    CHECK(ln != NULL, "lane_alloc accepts the key");
    if (!ln) return;
    lane_write(ln, 0.0, 20.0f, 0);
    lane_write(ln, 4.0, 80.0f, 0);
    for (int b = 0; b < 20; b++) play(inst, b * 0.2);
    CHECK(active_sources(inst, "synth", key) == 1,
          "ONE source on the target after 20 blocks (got %d) -- one per block is the leak",
          active_sources(inst, "synth", key));
    const float driven = (float)atof(SVAL);
    CHECK(driven > 20.0f && driven < 80.0f,
          "the parameter FOLLOWS the curve at phase 3.8 (plugin=%s)", SVAL);
    stop(inst);
    CHECK(!chain_mod_is_target_active(inst, "synth", key),
          "the transport stop released the override");
    CHECK(atof(SVAL) == 10.0, "the plugin is back on the knob's base (plugin=%s)", SVAL);
    free(inst);
}

static void too_long_id_refused(void) {
    printf("-- 1b. an id the mod bus cannot hold is REFUSED and counted, never truncated\n");
    chain_instance_t *inst = new_inst();
    add_synth(inst, "cutoff");
    char big[MOD_SOURCE_ID_LEN + 8];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    const int before = inst->mod_source_id_refused;
    int rc = chain_mod_emit_override(inst, big, "synth", "cutoff", 50.0f, 1);
    CHECK(rc < 0, "emit of an over-long id fails (rc=%d)", rc);
    CHECK(active_sources(inst, "synth", "cutoff") == 0, "nothing was allocated for it");
    CHECK(inst->mod_source_id_refused == before + 1, "the refusal is counted (%d)",
          inst->mod_source_id_refused);
    /* The widest id a lane can build must fit, by construction. */
    char t[16], p[32], sid[128];
    memset(t, 't', 15); t[15] = '\0';
    memset(p, 'p', 31); p[31] = '\0';
    int n = snprintf(sid, sizeof(sid), "lane:%s:%s", t, p);
    CHECK(n < MOD_SOURCE_ID_LEN, "the widest lane id (%d chars) fits MOD_SOURCE_ID_LEN (%d)",
          n, MOD_SOURCE_ID_LEN);
    free(inst);
}

/* ------------------------------------------------------------------ 2 */
static void remove_orphans_for_good(void) {
    printf("-- 2. fx:remove: the lane on the departed module never drives its successor\n");
    chain_instance_t *inst = new_inst();
    add_fx(inst, 2);
    lane_t *ln = lane_alloc(&inst->lanes, "fx1", "mix", 0, 0, &FP);
    lane_write(ln, 0.0, 0.1f, 0);
    lane_write(ln, 8.0, 0.9f, 0);
    play(inst, 1.0);
    CHECK(atof(FX[0].val) < 0.3, "afx1 is driven before the remove (%s)", FX[0].val);
    int rc = chain_reorder_remove(inst, 0, 0);   /* afx2 slides into fx1 */
    CHECK(rc == 1, "remove fx1");
    for (int b = 0; b < 10; b++) play(inst, 1.0 + b * 0.1);
    CHECK(fabs(atof(FX[1].val) - 0.5) < 1e-6,
          "afx2, now at fx1, is NOT driven by afx1's lane (mix=%s)", FX[1].val);
    CHECK(!chain_mod_is_target_active(inst, "fx1", "mix"), "no override on fx1:mix");
    CHECK(lane_param_get(inst, "state", (char[512]){0}, 512) == 0,
          "the orphaned lane is not written to the document");
    char mod[8];
    CHECK(!lane_automates_param(inst, "fx1", "mix"), "fx1:mix does not claim to be automated");
    (void)mod;
    /* An explicit write restarts it, as for any orphan: the user is now
     * automating the module that IS there. */
    lane_param_set(inst, "plock", "fx1 mix 2 1 0.8");
    for (int b = 0; b < 3; b++) play(inst, 2.0 + b * 0.1);
    CHECK(fabs(atof(FX[1].val) - 0.8) < 1e-3,
          "a p-lock on the new fx1 plays on the new module (mix=%s)", FX[1].val);
    free(inst);
}

/* ------------------------------------------------------------------ 3 */
static void move_keeps_one_source(const char *verb) {
    printf("-- 3. fx:%s under a playing lane: one source, released on stop\n", verb);
    chain_instance_t *inst = new_inst();
    add_fx(inst, 2);
    lane_t *ln = lane_alloc(&inst->lanes, "fx2", "mix", 0, 0, &FP);
    lane_write(ln, 0.0, 0.1f, 0);
    lane_write(ln, 8.0, 0.9f, 0);
    play(inst, 1.0);
    const char *nt;
    if (!strcmp(verb, "move")) {
        CHECK(chain_reorder_move(inst, 0, 1, 0) == 1, "move fx2 -> fx1");
        nt = "fx1";
    } else {
        CHECK(chain_reorder_insert(inst, 0, 0) == 1, "insert at fx1");
        nt = "fx3";
    }
    CHECK(!strcmp(ln->target, nt), "the lane follows its module to %s", nt);
    play(inst, 4.0);
    CHECK(active_sources(inst, nt, "mix") == 1, "exactly one source on %s:mix (got %d)",
          nt, active_sources(inst, nt, "mix"));
    stop(inst);
    CHECK(!chain_mod_is_target_active(inst, nt, "mix"),
          "stop released %s:mix -- nothing left asserting under the old name", nt);
    CHECK(fabs(atof(FX[1].val) - 0.5) < 1e-6, "afx2 is back on its knob (mix=%s)", FX[1].val);
    free(inst);
}

/* ------------------------------------------------------------------ 8 */
static void copies_follow_permutation(void) {
    printf("-- 8. undo / edit-base / stash / journal copies are retargeted too\n");
    chain_instance_t *inst = new_inst();
    add_fx(inst, 3);
    lane_t *ln = lane_alloc(&inst->lanes, "fx2", "mix", 0, 0, &FP);
    lane_write(ln, 0.0, 0.1f, 0);
    lane_undo_take(inst);                         /* undo copy names fx2 */
    memcpy(&inst->lanes_edit_base, &inst->lanes, sizeof(inst->lanes));
    inst->lanes_stash[1].id = 1;
    inst->lanes_stash[1].n = 1;
    inst->lanes_stash[1].lanes[0] = *ln;
    lane_journal_entry_t *je = &inst->lanes_journal[1];
    je->id = 1; je->nrec = 1;
    snprintf(je->rec[0].target, 16, "fx2");
    snprintf(je->rec[0].param, 32, "mix");
    lane_journal_entry_t *se = &inst->lanes_sjournal[1];
    *se = *je;
    /* A journal entry for fx3, whose module will LEAVE. */
    lane_journal_entry_t *gone = &inst->lanes_journal[2];
    gone->id = 2; gone->nrec = 1;
    snprintf(gone->rec[0].target, 16, "fx3");
    snprintf(gone->rec[0].param, 32, "mix");

    CHECK(chain_reorder_move(inst, 0, 1, 0) == 1, "move fx2 -> fx1");
    CHECK(!strcmp(inst->lanes_undo.lanes[0].target, "fx1"), "undo copy -> fx1 (%s)",
          inst->lanes_undo.lanes[0].target);
    CHECK(!strcmp(inst->lanes_edit_base.lanes[0].target, "fx1"), "edit base -> fx1 (%s)",
          inst->lanes_edit_base.lanes[0].target);
    CHECK(!strcmp(inst->lanes_stash[1].lanes[0].target, "fx1"), "stash -> fx1 (%s)",
          inst->lanes_stash[1].lanes[0].target);
    CHECK(!strcmp(je->rec[0].target, "fx1"), "journal rec -> fx1 (%s)", je->rec[0].target);
    CHECK(!strcmp(se->rec[0].target, "fx1"), "own-edit journal rec -> fx1 (%s)", se->rec[0].target);

    CHECK(chain_reorder_remove(inst, 0, 2) == 1, "remove fx3");
    CHECK(gone->id == 0, "a journal entry naming the departed module is voided (id=%u)", gone->id);
    CHECK(je->id == 1, "an unrelated journal entry survives");

    /* An undo copy whose module left comes back SILENT, not re-aimed. */
    chain_instance_t *i2 = new_inst();
    add_fx(i2, 2);
    lane_t *l2 = lane_alloc(&i2->lanes, "fx1", "mix", 0, 0, &FP);
    lane_write(l2, 0.0, 0.1f, 0);
    lane_write(l2, 8.0, 0.9f, 0);
    lane_undo_take(i2);
    CHECK(chain_reorder_remove(i2, 0, 0) == 1, "remove fx1 (afx2 slides in)");
    lane_param_set(i2, "undo", "1");
    for (int b = 0; b < 5; b++) play(i2, 1.0 + b * 0.1);
    CHECK(fabs(atof(FX[1].val) - 0.5) < 1e-6,
          "after Undo, afx2 at fx1 is not driven by the departed module's lane (mix=%s)",
          FX[1].val);
    free(inst);
    free(i2);
}

/* ------------------------------------------------------------------ 4 */
static void undo_does_not_cross_restores(void) {
    printf("-- 4. a restore empties the undo buffer\n");
    chain_instance_t *inst = new_inst();
    add_synth(inst, "cutoff");
    const char *docA = "V 2\nL synth cutoff 0 0 0 8 3 60 1\nP 1 50 0\n";
    lane_param_set(inst, "state", docA);
    char buf[64];
    lane_param_get(inst, "undoable", buf, sizeof(buf));
    CHECK(!strcmp(buf, "0"), "nothing to undo after a load (%s)", buf);

    /* Set B has no lanes file: the restore's quiet clear. */
    lane_param_set(inst, "reset", "1");
    lane_param_get(inst, "undoable", buf, sizeof(buf));
    CHECK(!strcmp(buf, "0"), "lanes:reset leaves nothing to undo (%s)", buf);
    CHECK(lane_param_get(inst, "state", (char[512]){0}, 512) == 0, "reset emptied the store");

    /* A user clear in A, THEN a set change to B with its own file. */
    lane_param_set(inst, "state", docA);
    lane_param_set(inst, "clear", "1");
    lane_param_get(inst, "undoable", buf, sizeof(buf));
    CHECK(!strcmp(buf, "1"), "a user clear is undoable (%s)", buf);
    lane_param_set(inst, "state", "V 2\nL synth cutoff 0 1 0 8 3 60 1\nP 2 70 0\n");
    lane_param_get(inst, "undoable", buf, sizeof(buf));
    CHECK(!strcmp(buf, "0"), "the next set's load discards the old set's undo (%s)", buf);
    lane_param_set(inst, "undo", "1");
    char doc[512];
    lane_param_get(inst, "state", doc, sizeof(doc));
    CHECK(strstr(doc, "L synth cutoff 0 1 ") != NULL,
          "Undo did not swap set A's lanes into set B:\n%s", doc);

    /* reset is a RESTORE for the unified history: it journals nothing. */
    unsigned w = inst->lanes_edit_ev_w;
    uint32_t jid; int kind = 0;
    while (lane_take_edit_event(inst, &jid, &kind)) {}
    lane_param_set(inst, "reset", "1");
    int saw_clear = 0, saw_reset = 0;
    while (lane_take_edit_event(inst, &jid, &kind)) {
        if (kind == LANE_EDIT_CLEAR) saw_clear = 1;
        if (kind == LANE_EDIT_RESET) saw_reset = 1;
    }
    (void)w;
    CHECK(!saw_clear, "reset is not announced as a user CLEAR");
    CHECK(saw_reset, "reset is announced as a RESET");
    free(inst);
}

/* ------------------------------------------------------------------ 6 */
static void empty_marker_clears(void) {
    printf("-- 6. \"{}\" (the snapshot's empty marker) loads as no lanes\n");
    chain_instance_t *inst = new_inst();
    add_synth(inst, "cutoff");
    lane_param_set(inst, "state", "V 2\nL synth cutoff 0 0 0 8 3 60 2\nP 0 20 0\nP 4 80 0\n");
    play(inst, 1.0);
    CHECK(chain_mod_is_target_active(inst, "synth", "cutoff"), "the lane is driving");
    lane_param_set(inst, "state", "{}\n");
    CHECK(lane_param_get(inst, "state", (char[512]){0}, 512) == 0, "\"{}\" emptied the store");
    CHECK(!chain_mod_is_target_active(inst, "synth", "cutoff"), "and released the override");
    lane_param_set(inst, "state", "V 2\nL synth cutoff 0 0 0 8 3 60 1\nP 0 20 0\n");
    lane_param_set(inst, "state", "  \n");
    CHECK(lane_param_get(inst, "state", (char[512]){0}, 512) > 0,
          "a bare EMPTY document (a lost write) does NOT empty the store");
    /* A MALFORMED document still leaves the store alone. */
    lane_param_set(inst, "state", "V 2\nL synth cutoff 0 0 0 8 3 60 1\nP 0 20 0\n");
    lane_param_set(inst, "state", "garbage\n");
    CHECK(lane_param_get(inst, "state", (char[512]){0}, 512) > 0,
          "a malformed document still loses nothing");
    free(inst);
}

/* ------------------------------------------------------------------ 7 */
static void rev_tracks_content(void) {
    printf("-- 7. lanes:rev moves with the document and not with playback\n");
    chain_instance_t *inst = new_inst();
    add_synth(inst, "cutoff");
    char a[32], b[32];
    lane_param_get(inst, "rev", a, sizeof(a));
    lane_param_get(inst, "rev", b, sizeof(b));
    CHECK(a[0] && !strcmp(a, b), "stable with nothing changing (%s / %s)", a, b);
    lane_param_set(inst, "state", "V 2\nL synth cutoff 0 0 0 8 3 60 2\nP 0 20 0\nP 4 80 0\n");
    lane_param_get(inst, "rev", b, sizeof(b));
    CHECK(strcmp(a, b) != 0, "a load changes it");
    for (int k = 0; k < 10; k++) play(inst, k * 0.5);
    lane_param_get(inst, "rev", a, sizeof(a));
    stop(inst);
    char c[32];
    lane_param_get(inst, "rev", c, sizeof(c));
    CHECK(!strcmp(a, b) && !strcmp(a, c), "playback and a stop do not (%s %s %s)", b, a, c);
    lane_param_set(inst, "plock", "synth cutoff 2 1 99");
    lane_param_get(inst, "rev", c, sizeof(c));
    CHECK(strcmp(a, c) != 0, "a p-lock does");
    /* A point VALUE changing in place (same count) must move it too. */
    lane_param_get(inst, "rev", a, sizeof(a));
    inst->lanes.lanes[0].pts[0].value += 1.0f;
    lane_param_get(inst, "rev", b, sizeof(b));
    CHECK(strcmp(a, b) != 0, "an in-place value edit does");
    free(inst);
}

/* ------------------------------------------------------------------ 9 */
static void adoption_releases_twin(void) {
    printf("-- 9. adoption that frees a DRIVING twin releases its override\n");
    chain_instance_t *inst = new_inst();
    add_synth(inst, "cutoff");
    lane_t *twin = lane_alloc(&inst->lanes, "synth", "cutoff", 0, 0, &FP);
    lane_write(twin, 0.0, 20.0f, 0);
    lane_write(twin, 8.0, 80.0f, 0);
    play(inst, 1.0);
    CHECK(twin->driving && chain_mod_is_target_active(inst, "synth", "cutoff"), "twin drives");
    /* A blind take on the same key, with nothing in the window yet. */
    lane_fingerprint_t none = { 0.0, 0.0, 0, -1 };
    lane_t *take = lane_alloc(&inst->lanes, "synth", "cutoff", 0, LANE_SLOT_PENDING, &none);
    CHECK(take != NULL, "blind take allocated");
    if (!take) { free(inst); return; }
    take->pending_len = 8.0;
    take->origin_pending = 1;
    inst->lane_new_row = 0;
    play(inst, 1.2);
    CHECK(take->slot == 0, "the take adopted row 0 (slot=%d)", take->slot);
    stop(inst);
    CHECK(!chain_mod_is_target_active(inst, "synth", "cutoff"),
          "no override left behind by the displaced twin");
    CHECK(atof(SVAL) == 10.0, "the plugin is back on the knob (%s)", SVAL);
    free(inst);
}

/* ------------------------------------------------------------------ 11 */
static void modulated_honours_kill_switch(void) {
    printf("-- 11. :modulated honours lanes_off\n");
    chain_instance_t *inst = new_inst();
    add_synth(inst, "cutoff");
    lane_t *ln = lane_alloc(&inst->lanes, "synth", "cutoff", 0, 0, &FP);
    lane_write(ln, 0.0, 20.0f, 0);
    CHECK(lane_automates_param(inst, "synth", "cutoff"), "automated while enabled");
    inst->lanes_enabled = 0;
    CHECK(!lane_automates_param(inst, "synth", "cutoff"), "not automated with lanes_off");
    free(inst);
}

int main(void) {
    long_key("cutoff");
    long_key("nvram_patchCommon_patchlevel");                 /* minijv, 28 */
    long_key("abcdefghijklmnopqrstuvwxyz01234");              /* 31, the cap */
    too_long_id_refused();
    remove_orphans_for_good();
    move_keeps_one_source("move");
    move_keeps_one_source("insert");
    copies_follow_permutation();
    undo_does_not_cross_restores();
    empty_marker_clears();
    rev_tracks_content();
    adoption_releases_twin();
    modulated_honours_kill_switch();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: lane review fixes\n");
    return 0;
}
