/*
 * What a chain-shape edit does to the ROUTINGS, driven against a real
 * chain_instance_t.
 *
 * Reordering a chain stopped being a reload and became a permutation, and the
 * carries that compensated for the reload -- the opaque state blob, the
 * modulation base, the LFO target -- were deleted along with it. Deleting a
 * carry is only safe if the thing it used to carry is now preserved by
 * construction. Nothing proved that. These four behaviours were confirmed by
 * ear BEFORE the permutation existed, so those passes describe a mechanism that
 * is gone:
 *
 *   1. a modulated parameter keeps its BASE value across a move
 *   2. deleting a modulated FX clears the LFO that was aimed at it
 *   3. (JS side, tests/host/test_chain_lfo_routing_swap.sh)
 *   4. a knob mapped to fx4+ survives the edit -- and the reload
 *
 * tests/host/test_chain_permute.c already exercises chain_permute.h in
 * isolation, with a synthetic `world_t` and bare id strings. That is the wrong
 * altitude for any of the above: it can prove chain_perm_retarget REWRITES a
 * string, and cannot prove that chain_reorder.c hands it the three tables that
 * actually hold those strings, in the right order, with the right count, or
 * that a base_value sitting beside the rewritten string comes out untouched.
 * The bug shape this file exists for is a table that is never walked at all --
 * knob_mappings was the third one and was found by reading, not by grepping.
 *
 * So: a real chain_instance_t, the real chain_reorder.c, the real
 * chain_mod.c bookkeeping, and assertions on the struct fields the user hears.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain_internal.h"

/* ------------------------------------------------------------------ stubs */
/*
 * Only the things that dlopen or log. The modulation bus (chain_mod.c) and the
 * parameter lookup (chain_params.c) are the REAL ones -- they are half of what
 * is under test, because "the entry stays valid" is a claim about their data.
 */
void chain_log(const char *msg) { (void)msg; }
void parse_debug_log(const char *msg) { (void)msg; }
CHAIN_INTERNAL void v2_chain_log(chain_instance_t *inst, const char *msg) {
    (void)inst; (void)msg;
}
CHAIN_INTERNAL void v2_synth_panic(chain_instance_t *inst) { (void)inst; }
CHAIN_INTERNAL int v2_load_synth(chain_instance_t *inst, const char *m) {
    (void)inst; (void)m; return 0;
}
CHAIN_INTERNAL void v2_unload_synth(chain_instance_t *inst) { (void)inst; }
CHAIN_INTERNAL int v2_load_audio_fx(chain_instance_t *inst, const char *m) {
    (void)inst; (void)m; return 0;
}
CHAIN_INTERNAL void v2_unload_all_audio_fx(chain_instance_t *inst) { inst->fx_count = 0; }
CHAIN_INTERNAL int v2_load_midi_fx(chain_instance_t *inst, const char *m) {
    (void)inst; (void)m; return 0;
}
CHAIN_INTERNAL void v2_unload_all_midi_fx(chain_instance_t *inst) { inst->midi_fx_count = 0; }

/*
 * The two per-position unloaders chain_reorder_remove calls. The shipped ones
 * live in chain_host.c (which cannot be compiled natively -- it is the TU that
 * dlopens plugins) and chain_midi.c. Both do the same two things that matter
 * here: name the position, and drop its modulation entries through the REAL
 * chain_mod_clear_target_entries. That correspondence is not assumed -- the
 * companion .sh pins it against both shipped unloaders, because a fixture that
 * quietly did MORE than production would turn this file into a test of itself.
 */
static int unloaded_fx_slot = -1;
static int unloaded_midi_fx_slot = -1;

