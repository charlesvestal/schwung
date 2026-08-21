/*
 * Master FX shape edits, driven against the REAL shim code.
 *
 * insert / remove / move are an ARRAY PERMUTATION: the FX instances keep
 * running and only their index changes. Expressed as a run of `fxN:module`
 * writes instead, every position behind the edit would be unloaded and
 * dlopen'd afresh -- a reverb loses the tail that was ringing.
 *
 * Master FX has zero behavioural coverage before this file; all nine of its
 * other tests are source greps. That matters here more than usual, because the
 * slot chain's permutation shipped with 28/28 mutations killed and STILL
 * segfaulted on hardware: the fixture used value arrays where production used
 * owned pointers, so the one bug that mattered was structurally out of reach.
 * So this drives the real loader, the real dlopen, the real owned buffers.
 *
 * Three groups of assertion, deliberately kept apart:
 *
 *   INVARIANTS   things that must hold after EVERY operation, checked without
 *                reference to what the operation was meant to do: no NULL
 *                owned pointer, the multiset of owned buffers unchanged, no
 *                dlopen handle lost, no two positions aliasing a buffer, and
 *                fx_count never below a loaded module. A test that only
 *                asserted the intended ordering would pass while leaking a
 *                handle or stranding a buffer.
 *
 *   ORDERING     that the modules actually landed where the verb said, and
 *                that the LOCKSTEP arrays came with them -- miss one and
 *                position 3 serves position 5's param metadata.
 *
 *   ROUTING      the one table that names a position by string (the two Master
 *                FX LFOs' target), including the base snapshot, which is
 *                indexed BY LFO rather than by position and so has to be
 *                invalidated by hand on a remove.
 *
 * The unit under test is INCLUDED rather than linked: three of the
 * per-position arrays are file-static, so nothing else can see them, and no
 * test-only accessor is added to production code. The wrapper supplies stubs
 * for the externs this pulls in, and one fixture module per position so a
 * module's identity is observable.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "shadow_chain_mgmt.c"

#ifndef FIXTURE_DIR
#error "FIXTURE_DIR must be defined by the test wrapper"
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL: "); \
                   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } \
} while (0)

/* ---------------------------------------------------------------- fixtures */

/* One module directory per position, so module_id makes a module's identity
 * legible in a failure message rather than just a pointer. */
static void fixture_path(int i, char *buf, size_t n) {
    snprintf(buf, n, "%s/mfx%d/dsp.so", FIXTURE_DIR, i);
}

static const char *expected_id(int i) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "mfx%d", i);
    return buf;
}

/* --------------------------------------------------------------- snapshots */

typedef struct {
    void *handle;
    void *instance;
    char module_id[64];
    int bypassed;
    char *cache;        /* owned: master_fx_slot_t.chain_params_cache */
    char *rt_cache;     /* owned: mfx_runtime_chain_params_cache[] */
    int rt_cached;      /* value, must travel WITH rt_cache */
    uint64_t rt_ms;     /* value, must travel WITH rt_cache */
} pos_t;

static void capture(pos_t *out) {
    /* Zeroed first so a whole-array memcmp (the "a refused edit changed
     * nothing" check) compares the FIELDS and not the struct padding, which is
     * whatever was on the stack. */
    memset(out, 0, MASTER_FX_SLOTS * sizeof(pos_t));
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        out[i].handle    = shadow_master_fx_slots[i].handle;
        out[i].instance  = shadow_master_fx_slots[i].instance;
        out[i].bypassed  = shadow_master_fx_slots[i].bypassed;
        out[i].cache     = shadow_master_fx_slots[i].chain_params_cache;
        out[i].rt_cache  = mfx_runtime_chain_params_cache[i];
        out[i].rt_cached = mfx_runtime_chain_params_cached[i];
        out[i].rt_ms     = mfx_runtime_chain_params_last_fetch_ms[i];
        snprintf(out[i].module_id, sizeof(out[i].module_id), "%s",
                 shadow_master_fx_slots[i].module_id);
    }
}

/* Distinct, checkable bytes in every per-position array that is NOT the struct
 * itself, so "did the lockstep arrays come along" is a real question rather
 * than a coincidence of everything being zero. */
