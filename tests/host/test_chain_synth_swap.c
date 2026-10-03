/*
 * A slot's synth swap, taken apart: stage / swap_step / retire
 * (chain_synth_load.c), run for real against a fixture synth the wrapper
 * script builds — the dlopen is NOT stubbed.
 *
 * What the async path promises, and what each check below is for:
 *
 *   - STAGE TOUCHES NOTHING LIVE. It runs on the loader while the callback
 *     renders this very instance, so after a stage the OLD synth must still
 *     be the one installed, untouched. A stage that installed early would put
 *     a half-built module in front of the render path.
 *   - THE SWAP WAITS FOR THE FADE. swap_step returns 0 while the old synth is
 *     still audible and the render is advancing the fade; it commits only once
 *     the gain is down — or at once for a slot the shim is not rendering (no
 *     render since the last step), or at the block bound as a backstop.
 *   - THE OLD SYNTH IS DESTROYED BY RETIRE, NOT BY THE COMMIT. The commit runs
 *     on the callback; destroy_instance can join threads and reap processes.
 *   - "none", and a failed load, CLEAR the synth — the semantics the
 *     synchronous path always had (it unloaded before it loaded).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain_internal.h"

/* ---- the async entry points (exported from chain_fx_load.c) ------------ */
int chain_load_stage(void *instance, const char *key, const char *value);
int chain_load_swap_step(void *instance, int force);
void chain_load_retire(void);
#define chain_synth_stage(i, m)       chain_load_stage((i), "synth:module", (m))
#define chain_synth_swap_step(i, f)   chain_load_swap_step((i), (f))
#define chain_synth_retire()          chain_load_retire()

/* ---- the fixture's own counters, read through dlsym --------------------- */
#include <dlfcn.h>

/* ------------------------------------------------------------------ stubs */
CHAIN_INTERNAL void v2_chain_log(chain_instance_t *inst, const char *msg) { (void)inst; (void)msg; }
CHAIN_INTERNAL int parse_ui_hierarchy_cache(const char *p, char *out, int n) { (void)p; if (out && n > 0) out[0] = '\0'; return 0; }
CHAIN_INTERNAL int valid_module_name(const char *name) { return name && name[0] && !strchr(name, '/'); }
CHAIN_INTERNAL int parse_chain_params(const char *module_path, chain_param_info_t *params, int *count) {
    (void)module_path;
    snprintf(params[0].key, sizeof(params[0].key), "%s", "fixture_param");
    if (count) *count = 1;
    return 0;
}
CHAIN_INTERNAL void chain_child_keys_load(chain_child_keys_t *ck, const char *module_path) {
    (void)module_path; if (ck) { ck->ntmpl = 0; ck->nalias = 0; ck->next_evict = 0; }
}
CHAIN_INTERNAL int json_get_int_in_section(const char *j, const char *s, const char *k, int *o) { (void)j; (void)s; (void)k; (void)o; return -1; }
CHAIN_INTERNAL int json_get_bool_in_section(const char *j, const char *s, const char *k, int *o) { (void)j; (void)s; (void)k; (void)o; return -1; }
CHAIN_INTERNAL int json_get_flag_in_section(const char *j, const char *s, const char *k) { (void)j; (void)s; (void)k; return 0; }
CHAIN_INTERNAL int json_get_string(const char *j, const char *k, char *o, int n) { (void)j; (void)k; (void)o; (void)n; return -1; }
CHAIN_INTERNAL int json_get_string_in_section(const char *j, const char *s, const char *k, char *o, int n) { (void)j; (void)s; (void)k; (void)o; (void)n; return -1; }
CHAIN_INTERNAL void chain_mod_clear_target_entries(chain_instance_t *inst, const char *t, int r) { (void)inst; (void)t; (void)r; }
CHAIN_INTERNAL void chain_voice_sends_load(chain_instance_t *inst) { (void)inst; }
CHAIN_INTERNAL void chain_bus_rebuild_voice_map(chain_instance_t *inst) { (void)inst; }
CHAIN_INTERNAL void smoother_reset(param_smoother_t *s) { (void)s; }
static int g_panics;
CHAIN_INTERNAL void v2_synth_panic(chain_instance_t *inst) { (void)inst; g_panics++; }

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static int *fixture_counter(const char *module, const char *sym) {
    char path[1024];
    if (strncmp(module, "fx", 2) == 0)
        snprintf(path, sizeof(path), "%s/audio_fx/%s/%s.so", FIXTURE_DIR, module, module);
    else
        snprintf(path, sizeof(path), "%s/sound_generators/%s/dsp.so", FIXTURE_DIR, module);
    /* Our OWN reference, never released: the chain dlcloses the module at
     * every retire, and a counter in an unmapped library is a segfault. With
     * this held, the mapping (and its counters) persists across reloads. */
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) return NULL;
    return (int *)dlsym(h, sym);
}