CHAIN_INTERNAL void v2_unload_audio_fx_slot(chain_instance_t *inst, int slot) {
    if (!inst || slot < 0 || slot >= MAX_AUDIO_FX) return;
    unloaded_fx_slot = slot;
    char target[16];
    chain_fx_component_id(target, sizeof(target), "fx", slot);
    chain_mod_clear_target_entries(inst, target, 0);
    inst->fx_plugins_v2[slot] = NULL;
    inst->fx_instances[slot] = NULL;
    inst->fx_is_v2[slot] = 0;
    inst->current_fx_modules[slot][0] = '\0';
}
CHAIN_INTERNAL void v2_unload_midi_fx_slot(chain_instance_t *inst, int slot) {
    if (!inst || slot < 0 || slot >= MAX_MIDI_FX) return;
    unloaded_midi_fx_slot = slot;
    char target[16];
    snprintf(target, sizeof(target), "midi_fx%d", slot + 1);   /* == chain_fx_component_id */
    chain_mod_clear_target_entries(inst, target, 0);
    inst->midi_fx_plugins[slot] = NULL;
    inst->midi_fx_instances[slot] = NULL;
    inst->current_midi_fx_modules[slot][0] = '\0';
}

/* ----------------------------------------------------------------- harness */
static int failures;

static void failf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    failures++;
}

#define EXPECT_STR(got, want, what) do { \
    if (strcmp((got), (want)) != 0) \
        failf("%s: got \"%s\", want \"%s\"", (what), (got), (want)); \
} while (0)

#define EXPECT_INT(got, want, what) do { \
    if ((got) != (want)) failf("%s: got %d, want %d", (what), (int)(got), (int)(want)); \
} while (0)

/* Modulation bases are compared EXACTLY. A base that has been through a save,
 * a reload or a re-read comes back as a nearby float, so a tolerant compare
 * here would pass for the exact regression this file is about. */
#define EXPECT_EXACT(got, want, what) do { \
    if ((got) != (want)) \
        failf("%s: got %.9g, want %.9g (exactly)", (what), (double)(got), (double)(want)); \
} while (0)

/* One fake plugin per position, so a modulation entry has something real to
 * point at and chain_mod can read and write through it. */
typedef struct { char log[256]; } fake_inst_t;
static fake_inst_t fake_fx[MAX_AUDIO_FX];
static fake_inst_t fake_midi_fx[MAX_MIDI_FX];

static void fake_set_param(void *instance, const char *key, const char *val) {
    fake_inst_t *fi = (fake_inst_t *)instance;
    size_t used = strlen(fi->log);
    snprintf(fi->log + used, sizeof(fi->log) - used, "%s=%.24s;", key, val);
}
static int fake_get_param(void *instance, const char *key, char *buf, int buf_len) {
    (void)instance; (void)key;
    snprintf(buf, buf_len, "0.5");
    return (int)strlen(buf);
}
static audio_fx_api_v2_t fake_fx_api = {
    .api_version = AUDIO_FX_API_VERSION_2,
    .set_param = fake_set_param,
    .get_param = fake_get_param,
};
static midi_fx_api_v1_t fake_midi_api = {
    .api_version = MIDI_FX_API_VERSION,
    .set_param = fake_set_param,
    .get_param = fake_get_param,
};

static chain_instance_t *inst;

/* A chain of `n_fx` audio FX and `n_mfx` MIDI FX, each with one parameter
 * whose key is unique to its position, so a lookup that lands on a neighbour
 * resolves nothing rather than resolving something plausible. */