static void stamp_lockstep(void) {
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        snprintf(mfx_runtime_chain_params_cache[i], MASTER_FX_CHAIN_PARAMS_MAX,
                 "runtime-%d", i);
        mfx_runtime_chain_params_cached[i] = 100 + i;
        mfx_runtime_chain_params_last_fetch_ms[i] = 1000 + (uint64_t)i;
        snprintf(shadow_master_fx_slots[i].chain_params_cache,
                 MASTER_FX_CHAIN_PARAMS_MAX, "slot-%d", i);
        shadow_master_fx_slots[i].chain_params_cached = 1;
    }
}

/* ------------------------------------------------------------- INVARIANTS */

static int ptr_cmp(const void *a, const void *b) {
    void *const *pa = (void *const *)a, *const *pb = (void *const *)b;
    return (*pa > *pb) - (*pa < *pb);
}

static void sorted_owned(const pos_t *p, void **slot_out, void **rt_out) {
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        slot_out[i] = p[i].cache;
        rt_out[i] = p[i].rt_cache;
    }
    qsort(slot_out, MASTER_FX_SLOTS, sizeof(void *), ptr_cmp);
    qsort(rt_out, MASTER_FX_SLOTS, sizeof(void *), ptr_cmp);
}

/*
 * Everything that must be true after ANY operation, said without reference to
 * what the operation was. `before` may be NULL for the very first call.
 */
static void invariants(const char *stage, const pos_t *before) {
    pos_t now[MASTER_FX_SLOTS];
    capture(now);

    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        /* THE one. A vacated owned position must receive the buffer displaced
         * off the end of the shift, contents cleared -- never a NULL pointer.
         * Nulling it is what took the SPI callback down on the slot chain:
         * the next module load parsed a param table straight through it. */
        CHECK(now[i].cache != NULL,
              "[%s] shadow_master_fx_slots[%d].chain_params_cache is NULL -- an owned "
              "buffer was nulled instead of rotated", stage, i);
        CHECK(now[i].rt_cache != NULL,
              "[%s] mfx_runtime_chain_params_cache[%d] is NULL -- an owned buffer was "
              "nulled instead of rotated", stage, i);
        for (int j = i + 1; j < MASTER_FX_SLOTS; j++) {
            CHECK(now[i].cache != now[j].cache,
                  "[%s] slot caches %d and %d alias", stage, i, j);
            CHECK(now[i].rt_cache != now[j].rt_cache,
                  "[%s] runtime caches %d and %d alias", stage, i, j);
        }
    }

    /* fx_count may never sit below a loaded position: a module that runs and
     * makes sound while the editor cannot see it is the fx3_module bug. */
    int count = shadow_master_fx_count();
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        if (now[i].instance) {
            CHECK(i < count,
                  "[%s] position %d is loaded but fx_count is %d", stage, i, count);
        }
    }
    CHECK(count >= 0 && count <= MASTER_FX_SLOTS,
          "[%s] fx_count %d is out of range", stage, count);

    if (!before) return;

    /* Nothing allocated, nothing freed, nothing swapped for a fresh block. */
    void *b_slot[MASTER_FX_SLOTS], *b_rt[MASTER_FX_SLOTS];
    void *a_slot[MASTER_FX_SLOTS], *a_rt[MASTER_FX_SLOTS];
    sorted_owned(before, b_slot, b_rt);
    sorted_owned(now, a_slot, a_rt);
    CHECK(memcmp(b_slot, a_slot, sizeof(b_slot)) == 0,
          "[%s] the multiset of slot chain_params buffers changed -- one was "
          "reallocated, stranded or leaked", stage);
    CHECK(memcmp(b_rt, a_rt, sizeof(b_rt)) == 0,
          "[%s] the multiset of runtime chain_params buffers changed", stage);
}

/*
 * Was the departed module actually UNLOADED, or merely forgotten?
 *
 * Forgetting is the failure mode: zeroing a position's struct leaks both the
 * dlopen handle and the FX instance, and does so in silence -- quieter than
 * the slot chain's crash, and therefore worse. Counting handle FIELDS cannot
 * see it, because a leak and a close look identical from the array's side. So
 * ask the dynamic linker instead: RTLD_NOLOAD returns a handle only if the
 * library is still resident.
 */
static void expect_not_resident(const char *stage, int fixture_index) {
    char path[512];
    fixture_path(fixture_index, path, sizeof(path));
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    checks++;
    if (h) {
        failures++;
        fprintf(stderr,
                "FAIL: [%s] mfx%d is still resident -- it was dropped from the array "
                "without being dlclose'd, leaking the handle and the FX instance\n",
                stage, fixture_index);
        dlclose(h);   /* undo the refcount this probe took */
    }
}

