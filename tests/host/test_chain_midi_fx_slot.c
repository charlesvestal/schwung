/*
 * Does "midi_fx<N>:module" land in slot N?
 *
 * tests/host/test_chain_key_index.c proves the PARSER returns the right index.
 * It cannot prove the index survives the call — and for a long time it did not:
 * v2_load_midi_fx() took only a name and always appended at midi_fx_count, so
 * chain_host.c parsed "midi_fx4:" to 3 and then threw the 3 away. On an empty
 * chain, "midi_fx4:module" and "midi_fx1:module" were byte-identical
 * operations. Nothing failed to compile, nothing failed a source pin, and the
 * only symptom would have been a MIDI FX reorder that silently does nothing.
 *
 * So this test runs the real loader from chain_midi.c against a real (tiny)
 * MIDI FX plugin built by the wrapper script, and asks which slot it landed
 * in. Everything chain_midi.c needs that lives in another translation unit is
 * stubbed below; the dlopen path is NOT stubbed, because "did the plugin
 * actually get placed" is the whole question.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain_internal.h"

/* ------------------------------------------------------------------ stubs */
/*
 * chain_midi.c pulls these in from chain_params.c / chain_json.c /
 * chain_mod.c / chain_host.c. None of them decide slot placement, so they are
 * stubbed to the cheapest thing that lets the loader run to completion.
 * parse_chain_params returning < 0 would make the loader roll back, so it must
 * succeed here — a rollback would look like "the slot is empty", which is the
 * exact failure this test is trying to distinguish.
 */
CHAIN_INTERNAL void v2_chain_log(chain_instance_t *inst, const char *msg) {
    (void)inst; (void)msg;
}
CHAIN_INTERNAL int parse_chain_params(const char *module_path,
                                      chain_param_info_t *params, int *count) {
    (void)module_path; (void)params;
    if (count) *count = 0;
    return 0;
}
CHAIN_INTERNAL int parse_ui_hierarchy_cache(const char *module_path, char *out, int out_len) {
    (void)module_path;
    if (out && out_len > 0) out[0] = '\0';
    return 0;
}
CHAIN_INTERNAL int json_get_int_in_section(const char *json, const char *section_key,
                                           const char *key, int *out) {
    (void)json; (void)section_key; (void)key; (void)out;
    return -1;
}
CHAIN_INTERNAL void chain_mod_clear_target_entries(chain_instance_t *inst,
                                                   const char *target, int restore_base) {
    (void)inst; (void)target; (void)restore_base;
}
CHAIN_INTERNAL chain_param_info_t *knob_find_param(chain_instance_t *inst,
                                                   const char *target, const char *param) {
    (void)inst; (void)target; (void)param;
    return NULL;
}
CHAIN_INTERNAL void knob_forward_value(chain_instance_t *inst, const char *target,
                                       const char *param, const char *val_str) {
    (void)inst; (void)target; (void)param; (void)val_str;
}

/* ------------------------------------------------------------------ harness */

#ifndef FIXTURE_DIR
#error "FIXTURE_DIR must be defined by the test wrapper"
#endif
static int failures = 0;

static void fail(const char *what) {
    fprintf(stderr, "FAIL: %s\n", what);
    failures++;
}