static void seed_chain(int n_fx, int n_mfx) {
    memset(fake_fx, 0, sizeof(fake_fx));
    memset(fake_midi_fx, 0, sizeof(fake_midi_fx));

    memset(inst->mod_targets, 0, sizeof(inst->mod_targets));
    inst->mod_target_count = 0;
    memset(inst->lfos, 0, sizeof(inst->lfos));
    memset(inst->knob_mappings, 0, sizeof(inst->knob_mappings));
    inst->knob_mapping_count = 0;
    inst->dirty = 0;
    unloaded_fx_slot = -1;
    unloaded_midi_fx_slot = -1;

    for (int i = 0; i < MAX_AUDIO_FX; i++) {
        inst->fx_is_v2[i] = 0;
        inst->fx_plugins_v2[i] = NULL;
        inst->fx_instances[i] = NULL;
        inst->fx_param_counts[i] = 0;
        inst->current_fx_modules[i][0] = '\0';
        memset(inst->fx_params[i], 0, MAX_CHAIN_PARAMS * sizeof(chain_param_info_t));
    }
    for (int i = 0; i < MAX_MIDI_FX; i++) {
        inst->midi_fx_plugins[i] = NULL;
        inst->midi_fx_instances[i] = NULL;
        inst->midi_fx_param_counts[i] = 0;
        inst->current_midi_fx_modules[i][0] = '\0';
        memset(inst->midi_fx_params[i], 0, MAX_CHAIN_PARAMS * sizeof(chain_param_info_t));
    }

    inst->fx_count = n_fx;
    for (int i = 0; i < n_fx; i++) {
        snprintf(inst->current_fx_modules[i], MAX_NAME_LEN, "afx%d", i + 1);
        inst->fx_is_v2[i] = 1;
        inst->fx_plugins_v2[i] = &fake_fx_api;
        inst->fx_instances[i] = &fake_fx[i];
        inst->fx_param_counts[i] = 1;
        snprintf(inst->fx_params[i][0].key, sizeof(inst->fx_params[i][0].key), "p%d", i + 1);
        inst->fx_params[i][0].max_val = 1.0f;
        inst->fx_params[i][0].step = 0.01f;
    }
    inst->midi_fx_count = n_mfx;
    for (int i = 0; i < n_mfx; i++) {
        snprintf(inst->current_midi_fx_modules[i], MAX_NAME_LEN, "mfx%d", i + 1);
        inst->midi_fx_plugins[i] = &fake_midi_api;
        inst->midi_fx_instances[i] = &fake_midi_fx[i];
        inst->midi_fx_param_counts[i] = 1;
        snprintf(inst->midi_fx_params[i][0].key, sizeof(inst->midi_fx_params[i][0].key),
                 "m%d", i + 1);
        inst->midi_fx_params[i][0].max_val = 1.0f;
    }
}

/* A modulation entry as the runtime builds one: a base the user dialled in, a
 * source contributing on top, and an effective value that is neither. */
static mod_target_state_t *add_mod(const char *target, const char *param,
                                   float base, const char *source, float contrib) {
    int i = inst->mod_target_count;
    mod_target_state_t *e = &inst->mod_targets[i];
    memset(e, 0, sizeof(*e));
    e->active = 1;
    e->enabled = 1;
    snprintf(e->target, sizeof(e->target), "%s", target);
    snprintf(e->param, sizeof(e->param), "%s", param);
    e->base_value = base;
    e->min_val = 0.0f;
    e->max_val = 1.0f;
    e->type = KNOB_TYPE_FLOAT;
    e->sources[0].active = 1;
    snprintf(e->sources[0].source_id, sizeof(e->sources[0].source_id), "%s", source);
    e->sources[0].contribution = contrib;
    e->effective_value = base + contrib;
    inst->mod_target_count = i + 1;
    return e;
}

/* Find the entry by the param key, which no permutation may rewrite -- looking
 * it up by TARGET would beg the question. */
static mod_target_state_t *mod_by_param(const char *param) {
    for (int i = 0; i < inst->mod_target_count; i++) {
        if (inst->mod_targets[i].active &&
            strcmp(inst->mod_targets[i].param, param) == 0) return &inst->mod_targets[i];
    }
    return NULL;
}

static void set_lfo(int n, const char *target, const char *param) {
    lfo_state_t *l = &inst->lfos[n];
    memset(l, 0, sizeof(*l));
    l->active = 1;
    l->depth = 0.5f;
    snprintf(l->target, sizeof(l->target), "%s", target);
    snprintf(l->param, sizeof(l->param), "%s", param);
}