/* Every dlopen handle that was live before must still be live after, unless
 * this operation was supposed to close exactly one. This is the ARRAY's half
 * of the question -- that a permutation neither drops nor invents a handle.
 * expect_not_resident above is the linker's half. */
static void handles_preserved(const char *stage, const pos_t *before,
                              int expect_closed) {
    pos_t now[MASTER_FX_SLOTS];
    capture(now);
    void *b[MASTER_FX_SLOTS], *a[MASTER_FX_SLOTS];
    int nb = 0, na = 0;
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        if (before[i].handle) b[nb++] = before[i].handle;
        if (now[i].handle) a[na++] = now[i].handle;
    }
    CHECK(na == nb - expect_closed,
          "[%s] %d dlopen handle(s) live before, %d after, expected %d closed",
          stage, nb, na, expect_closed);
    qsort(b, (size_t)nb, sizeof(void *), ptr_cmp);
    qsort(a, (size_t)na, sizeof(void *), ptr_cmp);
    /* Every surviving handle must be one that existed before: a permutation
     * has no business minting one. */
    for (int i = 0; i < na; i++) {
        int found = 0;
        for (int j = 0; j < nb; j++) if (a[i] == b[j]) { found = 1; break; }
        CHECK(found, "[%s] a dlopen handle appeared from nowhere", stage);
    }
}

/* -------------------------------------------------------------- ORDERING */

/* Position `at` now holds what `src` held in `before` -- the whole per-position
 * record, not just the module. */
static void expect_moved(const char *stage, const pos_t *before, int src, int at) {
    CHECK(shadow_master_fx_slots[at].instance == before[src].instance,
          "[%s] position %d should hold the instance from %d", stage, at, src);
    CHECK(shadow_master_fx_slots[at].handle == before[src].handle,
          "[%s] position %d should hold the dlopen handle from %d", stage, at, src);
    CHECK(strcmp(shadow_master_fx_slots[at].module_id, before[src].module_id) == 0,
          "[%s] position %d holds \"%s\", expected \"%s\" (from %d)", stage, at,
          shadow_master_fx_slots[at].module_id, before[src].module_id, src);
    CHECK(shadow_master_fx_slots[at].chain_params_cache == before[src].cache,
          "[%s] position %d did not bring its own chain_params buffer from %d",
          stage, at, src);
    /* The three file-static arrays are the ones a struct-shaped derivation is
     * blind to, so they are asserted explicitly. */
    CHECK(mfx_runtime_chain_params_cache[at] == before[src].rt_cache,
          "[%s] position %d did not bring its runtime chain_params buffer from %d",
          stage, at, src);
    CHECK(mfx_runtime_chain_params_cached[at] == before[src].rt_cached,
          "[%s] mfx_runtime_chain_params_cached did not travel with position %d->%d",
          stage, src, at);
    CHECK(mfx_runtime_chain_params_last_fetch_ms[at] == before[src].rt_ms,
          "[%s] mfx_runtime_chain_params_last_fetch_ms did not travel with %d->%d",
          stage, src, at);
}

/* A position opened by an insert, or left behind by a remove: empty of module,
 * but holding a live buffer whose CONTENTS are cleared. */
static void expect_vacant(const char *stage, int at) {
    CHECK(shadow_master_fx_slots[at].instance == NULL,
          "[%s] position %d should be vacant but holds an instance", stage, at);
    CHECK(shadow_master_fx_slots[at].handle == NULL,
          "[%s] position %d should be vacant but holds a dlopen handle", stage, at);
    CHECK(shadow_master_fx_slots[at].module_id[0] == '\0',
          "[%s] position %d should be vacant but names \"%s\"", stage, at,
          shadow_master_fx_slots[at].module_id);
    CHECK(shadow_master_fx_slots[at].chain_params_cache != NULL &&
          shadow_master_fx_slots[at].chain_params_cache[0] == '\0',
          "[%s] vacant position %d must keep its buffer with the CONTENTS cleared",
          stage, at);
    CHECK(mfx_runtime_chain_params_cache[at] != NULL &&
          mfx_runtime_chain_params_cache[at][0] == '\0',
          "[%s] vacant position %d must keep its runtime buffer with the CONTENTS "
          "cleared", stage, at);
    CHECK(mfx_runtime_chain_params_cached[at] == 0,
          "[%s] vacant position %d still claims cached params", stage, at);
    CHECK(mfx_runtime_chain_params_last_fetch_ms[at] == 0,
          "[%s] vacant position %d still carries a fetch timestamp", stage, at);
}