static void expect_int(const char *what, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

static void expect_slot_holds(chain_instance_t *inst, int slot, const char *module) {
    char what[96];
    snprintf(what, sizeof(what), "slot %d holds \"%s\"", slot, module);
    if (!inst->midi_fx_handles[slot] || !inst->midi_fx_plugins[slot] ||
        !inst->midi_fx_instances[slot]) {
        fprintf(stderr, "FAIL: %s: slot is empty\n", what);
        failures++;
        return;
    }
    if (strcmp(inst->current_midi_fx_modules[slot], module) != 0) {
        fprintf(stderr, "FAIL: %s: slot holds \"%s\"\n", what,
                inst->current_midi_fx_modules[slot]);
        failures++;
    }
}

static void expect_slot_empty(chain_instance_t *inst, int slot) {
    if (inst->midi_fx_handles[slot] || inst->midi_fx_plugins[slot] ||
        inst->midi_fx_instances[slot] || inst->current_midi_fx_modules[slot][0]) {
        fprintf(stderr, "FAIL: slot %d should be empty, holds \"%s\"\n", slot,
                inst->current_midi_fx_modules[slot]);
        failures++;
    }
}

/* The instance is ~1 MB of cached ui_hierarchy strings — heap, not stack. */
static chain_instance_t *fresh_instance(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    /* The per-position metadata blocks are pointers now (so a reorder can move
       a module without a multi-megabyte memmove in the audio callback), so a
       calloc'd instance is not usable until they exist. */
    if (!inst || !chain_alloc_position_storage(inst)) {
        fprintf(stderr, "FAIL: out of memory\n");
        exit(1);
    }
    /* The loader builds "<module_dir>/../midi_fx/<name>/dsp.so", so module_dir
     * is a sibling of the fixture midi_fx directory. The wrapper creates it:
     * ".." is resolved by the kernel, so the component before it has to be a
     * real directory even though nothing is ever read out of it. */
    snprintf(inst->module_dir, sizeof(inst->module_dir), "%s/chain", FIXTURE_DIR);
    return inst;
}

static void release(chain_instance_t *inst) {
    v2_unload_all_midi_fx(inst);
    chain_free_position_storage(inst);
    free(inst);
}

int main(void) {
    /*
     * THE BUG. On an empty chain, ask for slot 4 (index 3) by name.
     *
     * Before v2_load_midi_fx_slot existed this produced a plugin in slot 0 and
     * a count of 1 — indistinguishable from asking for slot 1.
     */
    {
        chain_instance_t *inst = fresh_instance();
        expect_int("load into slot 3 on an empty chain", v2_load_midi_fx_slot(inst, 3, "mfxa"), 0);
        expect_slot_empty(inst, 0);
        expect_slot_empty(inst, 1);
        expect_slot_empty(inst, 2);
        expect_slot_holds(inst, 3, "mfxa");
        /* Count is a HIGH-WATER MARK, matching the audio FX side: holes are
         * legal and both MIDI FX loops NULL-guard per slot. */
        expect_int("midi_fx_count after a hole load", inst->midi_fx_count, 4);
        release(inst);
    }

    /* Slot 1 is unchanged from the field behaviour: one FX, in slot 0. */
    {
        chain_instance_t *inst = fresh_instance();
        expect_int("load into slot 0", v2_load_midi_fx_slot(inst, 0, "mfxa"), 0);
        expect_slot_holds(inst, 0, "mfxa");
        expect_int("midi_fx_count after slot 0 load", inst->midi_fx_count, 1);
        release(inst);
    }

    /*
     * Slot 1 no longer owns the whole list. Writing midi_fx1:module over a
     * two-long chain replaces slot 0 and LEAVES slot 1 alone; it used to
     * unload everything first. At a cap of 8 the old rule destroyed up to
     * seven neighbours, and made a chain rewrite depend on write order.
     */
    {
        chain_instance_t *inst = fresh_instance();
        expect_int("append 1", v2_load_midi_fx(inst, "mfxa"), 0);
        expect_int("append 2", v2_load_midi_fx(inst, "mfxb"), 0);
        expect_slot_holds(inst, 0, "mfxa");
        expect_slot_holds(inst, 1, "mfxb");
        expect_int("midi_fx_count after two appends", inst->midi_fx_count, 2);

        expect_int("replace slot 0", v2_load_midi_fx_slot(inst, 0, "mfxb"), 0);
        expect_slot_holds(inst, 0, "mfxb");
        expect_slot_holds(inst, 1, "mfxb");
        expect_int("midi_fx_count after replacing slot 0", inst->midi_fx_count, 2);
        release(inst);
    }

    /* "none" and "" clear the addressed slot, and trailing empties shrink the
     * high-water mark — same rule as v2_load_audio_fx_slot. */
    {
        chain_instance_t *inst = fresh_instance();
        v2_load_midi_fx(inst, "mfxa");
        v2_load_midi_fx(inst, "mfxb");
        expect_int("clear slot 1", v2_load_midi_fx_slot(inst, 1, "none"), 0);
        expect_slot_holds(inst, 0, "mfxa");
        expect_slot_empty(inst, 1);
        expect_int("midi_fx_count after clearing the tail", inst->midi_fx_count, 1);

        /* Clearing an interior slot leaves the mark where it was. */
        v2_load_midi_fx_slot(inst, 2, "mfxb");
        expect_int("midi_fx_count", inst->midi_fx_count, 3);
        expect_int("clear slot 0", v2_load_midi_fx_slot(inst, 0, ""), 0);
        expect_slot_empty(inst, 0);
        expect_slot_holds(inst, 2, "mfxb");
        expect_int("midi_fx_count after clearing an interior slot", inst->midi_fx_count, 3);
        release(inst);
    }

    /* Out of range is refused, not clamped — a clamp would put a hand-edited
     * "midi_fx99:module" on top of a real FX. */
    {
        chain_instance_t *inst = fresh_instance();
        expect_int("slot -1 refused", v2_load_midi_fx_slot(inst, -1, "mfxa"), -1);
        expect_int("slot MAX refused", v2_load_midi_fx_slot(inst, MAX_MIDI_FX, "mfxa"), -1);
        expect_int("nothing loaded", inst->midi_fx_count, 0);
        release(inst);
    }

    /* The append wrapper still refuses once the list is full, and the refusal
     * does not disturb what is already loaded. */
    {
        chain_instance_t *inst = fresh_instance();
        for (int i = 0; i < MAX_MIDI_FX; i++) {
            if (v2_load_midi_fx(inst, "mfxa") != 0) fail("append below the cap failed");
        }
        expect_int("full", inst->midi_fx_count, MAX_MIDI_FX);
        expect_int("append past the cap refused", v2_load_midi_fx(inst, "mfxb"), -1);
        expect_int("still full", inst->midi_fx_count, MAX_MIDI_FX);
        expect_slot_holds(inst, MAX_MIDI_FX - 1, "mfxa");
        release(inst);
    }

    /* A failed load clears the addressed slot rather than leaving it
     * half-loaded: the slot is unloaded before the dlopen is attempted, which
     * is what v2_load_audio_fx_slot does too. */
    {
        chain_instance_t *inst = fresh_instance();
        v2_load_midi_fx_slot(inst, 0, "mfxa");
        expect_int("unknown module refused", v2_load_midi_fx_slot(inst, 0, "no_such_fx"), -1);
        expect_slot_empty(inst, 0);
        release(inst);
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d midi_fx slot check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: midi_fx<N>:module loads into slot N\n");
    return 0;
}