static void add_knob(int cc, const char *target, const char *param, float value) {
    int i = inst->knob_mapping_count++;
    knob_mapping_t *k = &inst->knob_mappings[i];
    memset(k, 0, sizeof(*k));
    k->cc = cc;
    snprintf(k->dests[0].target, sizeof(k->dests[0].target), "%s", target);
    snprintf(k->dests[0].param, sizeof(k->dests[0].param), "%s", param);
    k->dests[0].lo = 0.0f;
    k->dests[0].hi = 1.0f;
    k->dests[0].current_value = value;
    k->dest_count = 1;   /* a mapping with no destinations is not a mapping */
}

static knob_mapping_t *knob_by_cc(int cc) {
    for (int i = 0; i < inst->knob_mapping_count; i++)
        if (inst->knob_mappings[i].cc == cc) return &inst->knob_mappings[i];
    return NULL;
}

/*
 * THE INVARIANT, asserted separately from any expected ordering.
 *
 * Whatever a shape edit did, no routing may afterwards name a position that is
 * not in the chain. This does not care where anything went; it is the check
 * that survives someone changing what "should" happen, and it is the one that
 * catches a table nobody walked -- a stale "fx6" in a five-long chain is
 * exactly the shape of "the fourth string table was never retargeted".
 */
static void check_no_dangling(const char *what) {
    struct { const char *id; const char *kind; } rows[MAX_MOD_TARGETS + LFO_COUNT + MAX_KNOB_MAPPINGS];
    int n = 0;
    for (int i = 0; i < inst->mod_target_count; i++) {
        if (!inst->mod_targets[i].active) continue;
        rows[n].id = inst->mod_targets[i].target; rows[n].kind = "a modulation target"; n++;
    }
    for (int i = 0; i < LFO_COUNT; i++) {
        if (!inst->lfos[i].active) continue;
        rows[n].id = inst->lfos[i].target; rows[n].kind = "an LFO"; n++;
    }
    for (int i = 0; i < inst->knob_mapping_count; i++) {
        rows[n].id = inst->knob_mappings[i].dests[0].target; rows[n].kind = "a knob mapping"; n++;
    }
    for (int i = 0; i < n; i++) {
        const char *id = rows[i].id;
        if (!id[0]) continue;
        int fx = chain_fx_index_from_id(id, "fx", MAX_AUDIO_FX);
        int mfx = chain_fx_index_from_id(id, "midi_fx", MAX_MIDI_FX);
        if (fx >= 0 && fx >= inst->fx_count)
            failf("%s: %s still names %s, past a chain of %d audio FX",
                  what, rows[i].kind, id, inst->fx_count);
        if (mfx >= 0 && mfx >= inst->midi_fx_count)
            failf("%s: %s still names %s, past a chain of %d MIDI FX",
                  what, rows[i].kind, id, inst->midi_fx_count);
    }
}

/* ======================================================================== */
/* 1. A MODULATED PARAMETER KEEPS ITS BASE ACROSS A MOVE                    */
/* ======================================================================== */
/*
 * The base used to be CARRIED across the renumber by the editor, read out of
 * the DSP and written back after the reload. The permutation deleted that,
 * on the argument that the entry simply stays valid. That argument is only
 * true if the base travels with the entry rather than being recomputed, and
 * the failure mode if it is not is quiet: the parameter lands wherever the
 * modulation happens to be at that instant and stays there.
 */