/* --------------------------------------------------------------- scenarios */

/* Bring positions 0..n-1 up from scratch, and stamp the lockstep arrays. */
static void load_chain(int n) {
    shadow_chain_defaults();
    for (int i = 0; i < n; i++) {
        char path[512];
        fixture_path(i, path, sizeof(path));
        if (shadow_master_fx_slot_load(i, path) != 0) {
            fprintf(stderr, "FAIL: fixture load of position %d failed\n", i);
            failures++;
        }
    }
    stamp_lockstep();
    for (int i = 0; i < MASTER_FX_LFO_COUNT; i++) {
        memset(&shadow_master_fx_lfos[i], 0, sizeof(shadow_master_fx_lfos[i]));
        mfx_lfo_base_valid[i] = 0;
        mfx_lfo_base_value[i] = 0.0f;
    }
}

static void test_insert(void) {
    load_chain(5);
    CHECK(shadow_master_fx_count() == 5, "count after loading 5 is %d",
          shadow_master_fx_count());

    pos_t before[MASTER_FX_SLOTS];
    capture(before);
    invariants("insert:setup", NULL);

    CHECK(shadow_master_fx_insert(1) == 1, "insert at 1 was refused");
    invariants("after insert at 1", before);
    handles_preserved("after insert at 1", before, 0);

    CHECK(shadow_master_fx_count() == 6, "count after insert is %d",
          shadow_master_fx_count());
    expect_moved("after insert at 1", before, 0, 0);
    expect_vacant("after insert at 1", 1);
    for (int i = 1; i <= 4; i++) expect_moved("after insert at 1", before, i, i + 1);

    /* An append is the `+` box at the end of the chain, and is the same verb. */
    capture(before);
    CHECK(shadow_master_fx_insert(6) == 1, "append at the tail was refused");
    invariants("after append", before);
    CHECK(shadow_master_fx_count() == 7, "count after append is %d",
          shadow_master_fx_count());
    expect_vacant("after append", 6);
}

static void test_move(void) {
    load_chain(5);
    pos_t before[MASTER_FX_SLOTS];
    capture(before);

    /* Forward: 1 -> 3 rotates 2,3 down one. */
    CHECK(shadow_master_fx_move(1, 3) == 1, "move 1->3 was refused");
    invariants("after move 1->3", before);
    handles_preserved("after move 1->3", before, 0);
    CHECK(shadow_master_fx_count() == 5, "move changed the count to %d",
          shadow_master_fx_count());
    expect_moved("after move 1->3", before, 0, 0);
    expect_moved("after move 1->3", before, 2, 1);
    expect_moved("after move 1->3", before, 3, 2);
    expect_moved("after move 1->3", before, 1, 3);
    expect_moved("after move 1->3", before, 4, 4);

    /* Backward, and over a position that is EMPTY -- a hole is a legal chain
     * position, so it must be movable like any other. */
    load_chain(5);
    shadow_master_fx_slot_unload(2);
    capture(before);
    CHECK(shadow_master_fx_move(4, 0) == 1, "move 4->0 was refused");
    invariants("after move 4->0", before);
    handles_preserved("after move 4->0", before, 0);
    expect_moved("after move 4->0", before, 4, 0);
    for (int i = 0; i <= 3; i++) expect_moved("after move 4->0", before, i, i + 1);
}