/* What v2_render_block does to the gain each block, without the audio. */
static void render_block(chain_instance_t *inst) {
    if (!inst->synth_swap_fading) return;
    inst->synth_swap_gain -= (float)FRAMES_PER_BLOCK / (float)SYNTH_SWAP_FADE_SAMPLES;
    if (inst->synth_swap_gain < 0.0f) inst->synth_swap_gain = 0.0f;
    inst->synth_swap_rendered = 1;
}

int main(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    if (!inst || !chain_alloc_position_storage(inst)) { printf("FAIL: alloc\n"); return 1; }
    snprintf(inst->module_dir, sizeof(inst->module_dir), "%s/chain", FIXTURE_DIR);

    /* ---- synchronous load, the boot / fallback path ---------------------- */
    chain_synth_set_module(inst, "syna");
    CHECK(inst->synth_instance != NULL, "sync load installs an instance");
    CHECK(strcmp(inst->current_synth_module, "syna") == 0, "sync load names syna");
    void *a_inst = inst->synth_instance;
    int *a_destroyed = fixture_counter("syna", "fixture_destroyed");
    CHECK(a_destroyed && *a_destroyed == 0, "syna not destroyed yet");

    /* ---- STAGE leaves the live synth alone ------------------------------- */
    chain_synth_stage(inst, "synb");
    CHECK(inst->synth_instance == a_inst, "a stage must NOT install: the callback is rendering syna");
    CHECK(strcmp(inst->current_synth_module, "syna") == 0, "a stage must not rename the live synth");
    int *b_created = fixture_counter("synb", "fixture_created");
    int *b_destroyed = fixture_counter("synb", "fixture_destroyed");   /* while mapped */
    CHECK(b_created && *b_created == 1, "the stage built synb (create_instance ran)");

    /* ---- the swap waits for the fade ------------------------------------- */
    g_panics = 0;
    CHECK(chain_synth_swap_step(inst, 0) == 0, "first step only starts the fade");
    CHECK(inst->synth_swap_fading && inst->synth_swap_gain == 1.0f, "fade starts at full gain");
    int steps = 1;
    while (inst->synth_swap_gain > 0.0f && steps < 100) {
        render_block(inst);
        if (inst->synth_swap_gain > 0.0f)
            CHECK(chain_synth_swap_step(inst, 0) == 0, "no commit while the old synth is audible");
        steps++;
    }
    CHECK(inst->synth_instance == a_inst, "syna is still installed right up to the commit");
    CHECK(chain_synth_swap_step(inst, 0) == 1, "commits once the fade is down");
    CHECK(strcmp(inst->current_synth_module, "synb") == 0, "committed: synb installed");
    CHECK(inst->synth_instance != a_inst && inst->synth_instance, "committed: a new instance");
    CHECK(!inst->synth_swap_fading && inst->synth_swap_gain == 1.0f, "commit resets the fade");
    CHECK(g_panics == 1, "the old synth gets its all-notes-off at the commit");

    /* ---- retire, not commit, destroys ------------------------------------ */
    CHECK(*a_destroyed == 0, "the COMMIT must not destroy syna: it runs on the callback");
    CHECK(chain_synth_swap_step(inst, 1) == 1 &&
          strcmp(inst->current_synth_module, "synb") == 0 && *b_created == 1,
          "a step with nothing staged does nothing (never re-installs the stage)");
    chain_synth_retire();
    CHECK(*a_destroyed == 1, "retire destroys syna");
    chain_synth_retire();
    CHECK(*a_destroyed == 1, "a second retire is a no-op");

    /* ---- a slot the shim is not rendering commits at once ---------------- */
    chain_synth_stage(inst, "syna");
    CHECK(chain_synth_swap_step(inst, 0) == 0, "idle: first step starts the fade");
    /* no render_block: the idle gate skipped it */
    CHECK(chain_synth_swap_step(inst, 0) == 1, "idle: no render since the last step -> commit now");
    CHECK(strcmp(inst->current_synth_module, "syna") == 0, "idle: syna installed");
    chain_synth_retire();
    CHECK(b_destroyed && *b_destroyed == 1, "idle: synb retired");

    /* ---- the block bound is a backstop ----------------------------------- */
    chain_synth_stage(inst, "synb");
    int n = 0, done = 0;
    while (!done && n < 200) {
        done = chain_synth_swap_step(inst, 0);
        /* render that never brings the gain down (a stuck fade) */
        inst->synth_swap_rendered = 1;
        n++;
    }
    CHECK(done && n == SYNTH_SWAP_FADE_MAX_BLOCKS + 1, "a stuck fade commits at the block bound");
    chain_synth_retire();

    /* ---- "none" clears, and so does a load that fails --------------------- */
    chain_synth_stage(inst, "none");
    CHECK(chain_synth_swap_step(inst, 1) == 1, "forced step commits");
    CHECK(inst->synth_instance == NULL && inst->current_synth_module[0] == '\0', "none clears the synth");
    chain_synth_retire();
    CHECK(*b_destroyed == 2, "the cleared synth is retired");

    chain_synth_set_module(inst, "syna");
    chain_synth_stage(inst, "does-not-exist");
    CHECK(inst->synth_instance != NULL, "a failed stage leaves the live synth playing");
    chain_synth_swap_step(inst, 1);
    CHECK(inst->synth_instance == NULL, "a failed load clears at the commit, as the sync path always did");
    chain_synth_retire();

    /* ---- nothing loaded: a swap commits straight away -------------------- */
    chain_synth_stage(inst, "synb");
    CHECK(chain_synth_swap_step(inst, 0) == 1, "no old synth to fade: commit at once");
    CHECK(strcmp(inst->current_synth_module, "synb") == 0, "and synb is installed");
    chain_synth_retire();


    /* ==================== ONE AUDIO FX POSITION, swapped alone ============ */
    /* fx1 = fxa (writes 1000), fx2 = fxc (passes through, counts its calls).
     * Dry input 100. Swapping fx1 to fxb (writes 2000) must crossfade ONLY
     * position 1 — wet -> dry -> new wet — while fx2 runs every block. */
    CHECK(v2_load_audio_fx_slot(inst, 0, "fxa") == 0, "sync load fx1 = fxa");
    CHECK(v2_load_audio_fx_slot(inst, 1, "fxc") == 0, "sync load fx2 = fxc");
    int *fxa_destroyed = fixture_counter("fxa", "fixture_destroyed");
    int *fxc_processed = fixture_counter("fxc", "fixture_processed");
    void *fxa_inst = inst->fx_instances[0], *fxc_inst = inst->fx_instances[1];

    int16_t buf[FRAMES_PER_BLOCK * 2];
    #define RUN_BLOCK() do { \
        for (int k_ = 0; k_ < FRAMES_PER_BLOCK * 2; k_++) buf[k_] = 100; \
        for (int i_ = 0; i_ < inst->fx_count; i_++) \
            chain_fx_run_position(inst, i_, buf, FRAMES_PER_BLOCK); \
    } while (0)

    RUN_BLOCK();
    CHECK(buf[0] == 1000 && buf[FRAMES_PER_BLOCK * 2 - 1] == 1000, "steady: fx1 wet");

    CHECK(chain_load_stage(inst, "fx1:module", "fxb") == 1, "fx1:module is stageable");
    CHECK(inst->fx_instances[0] == fxa_inst, "an FX stage must NOT install: fxa is still live");
    int *fxb_created = fixture_counter("fxb", "fixture_created");
    CHECK(fxb_created && *fxb_created == 1, "the stage built fxb");

    int processed_before = *fxc_processed;
    int blocks = 0, last_first = 1000, last_tail = 1000, monotonic_out = 1;
    CHECK(chain_load_swap_step(inst, 0) == 0, "fx: first step only starts the fade-out");
    while (blocks < 100) {
        RUN_BLOCK();
        blocks++;
        if (buf[0] > last_first) monotonic_out = 0;
        last_first = buf[0];
        last_tail = buf[FRAMES_PER_BLOCK * 2 - 1];
        if (chain_load_swap_step(inst, 0)) break;
    }
    CHECK(monotonic_out, "fx1 fades monotonically from wet toward dry");
    CHECK(last_tail == 100, "fx1 is fully dry by the end of the block before the commit");
    CHECK(blocks >= FX_SWAP_FADE_SAMPLES / FRAMES_PER_BLOCK, "the fade-out took its full length");
    CHECK(inst->fx_instances[0] != fxa_inst && strcmp(inst->current_fx_modules[0], "fxb") == 0,
          "committed: fxb in fx1");
    CHECK(inst->fx_instances[1] == fxc_inst, "fx2 was never touched");
    CHECK(*fxa_destroyed == 0, "the FX commit must not destroy fxa: it runs on the callback");
    chain_load_retire();
    CHECK(*fxa_destroyed == 1, "retire destroys fxa");

    /* fade in: from dry toward 2000, then exactly 2000 */
    RUN_BLOCK();
    CHECK(buf[0] >= 100 && buf[0] < 300, "the new FX fades IN from dry");
    for (int n2 = 0; n2 < 50 && inst->fx_swap_phase; n2++) RUN_BLOCK();
    RUN_BLOCK();
    CHECK(inst->fx_swap_phase == 0 && buf[0] == 2000 && buf[FRAMES_PER_BLOCK * 2 - 1] == 2000,
          "fade-in completes at full wet");
    CHECK(*fxc_processed - processed_before >= blocks, "fx2 processed every single block of the swap");

    /* "none" fades fx1 out and leaves an interior hole: fx2 still there */
    CHECK(chain_load_stage(inst, "fx1:module", "none") == 1, "stage none");
    while (!chain_load_swap_step(inst, 0)) RUN_BLOCK();
    chain_load_retire();
    CHECK(inst->fx_instances[0] == NULL && inst->fx_count == 2, "none clears fx1, fx_count keeps fx2");
    RUN_BLOCK();
    CHECK(buf[0] == 100, "fx1 empty and fx2 pass-through: dry");

    /* a bypassed position has nothing audible to fade: commit at once */
    CHECK(v2_load_audio_fx_slot(inst, 0, "fxa") == 0, "reload fxa");
    inst->fx_bypassed[0] = 1;
    chain_load_stage(inst, "fx1:module", "fxb");
    CHECK(chain_load_swap_step(inst, 0) == 1, "bypassed: commit at once");
    chain_load_retire();
    CHECK(inst->fx_bypassed[0] == 0 && inst->fx_swap_phase == 2, "new module unbypassed, fading in");

    /* a refused name changes nothing */
    void *keep = inst->fx_instances[0];
    chain_load_stage(inst, "fx1:module", "../evil");
    chain_load_swap_step(inst, 1);
    chain_load_retire();
    CHECK(inst->fx_instances[0] == keep, "an invalid FX name leaves the position alone");

    chain_free_position_storage(inst);
    free(inst);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_chain_synth_swap: all passed\n");
    return 0;
}