static void test_move_preserves_base(void) {
    seed_chain(4, 0);
    mod_target_state_t *a = add_mod("fx2", "p2", 0.375f, "lfo1", 0.25f);
    mod_target_state_t *b = add_mod("fx4", "p4", 0.125f, "lfo2", -0.05f);
    float a_eff = a->effective_value, b_eff = b->effective_value;
    int count_before = inst->mod_target_count;

    /* Drag fx2 to the end of a four-long chain. */
    EXPECT_INT(chain_reorder_move(inst, 0, 1, 3), 1, "move fx2 to fx4 accepted");

    mod_target_state_t *moved = mod_by_param("p2");
    if (!moved) { failf("the modulation entry for the moved FX vanished"); return; }
    EXPECT_STR(moved->target, "fx4", "the entry followed its module");
    EXPECT_EXACT(moved->base_value, 0.375f, "the BASE the user dialled in survived the move");
    EXPECT_EXACT(moved->effective_value, a_eff, "the effective value survived the move");
    EXPECT_INT(moved->active, 1, "the entry is still active");
    EXPECT_INT(moved->enabled, 1, "the entry is still enabled");
    EXPECT_INT(moved->sources[0].active, 1, "the modulation SOURCE survived the move");
    EXPECT_STR(moved->sources[0].source_id, "lfo1", "the source id survived");
    EXPECT_EXACT(moved->sources[0].contribution, 0.25f, "the source contribution survived");

    /* The module that only slid down one keeps everything too -- a permutation
       that rebuilt entries would flatten this one just as thoroughly. */
    mod_target_state_t *slid = mod_by_param("p4");
    if (!slid) { failf("the modulation entry for the FX behind it vanished"); return; }
    EXPECT_STR(slid->target, "fx3", "the FX behind the move slid down one");
    EXPECT_EXACT(slid->base_value, 0.125f, "an untouched entry kept its base");
    EXPECT_EXACT(slid->effective_value, b_eff, "an untouched entry kept its effective value");
    EXPECT_INT(inst->mod_target_count, count_before, "a move invented or dropped an entry");
    EXPECT_INT(inst->dirty, 1, "a move did not mark the chain dirty");
    check_no_dangling("after a move");

    (void)b;

    /* The same for the other two verbs: an insert ahead of a modulated FX, and
       a removal of one in front of it, both renumber it without touching the
       value the user set. */
    seed_chain(3, 0);
    add_mod("fx2", "p2", 0.6875f, "lfo1", 0.1f);
    EXPECT_INT(chain_reorder_insert(inst, 0, 0), 1, "insert at the head accepted");
    {
        mod_target_state_t *e = mod_by_param("p2");
        if (!e) failf("the modulation entry did not survive an insert");
        else {
            EXPECT_STR(e->target, "fx3", "the entry followed the insert");
            EXPECT_EXACT(e->base_value, 0.6875f, "the base survived an insert ahead of it");
        }
    }

    seed_chain(3, 0);
    add_mod("fx3", "p3", 0.8125f, "lfo1", 0.1f);
    EXPECT_INT(chain_reorder_remove(inst, 0, 0), 1, "remove of the head accepted");
    {
        mod_target_state_t *e = mod_by_param("p3");
        if (!e) failf("the modulation entry did not survive the removal of an earlier FX");
        else {
            EXPECT_STR(e->target, "fx2", "the entry followed the removal down one");
            EXPECT_EXACT(e->base_value, 0.8125f, "the base survived a removal in front of it");
        }
    }

    /* And the MIDI section, which is the one the reported crash was in. */
    seed_chain(0, 3);
    add_mod("midi_fx3", "m3", 0.4375f, "lfo2", 0.2f);
    EXPECT_INT(chain_reorder_move(inst, 1, 2, 0), 1, "move midi_fx3 to the head accepted");
    {
        mod_target_state_t *e = mod_by_param("m3");
        if (!e) failf("the MIDI FX modulation entry vanished on a move");
        else {
            EXPECT_STR(e->target, "midi_fx1", "the MIDI FX entry followed its module");
            EXPECT_EXACT(e->base_value, 0.4375f, "the MIDI FX base survived the move");
        }
    }
}

/* ======================================================================== */
/* 2. DELETING A MODULATED FX CLEARS THE LFO AIMED AT IT                    */
/* ======================================================================== */
/*
 * An LFO names its destination by position string, and a position keeps its
 * name when its occupant leaves. So a delete that renumbers without clearing
 * leaves the LFO emitting into whatever slid into that index -- audible, and
 * attributable to nothing the user did. Both halves have to go: leaving
 * `param` behind stores the departed module parameter name, ready for the next
 * write of `target` alone to revive half a routing.
 */
