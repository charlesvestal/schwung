/*
 * The never-NULL invariant for Master FX owned param caches.
 *
 * master_fx_slot_t.chain_params_cache and the file-static
 * mfx_runtime_chain_params_cache[] are 64 KB buffers that used to be inline
 * arrays. They are pointers now because Master FX is becoming a list whose
 * reordering is a PERMUTATION run on the SPI callback, and a permutation must
 * rotate pointers rather than memmove ~128 KB per position through a ~900 us
 * budget.
 *
 * That is the exact shape that took the SPI callback down on the slot chain:
 * a vacated position had its owned pointer NULLed instead of rotated, and
 * v2_load_midi_fx_slot then parsed a param table through it — SIGSEGV on the
 * audio thread, plus a silently leaked allocation. See the PERMUTATION section
 * of docs/CHAIN.md and PERM_OWNED in src/host/chain_permute.h.
 *
 * The permutation is not written yet (step 4d). This test establishes the
 * invariant it will depend on, against the REAL shadow_chain_mgmt.c and the
 * REAL loader, driven through the whole lifecycle:
 *
 *     chain_mgmt_init -> shadow_chain_defaults -> load -> unload -> re-load
 *
 * At every step: every position has both buffers, non-NULL; the multiset of
 * buffer addresses is unchanged (nothing reallocated, nothing swapped, nothing
 * leaked); and no two positions share a buffer, which is what makes a rotation
 * meaningful rather than aliasing.
 *
 * A source grep would not catch this. shadow_chain_defaults() used to memset
 * the struct, which now compiles fine and reads fine and strands the pointer.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* One of the three per-position arrays is file-static, so the unit under test
 * is INCLUDED rather than linked: no test-only accessor is added to production
 * code, and the arrays observed here are literally the ones the loader writes.
 * The wrapper supplies stubs for the handful of externs this pulls in. */
#include "shadow_chain_mgmt.c"

#ifndef FIXTURE_DSP_PATH
#error "FIXTURE_DSP_PATH must be defined by the test wrapper"
#endif

#define mfx_test_runtime_cache_ptr(i) (mfx_runtime_chain_params_cache[(i)])

static int failures = 0;
static int checks = 0;

/* Snapshot of every owned pointer, so a step can be compared against the one
 * before it. */
typedef struct {
    char *slot_cache[MASTER_FX_SLOTS];
    char *runtime_cache[MASTER_FX_SLOTS];
} cache_snapshot_t;

static void snapshot(cache_snapshot_t *out) {
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        out->slot_cache[i] = shadow_master_fx_slots[i].chain_params_cache;
        out->runtime_cache[i] = mfx_test_runtime_cache_ptr(i);
    }
}

/* THE invariant. Everything else in this file is scaffolding for calling it
 * after each lifecycle step. */
static void assert_no_null(const char *stage) {
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        checks++;
        if (!shadow_master_fx_slots[i].chain_params_cache) {
            fprintf(stderr,
                    "FAIL [%s]: shadow_master_fx_slots[%d].chain_params_cache is NULL\n"
                    "      An owned buffer was released instead of rotated. The next\n"
                    "      loader parses a param table through it, on the SPI callback.\n",
                    stage, i);
            failures++;
        }
        checks++;
        if (!mfx_test_runtime_cache_ptr(i)) {
            fprintf(stderr,
                    "FAIL [%s]: mfx_runtime_chain_params_cache[%d] is NULL\n"
                    "      An owned buffer was released instead of rotated. The next\n"
                    "      loader parses a param table through it, on the SPI callback.\n",
                    stage, i);
            failures++;
        }
    }
}

/* No two positions may share a buffer: a rotation that aliased would let one
 * position's params overwrite another's, and would double-free at teardown. */
static void assert_distinct(const char *stage) {
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        for (int j = i + 1; j < MASTER_FX_SLOTS; j++) {
            checks++;
            if (shadow_master_fx_slots[i].chain_params_cache &&
                shadow_master_fx_slots[i].chain_params_cache ==
                shadow_master_fx_slots[j].chain_params_cache) {
                fprintf(stderr, "FAIL [%s]: slot cache %d and %d alias\n", stage, i, j);
                failures++;
            }
            checks++;
            if (mfx_test_runtime_cache_ptr(i) &&
                mfx_test_runtime_cache_ptr(i) == mfx_test_runtime_cache_ptr(j)) {
                fprintf(stderr, "FAIL [%s]: runtime cache %d and %d alias\n", stage, i, j);
                failures++;
            }
        }
    }
}

/* The multiset of addresses must survive every step. Compared as a multiset,
 * not element-wise, so this stays true once 4d starts rotating them. */
static int ptr_cmp(const void *a, const void *b) {
    char *const *pa = (char *const *)a;
    char *const *pb = (char *const *)b;
    if (*pa < *pb) return -1;
    if (*pa > *pb) return 1;
    return 0;
}