static void test_remove(void) {
    load_chain(5);
    pos_t before[MASTER_FX_SLOTS];
    capture(before);

    CHECK(shadow_master_fx_remove(1) == 1, "remove at 1 was refused");
    invariants("after remove at 1", before);
    /* Exactly one dlclose: the module that left. Everything behind it was
     * renumbered, not rebuilt. */
    handles_preserved("after remove at 1", before, 1);
    expect_not_resident("after remove at 1", 1);
    /* ...and the modules that only changed index are still loaded. A remove
     * that took its neighbours down with it would be the reload this whole
     * design exists to avoid. */
    for (int i = 0; i < 4; i++) {
        CHECK(shadow_master_fx_slots[i].instance != NULL,
              "position %d lost its instance to a remove elsewhere in the chain", i);
    }

    CHECK(shadow_master_fx_count() == 4, "count after remove is %d",
          shadow_master_fx_count());
    expect_moved("after remove at 1", before, 0, 0);
    for (int i = 2; i <= 4; i++) expect_moved("after remove at 1", before, i, i - 1);
    /* The tail the shift vacated holds the DEPARTED position's buffer, cleared
     * -- rotated the other way, not stranded. */
    expect_vacant("after remove at 1", 4);
    CHECK(shadow_master_fx_slots[4].chain_params_cache == before[1].cache,
          "the removed position's slot buffer was not rotated to the tail");
    CHECK(mfx_runtime_chain_params_cache[4] == before[1].rt_cache,
          "the removed position's runtime buffer was not rotated to the tail");

    /* Removing the last position is the same verb and leaves nothing behind. */
    capture(before);
    CHECK(shadow_master_fx_remove(3) == 1, "remove at the tail was refused");
    invariants("after remove at the tail", before);
    handles_preserved("after remove at the tail", before, 1);
    CHECK(shadow_master_fx_count() == 3, "count after tail remove is %d",
          shadow_master_fx_count());
    expect_vacant("after remove at the tail", 3);
}

static void test_lfo_routing(void) {
    /* A MOVE re-aims the target and KEEPS the base: the base belongs to the
     * module, and the module went with its index. */
    load_chain(5);
    lfo_state_t *l = &shadow_master_fx_lfos[0];
    l->enabled = 1;
    l->active = 1;
    snprintf(l->target, sizeof(l->target), "fx3");   /* position 2, 1-based id */
    snprintf(l->param, sizeof(l->param), "mix");
    mfx_lfo_base_valid[0] = 1;
    mfx_lfo_base_value[0] = 0.25f;

    CHECK(shadow_master_fx_move(2, 0) == 1, "move 2->0 was refused");
    CHECK(strcmp(l->target, "fx1") == 0,
          "an LFO aimed at fx3 should follow it to fx1, got \"%s\"", l->target);
    CHECK(strcmp(l->param, "mix") == 0, "a move cleared the LFO's param");
    CHECK(mfx_lfo_base_valid[0] == 1,
          "a move invalidated the LFO base; the module kept its identity, so the "
          "base is still its base and re-snapshotting would jump the parameter");
    CHECK(mfx_lfo_base_value[0] == 0.25f, "a move changed the LFO base value");

    /* A REMOVE of the targeted position clears both halves of the routing AND
     * invalidates the base. mfx_lfo_base_valid[] is indexed BY LFO, not by
     * position: leave it valid and the NEXT target the user picks is modulated
     * around the departed module's value. */
    CHECK(shadow_master_fx_remove(0) == 1, "remove at 0 was refused");
    CHECK(l->target[0] == '\0',
          "removing an LFO's target left it aimed at \"%s\" -- whatever slid into "
          "that index is now being modulated", l->target);
    CHECK(l->param[0] == '\0',
          "removing an LFO's target left param \"%s\" behind; a later target write "
          "silently revives half a routing", l->param);
    CHECK(mfx_lfo_base_valid[0] == 0,
          "removing an LFO's target left its base marked valid -- the next target "
          "will be modulated around a stale base");

    /* A routing BEHIND the removed position renumbers rather than being lost. */
    load_chain(5);
    l = &shadow_master_fx_lfos[1];
    l->enabled = 1;
    snprintf(l->target, sizeof(l->target), "fx4");   /* position 3 */
    snprintf(l->param, sizeof(l->param), "mix");
    mfx_lfo_base_valid[1] = 1;
    mfx_lfo_base_value[1] = 0.75f;
    CHECK(shadow_master_fx_remove(1) == 1, "remove at 1 was refused");
    CHECK(strcmp(l->target, "fx3") == 0,
          "a routing behind a removed position should renumber to fx3, got \"%s\"",
          l->target);
    CHECK(mfx_lfo_base_valid[1] == 1 && mfx_lfo_base_value[1] == 0.75f,
          "renumbering a surviving routing must not disturb its base");
}

/* A refused edit must change NOTHING. The owned pointers are lifted out of the
 * structs and put back around every permutation, so a refusal that took the
 * long way home would be a silent corruption of exactly the thing this design
 * exists to protect. */
static void expect_refused(const char *what, int result, const pos_t *before) {
    CHECK(result == 0, "%s should have been refused", what);
    pos_t now[MASTER_FX_SLOTS];
    capture(now);
    CHECK(memcmp(before, now, sizeof(now)) == 0,
          "%s was refused but changed state anyway", what);
}