static void test_remove_clears_lfo(void) {
    seed_chain(3, 0);
    add_mod("fx2", "p2", 0.5f, "lfo1", 0.1f);
    add_mod("fx3", "p3", 0.25f, "lfo2", 0.1f);
    set_lfo(0, "fx2", "p2");
    set_lfo(1, "fx3", "p3");

    EXPECT_INT(chain_reorder_remove(inst, 0, 1), 1, "remove of fx2 accepted");
    EXPECT_INT(unloaded_fx_slot, 1, "the removed position was unloaded before the shift");
    EXPECT_INT(inst->fx_count, 2, "the chain did not shrink");

    EXPECT_STR(inst->lfos[0].target, "", "the LFO aimed at the DELETED FX was not cleared");
    EXPECT_STR(inst->lfos[0].param, "",
               "the LFO kept the parameter name of a module that left -- a later "
               "write of target alone revives half a routing");
    EXPECT_INT(inst->lfos[0].active, 0, "the orphaned LFO is still marked active");

    /* ...and the one behind it followed, rather than being cleared along with
       it or left pointing at the index the deleted module vacated. */
    EXPECT_STR(inst->lfos[1].target, "fx2", "the LFO behind the deletion did not follow");
    EXPECT_STR(inst->lfos[1].param, "p3", "the surviving LFO lost its parameter");
    EXPECT_INT(inst->lfos[1].active, 1, "the surviving LFO was deactivated");

    /* The modulation entry of the deleted module is gone (the unloader), and
       the one behind it survived intact with its base (the permutation). */
    if (mod_by_param("p2")) failf("the deleted FX left an active modulation entry behind");
    {
        mod_target_state_t *e = mod_by_param("p3");
        if (!e) failf("deleting fx2 also destroyed the modulation of fx3");
        else {
            EXPECT_STR(e->target, "fx2", "the surviving modulation entry did not follow down");
            EXPECT_EXACT(e->base_value, 0.25f, "the surviving entry lost its base");
        }
    }
    check_no_dangling("after a remove");

    /* BOTH LFOs. Two is the whole table, and a loop that stops at the first is
       a shape this codebase keeps growing. */
    seed_chain(3, 0);
    set_lfo(0, "fx2", "p2");
    set_lfo(1, "fx2", "p2");
    chain_reorder_remove(inst, 0, 1);
    EXPECT_STR(inst->lfos[0].target, "", "LFO 1 survived the deletion");
    EXPECT_STR(inst->lfos[1].target, "",
               "only the FIRST LFO was cleared -- LFO 2 still names the deleted position");

    /* An LFO aimed at the SYNTH, or at the other section, is none of an audio
       FX deletion's business. Clearing those would silence modulation the user
       never touched. */
    seed_chain(3, 2);
    set_lfo(0, "synth", "cutoff");
    set_lfo(1, "midi_fx2", "m2");
    chain_reorder_remove(inst, 0, 1);
    EXPECT_STR(inst->lfos[0].target, "synth", "an audio FX deletion cleared a SYNTH routing");
    EXPECT_STR(inst->lfos[1].target, "midi_fx2",
               "an audio FX deletion cleared a MIDI FX routing");

    /* And the same rule on the MIDI side. */
    seed_chain(0, 3);
    add_mod("midi_fx2", "m2", 0.5f, "lfo1", 0.1f);
    set_lfo(0, "midi_fx2", "m2");
    set_lfo(1, "midi_fx3", "m3");
    EXPECT_INT(chain_reorder_remove(inst, 1, 1), 1, "remove of midi_fx2 accepted");
    EXPECT_INT(unloaded_midi_fx_slot, 1, "the removed MIDI position was unloaded");
    EXPECT_STR(inst->lfos[0].target, "", "the LFO aimed at the deleted MIDI FX was not cleared");
    EXPECT_STR(inst->lfos[0].param, "", "the deleted MIDI FX left its param name in the LFO");
    EXPECT_STR(inst->lfos[1].target, "midi_fx2", "the MIDI LFO behind the deletion did not follow");
    if (mod_by_param("m2")) failf("the deleted MIDI FX left an active modulation entry");
    check_no_dangling("after a MIDI FX remove");
}

