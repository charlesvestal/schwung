/*
 * test_voice_send_source.c — the rules that turn a MODULE's own parameters into
 * the host's 0..127 per-voice send levels.
 *
 * All of it lives in a header for exactly this reason: its only caller is
 * chain_bus.c, a translation unit the dev machine cannot build, and arithmetic
 * that ships there is arithmetic nobody runs. What is pinned here is the
 * declaration's shape (position IS the send index, and one entry too many is an
 * ERROR), the substitution, and the range mapping — including the two ways it
 * REFUSES, because a refusal is the whole reason the host never guesses a
 * scale.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "voice_send_source.h"

#define T VOICE_SEND_TMPL_LEN

static void test_declaration_shape(void)
{
    char t[BUS_MIX_SENDS][T];

    /* THE REFERENCE DECLARATION. Position is the send index: entry 0 is Send A
     * and entry 1 is Send B, never compacted and never reordered. */
    memset(t, 0, sizeof(t));
    assert(voice_send_params_parse("[\"{id}_send1\",\"{id}_send2\"]",
                                   t, BUS_MIX_SENDS, T) == 2);
    assert(strcmp(t[0], "{id}_send1") == 0);
    assert(strcmp(t[1], "{id}_send2") == 0);

    /* A SHORTER ARRAY IS A REAL ANSWER: this module has one send. */
    memset(t, 0, sizeof(t));
    assert(voice_send_params_parse("[\"{id}_verb\"]", t, BUS_MIX_SENDS, T) == 1);
    assert(strcmp(t[0], "{id}_verb") == 0);

    /* Declaring none, in both spellings a module can say it. */
    assert(voice_send_params_parse("[]", t, BUS_MIX_SENDS, T) == 0);
    assert(voice_send_params_parse("", t, BUS_MIX_SENDS, T) == 0);

    /* ONE TOO MANY IS AN ERROR, NOT A TRUNCATION. A module that believes it
     * declared three sends while the host quietly kept two has a control
     * writing into nothing, with nothing on screen to say so. */
    assert(voice_send_params_parse("[\"{id}_a\",\"{id}_b\",\"{id}_c\"]",
                                   t, BUS_MIX_SENDS, T) == VOICE_SEND_PARAMS_INVALID);

    /* NO "{id}" is one key for every voice — every pad writing the same level,
     * which would look like a working feature. Refused whole, so a second
     * well-formed entry does not survive beside it. */
    assert(voice_send_params_parse("[\"send1\"]", t, BUS_MIX_SENDS, T)
           == VOICE_SEND_PARAMS_INVALID);
    assert(voice_send_params_parse("[\"{id}_send1\",\"send2\"]", t, BUS_MIX_SENDS, T)
           == VOICE_SEND_PARAMS_INVALID);

    /* An entry that does not fit is refused rather than truncated: a truncated
     * key addresses a parameter that is not the one declared. */
    char longer[T + 16];
    memset(longer, 'x', sizeof(longer));
    longer[sizeof(longer) - 1] = '\0';
    char decl[T + 64];
    snprintf(decl, sizeof(decl), "[\"{id}%s\"]", longer);
    assert(voice_send_params_parse(decl, t, BUS_MIX_SENDS, T)
           == VOICE_SEND_PARAMS_INVALID);

    /* Unterminated, and NULL. NULL cannot arise on the in-process call this
     * serves; it is invalid rather than 0 so that a caller reading through the
     * SHM channel cannot turn a timed-out read into "declares none". */
    assert(voice_send_params_parse("[\"{id}_send1", t, BUS_MIX_SENDS, T)
           == VOICE_SEND_PARAMS_INVALID);
    assert(voice_send_params_parse(NULL, t, BUS_MIX_SENDS, T)
           == VOICE_SEND_PARAMS_INVALID);

    printf("  declaration shape: ok\n");
}