static void test_refusals(void) {
    load_chain(5);
    pos_t before[MASTER_FX_SLOTS];
    capture(before);
    int count = shadow_master_fx_count();

    expect_refused("insert past the end", shadow_master_fx_insert(count + 1), before);
    expect_refused("insert at a negative index", shadow_master_fx_insert(-1), before);
    expect_refused("remove past the end", shadow_master_fx_remove(count), before);
    expect_refused("remove at a negative index", shadow_master_fx_remove(-1), before);
    expect_refused("move to itself", shadow_master_fx_move(2, 2), before);
    expect_refused("move from past the end", shadow_master_fx_move(count, 0), before);
    expect_refused("move to past the end", shadow_master_fx_move(0, count), before);
    CHECK(shadow_master_fx_count() == count,
          "a refused edit changed the count to %d", shadow_master_fx_count());

    /* The same, over a chain whose LAST position is a hole.
     *
     * Without this the check above is weak: the published count repairs itself
     * from "highest loaded + 1", so a refused edit that corrupted the stored
     * count would be silently healed and the test would pass. A trailing hole
     * is the one shape where the stored count is the only thing that knows how
     * long the chain is -- and it is the shape the `+` box produces, since an
     * append opens an empty position before anything is loaded into it. */
    load_chain(4);
    CHECK(shadow_master_fx_insert(4) == 1, "append to open a trailing hole was refused");
    CHECK(shadow_master_fx_count() == 5, "an appended hole should make the chain 5 long");
    capture(before);
    expect_refused("move to itself, with a trailing hole",
                   shadow_master_fx_move(1, 1), before);
    CHECK(shadow_master_fx_count() == 5,
          "a refused edit lost the trailing hole; the chain is now %d long",
          shadow_master_fx_count());
    expect_refused("insert past the end, with a trailing hole",
                   shadow_master_fx_insert(6), before);
    CHECK(shadow_master_fx_count() == 5,
          "a refused insert lost the trailing hole; the chain is now %d long",
          shadow_master_fx_count());

    /* Full: there is no spare buffer to rotate into, and chain_perm_insert
     * refuses rather than truncating. */
    load_chain(MASTER_FX_SLOTS);
    capture(before);
    CHECK(shadow_master_fx_count() == MASTER_FX_SLOTS, "expected a full chain");
    expect_refused("insert into a full chain", shadow_master_fx_insert(0), before);
}

/* fx_count is the published length and the UI bounds its list by it. It must
 * never be less than "highest loaded + 1": boot restore and patch load both
 * bring positions up without a shape edit. */
static void test_count_honesty(void) {
    shadow_chain_defaults();
    CHECK(shadow_master_fx_count() == 0, "an empty Master FX should publish 0, got %d",
          shadow_master_fx_count());

    char path[512];
    fixture_path(7, path, sizeof(path));
    CHECK(shadow_master_fx_slot_load(7, path) == 0, "load into the last position failed");
    CHECK(shadow_master_fx_count() == MASTER_FX_SLOTS,
          "a module loaded into the last position must be inside the published "
          "count, got %d", shadow_master_fx_count());

    /* A hole is a legal chain position: emptying one does NOT shorten the
     * chain, exactly as in a slot chain's fx section. */
    load_chain(5);
    shadow_master_fx_slot_unload(2);
    CHECK(shadow_master_fx_count() == 5,
          "unloading a mid-chain position shortened the chain to %d",
          shadow_master_fx_count());
}

/* ------------------------------------------------------------- MIDI CAPTURE
 *
 * Capture follows the MODULE, never the position.
 *
 * shadow_midi.c used to answer "does Master FX want this control" from a raw
 * shadow_capture_rules_t* cached at init, taken from what was then
 * shadow_master_fx_slots[0].capture -- POSITION 0 and nothing else. That was
 * invisible while Master FX was a fixed array nobody reordered. With the move
 * gesture it becomes: drag a MIDI-triggered module -- a ducker, which is
 * exactly what people put on a master bus -- off position 0, and it silently
 * stops receiving MIDI. No swap, no reload, nothing to blame it on.
 *
 * So the assertion is not "capture works": it is that a module declaring
 * capture is heard from EVERY position, and keeps being heard after a
 * permutation moves it. The negatives are load-bearing too -- a union that
 * simply returned 1 would satisfy every positive above.
 */