static void assert_same_multiset(const char *stage,
                                 const cache_snapshot_t *before,
                                 const cache_snapshot_t *after) {
    char *b[MASTER_FX_SLOTS], *a[MASTER_FX_SLOTS];

    memcpy(b, before->slot_cache, sizeof(b));
    memcpy(a, after->slot_cache, sizeof(a));
    qsort(b, MASTER_FX_SLOTS, sizeof(char *), ptr_cmp);
    qsort(a, MASTER_FX_SLOTS, sizeof(char *), ptr_cmp);
    checks++;
    if (memcmp(b, a, sizeof(b)) != 0) {
        fprintf(stderr, "FAIL [%s]: slot cache buffers were reallocated or lost\n", stage);
        failures++;
    }

    memcpy(b, before->runtime_cache, sizeof(b));
    memcpy(a, after->runtime_cache, sizeof(a));
    qsort(b, MASTER_FX_SLOTS, sizeof(char *), ptr_cmp);
    qsort(a, MASTER_FX_SLOTS, sizeof(char *), ptr_cmp);
    checks++;
    if (memcmp(b, a, sizeof(b)) != 0) {
        fprintf(stderr, "FAIL [%s]: runtime cache buffers were reallocated or lost\n", stage);
        failures++;
    }
}

static void step(const char *stage, cache_snapshot_t *prev) {
    cache_snapshot_t now;
    assert_no_null(stage);
    assert_distinct(stage);
    snapshot(&now);
    assert_same_multiset(stage, prev, &now);
    *prev = now;
}

int main(void) {
    chain_mgmt_host_t h;
    memset(&h, 0, sizeof(h));

    /* --- init ------------------------------------------------------- */
    chain_mgmt_init(&h);
    assert_no_null("after chain_mgmt_init");
    assert_distinct("after chain_mgmt_init");

    cache_snapshot_t prev;
    snapshot(&prev);

    /* --- defaults: the step that used to memset the struct ----------- */
    shadow_chain_defaults();
    step("after shadow_chain_defaults", &prev);

    /* --- load every position with the real loader -------------------- */
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        if (shadow_master_fx_slot_load(i, FIXTURE_DSP_PATH) != 0) {
            fprintf(stderr, "FAIL: load of position %d failed\n", i);
            failures++;
        }
        checks++;
        if (!shadow_master_fx_slots[i].instance) {
            fprintf(stderr, "FAIL: position %d has no instance after load\n", i);
            failures++;
        }
    }
    step("after loading every position", &prev);

    /* The loader must have filled the module.json snapshot through the owned
     * pointer — proving the buffer is actually the one being written, not a
     * decoration the test could pass with a dangling pointer. */
    checks++;
    if (!shadow_master_fx_slots[0].chain_params_cached ||
        shadow_master_fx_slots[0].chain_params_cache[0] != '[') {
        fprintf(stderr, "FAIL: chain_params were not cached into the owned buffer\n");
        failures++;
    }

    /* --- unload, mid-chain first (the vacate a remove will do) ------- */
    shadow_master_fx_slot_unload(2);
    step("after unloading a mid-chain position", &prev);
    checks++;
    if (shadow_master_fx_slots[2].instance) {
        fprintf(stderr, "FAIL: position 2 still has an instance after unload\n");
        failures++;
    }

    shadow_master_fx_unload_all();
    step("after unloading every position", &prev);

    /* --- re-load: the vacated buffers must still be usable ----------- */
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        if (shadow_master_fx_slot_load(i, FIXTURE_DSP_PATH) != 0) {
            fprintf(stderr, "FAIL: re-load of position %d failed\n", i);
            failures++;
        }
    }
    step("after re-loading every position", &prev);

    checks++;
    if (!shadow_master_fx_slots[2].chain_params_cached) {
        fprintf(stderr, "FAIL: re-loaded position 2 did not re-cache chain_params\n");
        failures++;
    }

    /* --- defaults again, this time over LOADED positions ------------- *
     * The old memset survived boot only because the slots were empty. */
    shadow_chain_defaults();
    step("after shadow_chain_defaults over loaded positions", &prev);
    for (int i = 0; i < MASTER_FX_SLOTS; i++) {
        checks++;
        if (shadow_master_fx_slots[i].instance || shadow_master_fx_slots[i].handle) {
            fprintf(stderr, "FAIL: shadow_chain_defaults left position %d loaded\n", i);
            failures++;
        }
        checks++;
        if (shadow_master_fx_slots[i].chain_params_cached ||
            shadow_master_fx_slots[i].chain_params_cache[0] != '\0') {
            fprintf(stderr, "FAIL: shadow_chain_defaults left stale params at %d\n", i);
            failures++;
        }
    }

    /* --- ensure() is idempotent and must not reallocate --------------- */
    if (!shadow_master_fx_storage_ensure()) {
        fprintf(stderr, "FAIL: shadow_master_fx_storage_ensure reported missing storage\n");
        failures++;
    }
    step("after a redundant shadow_master_fx_storage_ensure", &prev);

    if (failures) {
        fprintf(stderr, "%d/%d check(s) FAILED\n", failures, checks);
        return 1;
    }
    printf("test_master_fx_cache_ownership: %d checks pass "
           "(MASTER_FX_SLOTS = %d, cache = %d bytes)\n",
           checks, MASTER_FX_SLOTS, MASTER_FX_CHAIN_PARAMS_MAX);
    return 0;
}