static void test_substitution(void)
{
    char k[VOICE_SEND_KEY_LEN];

    assert(voice_send_key_build("{id}_send1", "pad7", k, sizeof(k)) == 1);
    assert(strcmp(k, "pad7_send1") == 0);
    /* VERBATIM. The host does not know how a module numbers its own params and
     * must never adjust the id it was given — a host that "corrected" an
     * off-by-one would be unpredictable for every other module. */
    assert(voice_send_key_build("{id}_send1", "bd", k, sizeof(k)) == 1);
    assert(strcmp(k, "bd_send1") == 0);
    /* The token need not be at the front. */
    assert(voice_send_key_build("send1_{id}", "pad7", k, sizeof(k)) == 1);
    assert(strcmp(k, "send1_pad7") == 0);
    /* Only the FIRST occurrence: a second is left literal so the key misses
     * loudly rather than addressing something. */
    assert(voice_send_key_build("{id}_{id}", "p", k, sizeof(k)) == 1);
    assert(strcmp(k, "p_{id}") == 0);
    /* No token, and no room. */
    assert(voice_send_key_build("send1", "pad7", k, sizeof(k)) == 0);
    {
        char tiny[6];
        assert(voice_send_key_build("{id}_send1", "pad7", tiny, sizeof(tiny)) == 0);
    }
    printf("  substitution: ok\n");
}

static void test_meta_key(void)
{
    char m[VOICE_SEND_KEY_LEN];

    /* THE FOCUS-ADDRESSED SPELLING. A per-voice parameter is authored ONCE on
     * the child level of a ui_hierarchy and addressed by focus — 32 voices x N
     * params would not fit chain_params — so this is the spelling the range
     * actually has to be looked up under. */
    assert(voice_send_meta_key("{id}_send1", m, sizeof(m)) == 1);
    assert(strcmp(m, "send1") == 0);
    /* The separator that goes is the one AFTER the token, or the one before it
     * when the token is at the end. */
    assert(voice_send_meta_key("send1_{id}", m, sizeof(m)) == 1);
    assert(strcmp(m, "send1") == 0);
    assert(voice_send_meta_key("pad.{id}.send1", m, sizeof(m)) == 1);
    assert(strcmp(m, "pad.send1") == 0);
    /* Nothing but the token leaves nothing to look up. */
    assert(voice_send_meta_key("{id}", m, sizeof(m)) == 0);
    assert(voice_send_meta_key("send1", m, sizeof(m)) == 0);
    printf("  metadata key: ok\n");
}

static void test_suffix_match(void)
{
    /* What arms a re-read when a write goes past on its way to the module.
     * Deliberately LOOSE: it also catches dr32's focus alias `pad_send1`, which
     * names a pad the host cannot resolve and which the substituted keys
     * therefore cannot match. The cost of a false positive is one extra sweep;
     * the sweep asks the module, so it can never be a wrong level. */
    assert(voice_send_key_has_suffix("{id}_send1", "pad7_send1") == 1);
    assert(voice_send_key_has_suffix("{id}_send1", "pad_send1") == 1);
    assert(voice_send_key_has_suffix("{id}_send1", "pad7_send2") == 0);
    assert(voice_send_key_has_suffix("{id}_send1", "cutoff") == 0);
    /* "{id}" alone would match every key the module has. Refused. */
    assert(voice_send_key_has_suffix("{id}", "cutoff") == 0);
    assert(voice_send_key_has_suffix("{id}_send1", NULL) == 0);
    printf("  suffix match: ok\n");
}