static void capturing_fixture_path(char *buf, size_t n) {
    snprintf(buf, n, "%s/mfxcap/dsp.so", FIXTURE_DIR);
}

static void expect_capture(const char *stage, int pads, int tracks) {
    CHECK(shadow_master_fx_captures_note(CAPTURE_PADS_NOTE_MIN) == pads,
          "[%s] pad note %d: captures_note said %d, expected %d",
          stage, CAPTURE_PADS_NOTE_MIN,
          shadow_master_fx_captures_note(CAPTURE_PADS_NOTE_MIN), pads);
    CHECK(shadow_master_fx_captures_note(CAPTURE_PADS_NOTE_MAX) == pads,
          "[%s] pad note %d: captures_note said %d, expected %d",
          stage, CAPTURE_PADS_NOTE_MAX,
          shadow_master_fx_captures_note(CAPTURE_PADS_NOTE_MAX), pads);
    CHECK(shadow_master_fx_captures_cc(CAPTURE_TRACKS_CC_MIN) == tracks,
          "[%s] track CC %d: captures_cc said %d, expected %d",
          stage, CAPTURE_TRACKS_CC_MIN,
          shadow_master_fx_captures_cc(CAPTURE_TRACKS_CC_MIN), tracks);
    /* The fixture declares pads + tracks and nothing else. Without these, a
     * predicate that always said yes would pass every check above. */
    CHECK(!shadow_master_fx_captures_note(CAPTURE_STEPS_NOTE_MIN),
          "[%s] a step note was claimed by a chain that never declared steps", stage);
    CHECK(!shadow_master_fx_captures_cc(CAPTURE_KNOBS_CC_MIN),
          "[%s] a knob CC was claimed by a chain that never declared knobs", stage);
}

static void test_capture_follows_the_module(void) {
    char cap_path[512];
    capturing_fixture_path(cap_path, sizeof(cap_path));

    shadow_master_fx_unload_all();
    expect_capture("nothing loaded", 0, 0);

    /* THE one: every position, alone, must be heard. */
    for (int p = 0; p < MASTER_FX_SLOTS; p++) {
        shadow_master_fx_unload_all();
        CHECK(shadow_master_fx_slot_load(p, cap_path) == 0,
              "the capturing fixture failed to load at position %d", p);
        char stage[64];
        snprintf(stage, sizeof(stage), "capturing module alone at position %d", p);
        expect_capture(stage, 1, 1);
    }

    /* A loaded chain of modules that declare nothing must claim nothing --
     * otherwise the loop above proves only that something, somewhere, is on. */
    load_chain(MASTER_FX_SLOTS);
    expect_capture("a chain of non-capturing modules", 0, 0);

    /* Now the gesture that made this user-visible. Put the capturing module at
     * the head, with plain modules behind it, and move it away. */
    shadow_master_fx_unload_all();
    for (int i = 1; i < 4; i++) {
        char path[512];
        fixture_path(i, path, sizeof(path));
        CHECK(shadow_master_fx_slot_load(i, path) == 0,
              "fixture load of position %d failed", i);
    }
    CHECK(shadow_master_fx_slot_load(0, cap_path) == 0,
          "the capturing fixture failed to load at the head");
    expect_capture("capturing module at the head", 1, 1);

    CHECK(shadow_master_fx_move(0, 3) == 1, "move 0->3 was refused");
    CHECK(strcmp(shadow_master_fx_slots[3].module_id, "mfxcap") == 0,
          "after move 0->3 the capturing module is at neither end: position 3 holds "
          "\"%s\"", shadow_master_fx_slots[3].module_id);
    expect_capture("capturing module dragged from position 0 to position 3", 1, 1);

    /* ...and its rules leave with it, rather than haunting the position. */
    CHECK(shadow_master_fx_remove(3) == 1, "remove of position 3 was refused");
    expect_capture("capturing module removed", 0, 0);

    shadow_master_fx_unload_all();
}

/* ------------------------------------------------------- the param surface
 *
 * 4e is pure JS, which is only true if the wire keys already work. So drive
 * shadow_inprocess_handle_param_request itself -- the same entry point the SPI
 * callback uses -- rather than calling the C functions directly.
 *
 * Ids are 1-BASED on the wire, matching "fx1".."fx8" and the slot chain's
 * fx:insert. An off-by-one here would edit the wrong position, and there is no
 * type in JS to catch it.
 */
