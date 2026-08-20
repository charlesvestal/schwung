/*
 * Tests for shadow_fx_key.h — the "is this param write a module landing in an
 * FX slot?" test that gates chain slot activation in shadow_chain_mgmt.c.
 *
 * This decides whether a slot makes sound. The shim marks a slot active when a
 * module lands in it; if this says no for "fx5:module" the slot stays silent
 * with a module loaded in it, which is exactly the bug it was written to fix.
 * If it says yes too readily, unrelated writes mark empty slots active.
 *
 * The near-miss cases below are the point of the file. A future "simplification"
 * — matching on strstr(key, "fx"), or dropping the ":module" suffix check, or
 * bounding N by a hard-coded cap — passes the obvious cases and breaks these.
 */
#include <stdio.h>
#include "shadow_fx_key.h"

/*
 * The caps come in on the command line, read out of chain_internal.h by the
 * shell wrapper. The parser is deliberately cap-AGNOSTIC, so these are not
 * bounds to enforce — they are used to prove that whatever the shipped caps
 * are, every position within them is recognised. If the caps rise to 16, this
 * test widens automatically instead of quietly covering half the range.
 */
#ifndef TEST_MAX_FX
#error "TEST_MAX_FX must be defined by the test wrapper"
#endif
#ifndef TEST_MAX_MIDI_FX
#error "TEST_MAX_MIDI_FX must be defined by the test wrapper"
#endif

static int failures = 0;
static int checks = 0;

static void expect(const char *key, int want) {
    int got = shadow_key_is_fx_module(key);
    checks++;
    if (got != want) {
        fprintf(stderr, "FAIL \"%s\": got %d, want %d\n", key, got, want);
        failures++;
    }
}

int main(void) {
    char key[64];

    /* Every position the shipped caps actually allow, both families. The
     * original bug was that only positions 1 and 2 were recognised. */
    for (int i = 1; i <= TEST_MAX_FX; i++) {
        snprintf(key, sizeof(key), "fx%d:module", i);
        expect(key, 1);
    }
    for (int i = 1; i <= TEST_MAX_MIDI_FX; i++) {
        snprintf(key, sizeof(key), "midi_fx%d:module", i);
        expect(key, 1);
    }

    /* Cap-agnostic by design: an index past the cap still matches the SHAPE.
     * The caller confirms against the DSP's own list lengths, and the DSP
     * refuses to load out of range, so the slot correctly stays inactive.
     * Encoding a cap here instead would reintroduce the duplicated constant
     * that made fx3..fx8 silent. */
    expect("fx99:module", 1);
    expect("midi_fx12:module", 1);

    /* The synth has its own activation branch — one that also adopts the
     * module's default forward channel. Matching it here would drop that. */
    expect("synth:module", 0);

    /* Master FX is a separate, deliberately-4-slot structure with its own
     * routing. Its keys must not reach chain slot activation. */
    expect("master_fx:fx1:module", 0);
    expect("master_fx:fx3:module", 0);

    /* Right slot, wrong subkey: these are ordinary param writes, not module
     * landings, and must not activate an empty slot. */
    expect("fx1:bypassed", 0);
    expect("fx5:cutoff", 0);
    expect("midi_fx1:state", 0);
    expect("fx1:module:extra", 0);

    /* Missing or malformed index. "fx0" is not an id we emit (keys are
     * 1-based), and leading zeros are rejected to match chain_key_index.h. */
    expect("fx:module", 0);
    expect("fx0:module", 0);
    expect("midi_fx0:module", 0);
    expect("fx01:module", 0);
    expect("midi_fx:module", 0);
    expect("fxa:module", 0);
    expect("fx-1:module", 0);

    /* Truncations and near-spellings. */
    expect("fx1module", 0);
    expect("fx1:", 0);
    expect("fx1", 0);
    expect("fx", 0);
    expect("midi_fx", 0);
    expect("", 0);
    expect(NULL, 0);

    /* A substring match anywhere in the key must not count — the prefix has to
     * match from the start. This is what breaks if someone reaches for strstr. */
    expect("slot_fx1:module", 0);
    expect("xfx1:module", 0);

    if (failures) {
        fprintf(stderr, "%d/%d case(s) FAILED\n", failures, checks);
        return 1;
    }
    printf("test_shadow_fx_key: %d cases pass (caps %d audio / %d midi)\n",
           checks, TEST_MAX_FX, TEST_MAX_MIDI_FX);
    return 0;
}
