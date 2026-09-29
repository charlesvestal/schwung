/*
 * scene_morph.h: the formula, the verbs, the wire format.
 *
 * Every case here is one sentence of the spec
 * (docs/superpowers/specs/2026-09-27-scene-morphing-design.md, section 1).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "scene_morph.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-5f)

static scene_table_t t, t2, scratch;

static float eval(const scene_table_t *tb, const char *target, const char *param,
                  int a, int b, float base, float x, int kind, int *contributes) {
    int i = scene_find(tb, target, param);
    int ha = 0, hb = 0; float va = 0, vb = 0;
    *contributes = (i >= 0) && scene_resolve(&tb->pairs[i], a, b, &ha, &va, &hb, &vb);
    if (!*contributes) return base;
    return scene_morph_value(ha, va, hb, vb, base, x, kind);
}

int main(void) {
    int c;
    memset(&t, 0, sizeof(t));

    /* ---- the formula ---------------------------------------------------- */
    scene_lock(&t, 0, "synth", "cutoff", 0.2f, "obxd");
    scene_lock(&t, 1, "synth", "cutoff", 0.8f, "obxd");
    CHECK(NEAR(eval(&t, "synth", "cutoff", 0, 1, 0.5f, 0.0f, SCENE_KIND_FLOAT, &c), 0.2f) && c,
          "locked at both ends: x=0 is A");
    CHECK(NEAR(eval(&t, "synth", "cutoff", 0, 1, 0.5f, 1.0f, SCENE_KIND_FLOAT, &c), 0.8f),
          "locked at both ends: x=1 is B");
    CHECK(NEAR(eval(&t, "synth", "cutoff", 0, 1, 0.5f, 0.25f, SCENE_KIND_FLOAT, &c), 0.35f),
          "locked at both ends: x=0.25 interpolates");

    scene_lock(&t, 2, "synth", "reso", 1.0f, "obxd");
    CHECK(NEAR(eval(&t, "synth", "reso", 2, 3, 0.4f, 0.5f, SCENE_KIND_FLOAT, &c), 0.7f),
          "A only: morphs A -> base");
    CHECK(NEAR(eval(&t, "synth", "reso", 3, 2, 0.4f, 0.5f, SCENE_KIND_FLOAT, &c), 0.7f),
          "B only: morphs base -> B");
    CHECK(NEAR(eval(&t, "synth", "reso", 3, 2, 0.1f, 0.5f, SCENE_KIND_FLOAT, &c), 0.55f),
          "the unlocked end follows a LIVE base");
    eval(&t, "synth", "reso", 4, 5, 0.4f, 0.5f, SCENE_KIND_FLOAT, &c);
    CHECK(!c, "locked in neither end: no contribution");
    eval(&t, "synth", "nothing", 0, 1, 0.4f, 0.5f, SCENE_KIND_FLOAT, &c);
    CHECK(!c, "unknown pair: no contribution");

    scene_lock(&t, 0, "fx1", "mode", 1.0f, "freeverb");
    scene_lock(&t, 1, "fx1", "mode", 3.0f, "freeverb");
    CHECK(NEAR(eval(&t, "fx1", "mode", 0, 1, 0, 0.4999f, SCENE_KIND_ENUM, &c), 1.0f),
          "enum below 0.5 holds A");
    CHECK(NEAR(eval(&t, "fx1", "mode", 0, 1, 0, 0.5f, SCENE_KIND_ENUM, &c), 3.0f),
          "enum switches at exactly 0.5");
    CHECK(NEAR(eval(&t, "fx1", "mode", 0, 1, 0, 0.3f, SCENE_KIND_INT, &c), 2.0f),
          "int is rounded (1 + 2*0.3 = 1.6 -> 2)");

    CHECK(NEAR(eval(&t, "synth", "cutoff", 1, 1, 0.5f, 0.0f, SCENE_KIND_FLOAT, &c), 0.8f) &&
          NEAR(eval(&t, "synth", "cutoff", 1, 1, 0.5f, 0.7f, SCENE_KIND_FLOAT, &c), 0.8f),
          "A == B: the fader has no effect");
    CHECK(NEAR(eval(&t, "synth", "cutoff", SCENE_NONE, 1, 0.5f, 0.5f, SCENE_KIND_FLOAT, &c), 0.65f),
          "A = none: morphs base -> B");
    CHECK(NEAR(eval(&t, "synth", "cutoff", SCENE_NONE, SCENE_NONE, 0.5f, 0.5f, SCENE_KIND_FLOAT, &c), 0.5f) && !c,
          "A = B = none: nothing");
    CHECK(NEAR(scene_morph_value(1, 0.2f, 1, 0.8f, 0, NAN, SCENE_KIND_FLOAT), 0.2f),
          "NaN fader clamps to 0");
    CHECK(scene_xfade_to_q(1.0f) == 65535 && scene_xfade_to_q(0.0f) == 0 &&
          NEAR(scene_xfade_from_q(scene_xfade_to_q(0.5f)), 0.5f), "fader q round trip");

    /* ---- module replacement --------------------------------------------- */
    scene_lock(&t, 5, "synth", "cutoff", 0.9f, "dx7");
    int i = scene_find(&t, "synth", "cutoff");
    CHECK(i >= 0 && t.pairs[i].mask == (1u << 5) && strcmp(t.pairs[i].module, "dx7") == 0,
          "a lock under a DIFFERENT module replaces the pair");

    /* ---- verbs ---------------------------------------------------------- */
    memset(&t, 0, sizeof(t));
    CHECK(scene_apply_lock_verb(&t, "3 synth cutoff 0.25 obxd") == SCENE_OK &&
          scene_lock_count(&t, 3) == 1, "lock verb");
    CHECK(scene_apply_lock_verb(&t, "32 synth cutoff 0.25 obxd") == SCENE_ERR_ARGS, "half 32 rejected");
    CHECK(scene_apply_lock_verb(&t, "31 synth cutoff 0.25 obxd") == SCENE_OK && (t.pairs[0].mask >> 31) == 1, "half 31 fits in the mask");
    scene_apply_unlock_verb(&t, "31 synth cutoff");
    CHECK(scene_apply_lock_verb(&t, "3 synth cutoff nan obxd") == SCENE_ERR_ARGS, "nan value rejected");
    CHECK(scene_apply_lock_verb(&t, "3 synth cutoff 0.2") == SCENE_ERR_ARGS, "missing module rejected");
    CHECK(scene_apply_lock_verb(&t, "3 synth cutoff 0.2 obxd extra") == SCENE_ERR_ARGS, "trailing token rejected");
    CHECK(scene_apply_lock_verb(&t, "3 synth a_param_key_that_is_far_too_long_to_fit 0.2 obxd") == SCENE_ERR_ARGS,
          "an over-long key is refused, never truncated");
    scene_apply_lock_verb(&t, "4 synth cutoff 0.5 obxd");
    CHECK(scene_apply_unlock_verb(&t, "3 synth cutoff") == SCENE_OK &&
          scene_lock_count(&t, 3) == 0 && scene_lock_count(&t, 4) == 1, "unlock one scene keeps the other");
    CHECK(scene_apply_unlock_verb(&t, "4 synth cutoff") == SCENE_OK && t.count == 0,
          "unlocking the last scene removes the pair");

    scene_apply_lock_verb(&t, "0 synth a 0.1 m");
    scene_apply_lock_verb(&t, "0 synth b 0.2 m");
    scene_apply_lock_verb(&t, "1 synth b 0.3 m");
    scene_apply_lock_verb(&t, "1 synth c 0.4 m");
    CHECK(scene_apply_copy_verb(&t, "0 1") == SCENE_OK &&
          scene_lock_count(&t, 1) == 2 && scene_find(&t, "synth", "c") < 0 &&
          NEAR(t.pairs[scene_find(&t, "synth", "b")].values[1], 0.2f),
          "copy makes dst EXACTLY src (its own extra lock is gone)");
    CHECK(scene_apply_clear_verb(&t, "0") == SCENE_OK && scene_lock_count(&t, 0) == 0 &&
          scene_lock_count(&t, 1) == 2, "clear one scene");
    CHECK(scene_apply_clear_verb(&t, "x") == SCENE_ERR_ARGS, "clear rejects garbage");

    /* ---- cap ------------------------------------------------------------ */
    memset(&t, 0, sizeof(t));
    char key[32];
    for (int k = 0; k < SCENE_MAX_PAIRS; k++) {
        snprintf(key, sizeof(key), "p%d", k);
        scene_lock(&t, 0, "synth", key, 0.5f, "m");
    }
    CHECK(t.count == SCENE_MAX_PAIRS, "fills to the cap");
    CHECK(scene_lock(&t, 1, "synth", "one_more", 0.5f, "m") == SCENE_ERR_FULL && t.count == SCENE_MAX_PAIRS,
          "a new pair past the cap is REFUSED");
    CHECK(scene_lock(&t, 1, "synth", "p7", 0.9f, "m") == SCENE_OK,
          "a new scene on an EXISTING pair still fits at the cap");

    /* ---- load / dump ---------------------------------------------------- */
    memset(&t, 0, sizeof(t));
    const char *doc = "0 synth cutoff 0.25 obxd\n1 synth cutoff 0.75 obxd\n\n15 fx2 mix 0.123456791 cloudseed\n";
    CHECK(scene_load(&t, &scratch, doc) == SCENE_OK && t.count == 2, "load");
    static char a[8192], b[8192];
    int na = scene_dump(&t, a, sizeof(a));
    memset(&t2, 0, sizeof(t2));
    CHECK(na > 0 && scene_load(&t2, &scratch, a) == SCENE_OK, "dump loads back");
    int nb = scene_dump(&t2, b, sizeof(b));
    CHECK(na == nb && memcmp(a, b, (size_t)na) == 0, "dump -> load -> dump is BYTE-EXACT");
    CHECK(memcmp(&t, &t2, sizeof(t)) == 0, "and the tables are identical");

    scene_table_t before = t;
    CHECK(scene_load(&t, &scratch, "0 synth x 0.1 m\n0 synth y bogus m\n") == SCENE_ERR_ARGS &&
          memcmp(&t, &before, sizeof(t)) == 0, "a malformed line rejects the WHOLE load, table untouched");
    CHECK(scene_load(&t, &scratch, "") == SCENE_OK && t.count == 0, "an empty load is an empty bank");
    char tiny[16];
    CHECK(scene_dump(&before, tiny, sizeof(tiny)) == -1 && tiny[0] == '\0',
          "a dump that does not fit fails whole, never truncates");

    /* ---- edit filter ---------------------------------------------------- */
    CHECK(scene_edit_subkey_eligible("cutoff"), "plain param eligible");
    CHECK(!scene_edit_subkey_eligible("module") && !scene_edit_subkey_eligible("state") &&
          !scene_edit_subkey_eligible("bypassed") && !scene_edit_subkey_eligible("preset"),
          "identity / state / bypass / preset never lock");
    CHECK(!scene_edit_subkey_eligible("cutoff:effective") && !scene_edit_subkey_eligible("cutoff:base"),
          "suffixed views never lock");
    CHECK(!scene_edit_subkey_eligible("") && !scene_edit_subkey_eligible(NULL), "empty never locks");

    /* THE LIVE TAKEOVER: three points, (0, A) (x0, k) (1, B). */
    {
        scene_takeover_t t = { 1, 0.75f, 50.0f };   /* A 0, B 100; turned to 50 at 75% */
        CHECK(NEAR(scene_takeover_value(&t, 0, 100, 0.75f, SCENE_KIND_FLOAT), 50), "at the anchor: the turn");
        CHECK(NEAR(scene_takeover_value(&t, 0, 100, 0.875f, SCENE_KIND_FLOAT), 75), "toward B: halfway to B");
        CHECK(NEAR(scene_takeover_value(&t, 0, 100, 0.375f, SCENE_KIND_FLOAT), 25), "toward A: halfway to A");
        CHECK(NEAR(scene_takeover_value(&t, 0, 100, 0.0f, SCENE_KIND_FLOAT), 0) &&
              NEAR(scene_takeover_value(&t, 0, 100, 1.0f, SCENE_KIND_FLOAT), 100), "the ends are the ends");
        CHECK(!scene_takeover_expired(&t, 0.5f) && scene_takeover_expired(&t, 0.0f) &&
              scene_takeover_expired(&t, 1.0f), "released at EITHER end, never between");
        scene_takeover_t e = { 1, 1.0f, 30.0f };    /* turned AT B */
        CHECK(NEAR(scene_takeover_value(&e, 0, 100, 1.0f, SCENE_KIND_FLOAT), 30) && !scene_takeover_expired(&e, 1.0f),
              "an anchor made AT an end holds there");
        CHECK(NEAR(scene_takeover_value(&e, 0, 100, 0.5f, SCENE_KIND_FLOAT), 15) && scene_takeover_expired(&e, 0.0f),
              "... morphs to the other end, and goes there");
        CHECK(NEAR(scene_takeover_k(80, 10, 15, SCENE_KIND_FLOAT, 0, 127), 85), "a turn is a CHANGE to what is heard");
        CHECK(NEAR(scene_takeover_k(125, 10, 15, SCENE_KIND_FLOAT, 0, 127), 127), "... clamped to the range");
        CHECK(NEAR(scene_takeover_k(3, 0, 2, SCENE_KIND_ENUM, 0, 3), 2), "an enum takes the written option");
    }

    /* PROGRAM CHANGE SELECTS A SCENE. */
    {
        uint8_t pairs[32];
        for (int k = 0; k < 16; k++) { pairs[k * 2] = (uint8_t)k; pairs[k * 2 + 1] = (uint8_t)k; }
        pairs[4] = 8; pairs[5] = SCENE_NONE;          /* scene 3 = A9 + none */
        uint8_t k = 99, a = 99, b = 99;
        CHECK(scene_pc_select(16, 0xCF, 2, pairs, &k, &a, &b) && k == 2 && a == 8 && b == SCENE_NONE,
              "PC 2 on ch 16 = scene 3, its pairing: A9 (half 8), B none");
        CHECK(scene_pc_select(16, 0xCF, 5, pairs, &k, &a, &b) && k == 5 && a == 5 && b == 16 + 5,
              "PC 5 = scene 6 = A6 + B6 (halves 5, 21)");
        CHECK(!scene_pc_select(16, 0xC0, 2, pairs, &k, &a, &b), "another channel is not ours");
        CHECK(!scene_pc_select(16, 0xCF, 16, pairs, &k, &a, &b), "PC 16 and up is not a scene");
        CHECK(!scene_pc_select(0, 0xCF, 2, pairs, &k, &a, &b), "channel 0 is OFF");
        CHECK(!scene_pc_select(1, 0xB0, 2, pairs, &k, &a, &b) && scene_pc_select(1, 0xC0, 0, pairs, &k, &a, &b) && k == 0,
              "only a Program Change, on the channel set");
        CHECK(scene_pc_snapshot(16, 0xCF, 126) == SCENE_PC_SNAPSHOT_TAKE &&
              scene_pc_snapshot(16, 0xCF, 127) == SCENE_PC_SNAPSHOT_RECALL, "PC 126 takes, PC 127 recalls");
        CHECK(!scene_pc_snapshot(16, 0xC0, 127) && !scene_pc_snapshot(0, 0xCF, 127) &&
              !scene_pc_snapshot(16, 0xCF, 4) && !scene_pc_select(16, 0xCF, 127, pairs, &k, &a, &b),
              "... on the scene channel only, and neither is a scene");
    }

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