static shadow_param_t g_param;
static shadow_param_t *g_param_ptr = &g_param;
static uint32_t g_req_id = 0;

static void param_request(uint8_t type, const char *key, const char *value) {
    memset(&g_param, 0, sizeof(g_param));
    snprintf(g_param.key, SHADOW_PARAM_KEY_LEN, "%s", key);
    if (value) snprintf(g_param.value, SHADOW_PARAM_VALUE_LEN, "%s", value);
    g_param.request_id = ++g_req_id;
    __atomic_store_n(&g_param.request_type, type, __ATOMIC_RELEASE);
    shadow_inprocess_handle_param_request();
}

static void test_param_surface(void) {
    load_chain(4);

    param_request(2, "master_fx:fx_count", NULL);
    CHECK(g_param.error == 0 && strcmp((const char *)g_param.value, "4") == 0,
          "master_fx:fx_count answered \"%s\" (error %d), expected 4",
          (const char *)g_param.value, g_param.error);

    pos_t before[MASTER_FX_SLOTS];
    capture(before);

    /* "2" is 1-based: open a hole at position 1. */
    param_request(1, "master_fx:fx:insert", "2");
    CHECK(g_param.error == 0, "master_fx:fx:insert failed with error %d", g_param.error);
    expect_moved("via master_fx:fx:insert", before, 0, 0);
    expect_vacant("via master_fx:fx:insert", 1);
    expect_moved("via master_fx:fx:insert", before, 1, 2);
    invariants("via master_fx:fx:insert", before);

    param_request(2, "master_fx:fx_count", NULL);
    CHECK(strcmp((const char *)g_param.value, "5") == 0,
          "fx_count after an insert answered \"%s\", expected 5",
          (const char *)g_param.value);

    capture(before);
    param_request(1, "master_fx:fx:move", "1>3");
    CHECK(g_param.error == 0, "master_fx:fx:move failed with error %d", g_param.error);
    expect_moved("via master_fx:fx:move", before, 0, 2);
    invariants("via master_fx:fx:move", before);

    capture(before);
    param_request(1, "master_fx:fx:remove", "3");
    CHECK(g_param.error == 0, "master_fx:fx:remove failed with error %d", g_param.error);
    expect_moved("via master_fx:fx:remove", before, 3, 2);
    invariants("via master_fx:fx:remove", before);
    param_request(2, "master_fx:fx_count", NULL);
    CHECK(strcmp((const char *)g_param.value, "4") == 0,
          "fx_count after a remove answered \"%s\", expected 4",
          (const char *)g_param.value);

    /* A malformed verb must be refused, not routed into position 0's module
     * as a garbage param write -- the misroute master_fx_key.h exists to stop. */
    capture(before);
    param_request(1, "master_fx:fx:move", "not-a-move");
    CHECK(g_param.error != 0, "a malformed move was accepted");
    param_request(1, "master_fx:fx:insert", "99");
    CHECK(g_param.error != 0, "an out-of-range insert was accepted");
    param_request(1, "master_fx:fx_count", "3");
    CHECK(g_param.error != 0, "fx_count is read-only but a write was accepted");
    pos_t now[MASTER_FX_SLOTS];
    capture(now);
    CHECK(memcmp(before, now, sizeof(now)) == 0,
          "a refused verb over the param channel changed state anyway");
}

int main(void) {
    chain_mgmt_host_t h;
    memset(&h, 0, sizeof(h));
    h.shadow_param_ptr = &g_param_ptr;
    chain_mgmt_init(&h);

    /* Prove the fixtures are distinguishable before relying on that. */
    load_chain(MASTER_FX_SLOTS);
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        CHECK(strcmp(shadow_master_fx_slots[i].module_id, expected_id(i)) == 0,
              "fixture %d loaded as \"%s\"", i, shadow_master_fx_slots[i].module_id);
    }

    test_insert();
    test_move();
    test_remove();
    test_lfo_routing();
    test_refusals();
    test_count_honesty();
    test_capture_follows_the_module();
    test_param_surface();

    shadow_master_fx_unload_all();

    if (failures) {
        fprintf(stderr, "%d/%d check(s) FAILED\n", failures, checks);
        return 1;
    }
    printf("test_master_fx_permute: %d checks pass (MASTER_FX_SLOTS = %d)\n",
           checks, MASTER_FX_SLOTS);
    return 0;
}