static void test_range_mapping(void)
{
    int lvl = -1;

    /* LINEAR, the default law. */
    assert(voice_send_level_map(0.0, 0.0, 1.0, 0, &lvl) == 1 && lvl == 0);
    assert(voice_send_level_map(1.0, 0.0, 1.0, 0, &lvl) == 1
           && lvl == BUS_MIX_SEND_LEVEL_MAX);
    assert(voice_send_level_map(0.5, 0.0, 1.0, 0, &lvl) == 1 && lvl == 64);
    /* A module ALREADY in 0..127 is not a special case, it is a range. */
    assert(voice_send_level_map(127.0, 0.0, 127.0, 0, &lvl) == 1 && lvl == 127);
    assert(voice_send_level_map(64.0, 0.0, 127.0, 0, &lvl) == 1 && lvl == 64);
    /* A NEGATIVE-FLOORED linear range: -1..1 with 0 in the middle. */
    assert(voice_send_level_map(0.0, -1.0, 1.0, 0, &lvl) == 1 && lvl == 64);

    /* dB. 0 dB is EXACTLY unity — which is the reason this law exists at all:
     * mapping dr32's -70..+6 linearly would put 0 dB at 118, a control marked
     * unity that is not. */
    assert(voice_send_level_map(0.0, -70.0, 6.0, 1, &lvl) == 1
           && lvl == BUS_MIX_SEND_LEVEL_MAX);
    /* -6 dB is (very nearly) half the voltage. */
    assert(voice_send_level_map(-6.0, -70.0, 6.0, 1, &lvl) == 1 && lvl == 64);
    /* -20 dB is a tenth. */
    assert(voice_send_level_map(-20.0, -70.0, 6.0, 1, &lvl) == 1 && lvl == 13);
    /* Boost is CLAMPED, not wrapped: bus_mix_send cannot exceed unity. */
    assert(voice_send_level_map(6.0, -70.0, 6.0, 1, &lvl) == 1
           && lvl == BUS_MIX_SEND_LEVEL_MAX);

    /* AT OR BELOW THE FLOOR IS EXACTLY OFF, whatever floor the module chose.
     * Without the explicit test, a -40 dB floor would map to 1 — a control at
     * its own minimum still sending. */
    assert(voice_send_level_map(-70.0, -70.0, 6.0, 1, &lvl) == 1 && lvl == 0);
    assert(voice_send_level_map(-99.0, -70.0, 6.0, 1, &lvl) == 1 && lvl == 0);
    assert(voice_send_level_map(-40.0, -40.0, 6.0, 1, &lvl) == 1 && lvl == 0);
    assert(voice_send_level_map(-1.0, 0.0, 1.0, 0, &lvl) == 1 && lvl == 0);

    /* THE REFUSAL. A range that cannot carry a mapping leaves the level ALONE
     * — the host does not pick a scale. Assuming 0..127 mis-scales a 0..1
     * module by two orders of magnitude; assuming 0..1 mutes a 0..127 one. */
    lvl = 55;
    assert(voice_send_level_map(0.5, 1.0, 1.0, 0, &lvl) == 0 && lvl == 55);
    assert(voice_send_level_map(0.5, 1.0, 0.0, 0, &lvl) == 0 && lvl == 55);
    assert(voice_send_level_map(0.5, 1.0, 1.0, 1, &lvl) == 0 && lvl == 55);

    printf("  range mapping: ok\n");
}

static void test_unit_is_db(void)
{
    assert(voice_send_unit_is_db("dB") == 1);
    assert(voice_send_unit_is_db("db") == 1);
    assert(voice_send_unit_is_db("DB") == 1);
    /* Not a prefix match: "dBFS" is a different unit and would take the wrong
     * law silently. */
    assert(voice_send_unit_is_db("dBFS") == 0);
    assert(voice_send_unit_is_db("%") == 0);
    assert(voice_send_unit_is_db("") == 0);
    assert(voice_send_unit_is_db(NULL) == 0);
    printf("  unit: ok\n");
}

int main(void)
{
    printf("test_voice_send_source (sends=%d, max=%d):\n",
           BUS_MIX_SENDS, BUS_MIX_SEND_LEVEL_MAX);
    test_declaration_shape();
    test_substitution();
    test_meta_key();
    test_suffix_match();
    test_range_mapping();
    test_unit_is_db();
    printf("PASS\n");
    return 0;
}