/* ======================================================================== */
/* 3. A KNOB MAPPED TO FX 4+ FOLLOWS THE EDIT                               */
/* ======================================================================== */
/*
 * knob_mappings is the THIRD table that names a position by string, and it was
 * found by reading rather than by grepping -- the other two are called
 * "target" in a modulation context and this one is not. Everything about fx4+
 * has a separate history of failing silently: the metadata ladders stopped at
 * fx3 and midi_fx2, and the patch layer at fx4, each time leaving the higher
 * positions working-but-wrong rather than broken.
 *
 * So this fills the whole table (eight, the cap) and checks EVERY entry, not
 * the one being dragged: an off-by-one bound of 4 leaves entries 5..8 naming
 * whatever slid into their old index, and the knob quietly drives the wrong
 * module.
 */
static void test_knob_mapping_follows(void) {
    seed_chain(8, 0);
    for (int i = 0; i < MAX_KNOB_MAPPINGS; i++) {
        char target[16], param[8];
        snprintf(target, sizeof(target), "fx%d", i + 1);
        snprintf(param, sizeof(param), "p%d", i + 1);
        add_knob(71 + i, target, param, 0.1f * (float)(i + 1));
    }
    EXPECT_INT(inst->knob_mapping_count, 8, "the fixture filled the knob table");

    /* Drag fx6 -- past every bound that has ever been hardcoded -- to fx2. */
    EXPECT_INT(chain_reorder_move(inst, 0, 5, 1), 1, "move fx6 to fx2 accepted");

    /* The whole permutation, entry by entry, keyed on the CC (which no shape
       edit may rewrite). Rotating 6 into 2 pushes 2..5 along by one. */
    static const char *want[MAX_KNOB_MAPPINGS] = {
        "fx1", "fx3", "fx4", "fx5", "fx6", "fx2", "fx7", "fx8"
    };
    for (int i = 0; i < MAX_KNOB_MAPPINGS; i++) {
        knob_mapping_t *k = knob_by_cc(71 + i);
        char what[96];
        if (!k) { failf("the knob mapping on CC %d disappeared", 71 + i); continue; }
        snprintf(what, sizeof(what), "CC %d (was fx%d) after moving fx6 to fx2", 71 + i, i + 1);
        EXPECT_STR(k->dests[0].target, want[i], what);
        snprintf(what, sizeof(what), "CC %d kept its parameter", 71 + i);
        {
            char p[8];
            snprintf(p, sizeof(p), "p%d", i + 1);
            EXPECT_STR(k->dests[0].param, p, what);
        }
        snprintf(what, sizeof(what), "CC %d kept the value the knob was left at", 71 + i);
        EXPECT_EXACT(k->dests[0].current_value, 0.1f * (float)(i + 1), what);
    }
    EXPECT_INT(inst->knob_mapping_count, 8, "a move changed how many knobs are mapped");
    check_no_dangling("after a move with a full knob table");

    /* DELETING the mapped module clears BOTH halves. A target left naming a
       position that another module now occupies is a knob silently driving the
       wrong plugin; a param left behind revives on the next target write. */
    seed_chain(8, 0);
    add_knob(71, "fx6", "p6", 0.6f);
    add_knob(72, "fx8", "p8", 0.8f);
    EXPECT_INT(chain_reorder_remove(inst, 0, 5), 1, "remove of fx6 accepted");
    {
        knob_mapping_t *k = knob_by_cc(71);
        if (!k) failf("the knob mapping row vanished on a removal");
        else {
            EXPECT_STR(k->dests[0].target, "", "the knob still names the position of a deleted module");
            EXPECT_STR(k->dests[0].param, "", "the knob kept the deleted module parameter name");
        }
        k = knob_by_cc(72);
        if (!k) failf("the second knob mapping vanished");
        else {
            EXPECT_STR(k->dests[0].target, "fx7", "the knob behind the deletion did not follow down");
            EXPECT_STR(k->dests[0].param, "p8", "the following knob lost its parameter");
            EXPECT_EXACT(k->dests[0].current_value, 0.8f, "the following knob lost its value");
        }
    }
    check_no_dangling("after removing a mapped fx6");

    /* An audio FX edit is not a MIDI FX edit. The two prefixes share the
       suffix "fx", so a prefix test that is not anchored rewrites both. */
    seed_chain(8, 8);
    add_knob(71, "midi_fx6", "m6", 0.66f);
    add_knob(72, "synth", "cutoff", 0.5f);
    chain_reorder_move(inst, 0, 5, 1);
    EXPECT_STR(knob_by_cc(71)->dests[0].target, "midi_fx6",
               "an AUDIO FX move rewrote a MIDI FX knob mapping");
    EXPECT_STR(knob_by_cc(72)->dests[0].target, "synth", "an audio FX move rewrote a synth knob mapping");

    /* ...and the MIDI side moves its own. */
    chain_reorder_move(inst, 1, 5, 1);
    EXPECT_STR(knob_by_cc(71)->dests[0].target, "midi_fx2", "a MIDI FX move did not carry its knob mapping");
    EXPECT_EXACT(knob_by_cc(71)->dests[0].current_value, 0.66f, "the MIDI FX knob lost its value");
}

/* ======================================================================== */
/* 4. A REFUSED EDIT CHANGES NOTHING                                        */
/* ======================================================================== */
/*
 * The three verbs refuse what would corrupt (out of range, full, a move onto
 * itself). A refusal that had already retargeted half the tables would leave
 * the routings describing a chain shape that never happened -- worse than the
 * edit, because there is no undo and nothing on screen changed.
 */
static void test_refusal_is_inert(void) {
    seed_chain(2, 0);
    add_mod("fx2", "p2", 0.375f, "lfo1", 0.1f);
    set_lfo(0, "fx2", "p2");
    add_knob(71, "fx2", "p2", 0.42f);

    EXPECT_INT(chain_reorder_move(inst, 0, 1, 1), 0, "a move onto itself was accepted");
    EXPECT_INT(chain_reorder_move(inst, 0, 0, 5), 0, "a move past the end was accepted");
    EXPECT_INT(chain_reorder_remove(inst, 0, 4), 0, "a remove past the end was accepted");
    EXPECT_INT(chain_reorder_insert(inst, 0, -1), 0, "an insert at a negative position was accepted");

    EXPECT_STR(inst->lfos[0].target, "fx2", "a refused edit moved an LFO routing");
    EXPECT_STR(knob_by_cc(71)->dests[0].target, "fx2", "a refused edit moved a knob mapping");
    {
        mod_target_state_t *e = mod_by_param("p2");
        if (!e) failf("a refused edit destroyed a modulation entry");
        else {
            EXPECT_STR(e->target, "fx2", "a refused edit moved a modulation target");
            EXPECT_EXACT(e->base_value, 0.375f, "a refused edit changed a modulation base");
        }
    }
    EXPECT_INT(inst->fx_count, 2, "a refused edit changed the chain length");
}

int main(void) {
    inst = calloc(1, sizeof(*inst));
    if (!inst || !chain_alloc_position_storage(inst)) {
        fprintf(stderr, "FAIL: out of memory\n");
        return 1;
    }

    test_move_preserves_base();
    test_remove_clears_lfo();
    test_knob_mapping_follows();
    test_refusal_is_inert();

    chain_free_position_storage(inst);
    free(inst);

    if (failures) {
        fprintf(stderr, "FAIL: %d chain reorder routing check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: a chain shape edit carries all three position-keyed tables -- a "
           "modulated parameter keeps its BASE across a move, a deleted module's "
           "LFO and knob mapping are cleared in both halves while the routings "
           "behind it only renumber, and a refused edit changes nothing\n");
    return 0;
}
