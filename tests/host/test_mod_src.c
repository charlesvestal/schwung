/*
 * mod_src.h -- the mod-route source arithmetic, run natively.
 *
 * Everything in that header is pure, which is the point of it existing
 * separately: the mapping from a 7-bit MIDI byte to the bipolar signal the
 * modulation bus expects is testable OFF the device, and the alternative is
 * discovering on hardware that velocity 64 is not centre. Same reasoning as
 * bus_route.h and send_fx_key.h, whose preambles say it first.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "mod_src.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
    else printf("  ok  %s\n", (msg)); \
} while (0)
#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 0.005f)

int main(void) {
    /* ---- zero is an LFO, so no patch on disk needs migrating ----------- */
    CHECK(MOD_SRC_LFO == 0, "MOD_SRC_LFO is 0: a zeroed route is an LFO");

    /* ---- names round-trip, and a bad type is never a NULL -------------- */
    for (int t = 0; t < MOD_SRC_COUNT; t++) {
        const char *n = mod_src_name(t);
        CHECK(n && n[0], "every type has a non-empty name");
        CHECK(mod_src_from_name(n) == t, "the name round-trips to its own type");
    }
    CHECK(strcmp(mod_src_name(-1), "lfo") == 0, "a negative type reads as lfo");
    CHECK(strcmp(mod_src_name(MOD_SRC_COUNT), "lfo") == 0,
          "an out-of-range type reads as lfo");
    CHECK(mod_src_from_name("nonsense") == MOD_SRC_LFO,
          "an unknown name falls back to lfo rather than rejecting");
    CHECK(mod_src_from_name(NULL) == MOD_SRC_LFO, "NULL falls back to lfo");

    /* ---- rest values: an unplayed route must not sit on a rail --------- */
    mod_input_t in;
    mod_input_reset(&in);
    CHECK(in.velocity == 64, "velocity rests at centre, not hard left");
    CHECK(in.note == 64, "note rests at centre");
    CHECK(NEAR(mod_src_signal(MOD_SRC_PRESSURE, &in, 0), -1.0f),
          "pressure rests at the bottom of its span, where a player starts it");
    CHECK(NEAR(mod_src_signal(MOD_SRC_CC, &in, 74), -1.0f),
          "an untouched CC rests at the bottom of its span");
    CHECK(fabsf(mod_src_signal(MOD_SRC_VELOCITY, &in, 0)) < 0.02f,
          "an unplayed velocity route sits at centre, not at -1");

    /* ---- velocity spans the full bipolar range ------------------------- */
    in.velocity = 0;
    CHECK(NEAR(mod_src_signal(MOD_SRC_VELOCITY, &in, 0), -1.0f), "velocity 0 is -1.0");
    in.velocity = 127;
    CHECK(NEAR(mod_src_signal(MOD_SRC_VELOCITY, &in, 0), 1.0f), "velocity 127 is +1.0");
    in.velocity = 64;
    CHECK(fabsf(mod_src_signal(MOD_SRC_VELOCITY, &in, 0)) < 0.02f, "velocity 64 is centre");

    /* ---- pressure and note use the same map --------------------------- */
    in.pressure = 127;
    CHECK(NEAR(mod_src_signal(MOD_SRC_PRESSURE, &in, 0), 1.0f), "pressure 127 is +1.0");
    in.note = 0;
    CHECK(NEAR(mod_src_signal(MOD_SRC_NOTE, &in, 0), -1.0f), "note 0 is -1.0");
    in.note = 127;
    CHECK(NEAR(mod_src_signal(MOD_SRC_NOTE, &in, 0), 1.0f), "note 127 is +1.0");

    /* ---- CC: the route reads the controller number IT asked for -------- */
    mod_input_reset(&in);
    in.cc[74] = 127;
    in.cc[1] = 64;
    CHECK(NEAR(mod_src_signal(MOD_SRC_CC, &in, 74), 1.0f),
          "the route reads the CC number it asked for");
    CHECK(fabsf(mod_src_signal(MOD_SRC_CC, &in, 1)) < 0.02f,
          "a different CC number reads its own value, not the neighbour's");

    /*
     * OUT-OF-RANGE MUST BE PINNED TO THE CLAMPED VALUE, not to a plausible one.
     *
     * The first version of these two asserted that cc_num 200 and -5 both read
     * -1.0. Both passed with the clamp DELETED: cc[] is the last member of
     * mod_input_t, so cc[200] is an out-of-bounds read that happened to find a
     * zero, which maps to exactly the -1.0 the test wanted. A green light for
     * undefined behaviour.
     *
     * So the rails are loaded with a value the clamp can be SEEN to reach:
     * cc[127] and cc[0] both hold 127, so a clamped read returns +1.0 while an
     * unclamped one returns whatever is past the struct. The assertion now
     * distinguishes the two implementations instead of agreeing with both.
     */
    in.cc[127] = 127;
    in.cc[0] = 127;
    CHECK(NEAR(mod_src_signal(MOD_SRC_CC, &in, 200), 1.0f),
          "a too-large CC number clamps to 127 and reads cc[127]");
    CHECK(NEAR(mod_src_signal(MOD_SRC_CC, &in, -5), 1.0f),
          "a negative CC number clamps to 0 and reads cc[0]");

    /* ---- silence, not garbage, for the cases this header does not own -- */
    CHECK(mod_src_signal(MOD_SRC_COUNT, &in, 0) == 0.0f,
          "an out-of-range type contributes nothing");
    CHECK(mod_src_signal(-1, &in, 0) == 0.0f, "a negative type contributes nothing");
    CHECK(mod_src_signal(MOD_SRC_VELOCITY, NULL, 0) == 0.0f,
          "a NULL input contributes nothing");
    CHECK(mod_src_signal(MOD_SRC_LFO, &in, 0) == 0.0f,
          "LFO is 0 here: its signal comes from lfo_compute_shape, which owns phase");

    /* ---- slew ---------------------------------------------------------- */
    float v = -1.0f;
    for (int i = 0; i < 2000; i++) v = mod_src_slew(v, 1.0f, 0.05f);
    CHECK(v == 1.0f,
          "slew converges EXACTLY on its target, so a settled source stops writing");
    CHECK(mod_src_slew(1.0f, 1.0f, 0.05f) == 1.0f, "slew at the target is idempotent");
    CHECK(mod_src_slew(-1.0f, 1.0f, 0.0f) == 1.0f,
          "a coefficient of 0 means no slew at all, i.e. jump to the target");
    CHECK(mod_src_slew(0.0f, 1.0f, 1.0f) == 0.0f,
          "a coefficient of 1 means never arrive");
    CHECK(mod_src_slew(0.0f, 1.0f, -3.0f) == 1.0f, "a negative coefficient clamps to jump");
    CHECK(mod_src_slew(0.0f, 1.0f, 7.0f) == 0.0f, "a coefficient above 1 clamps to never");
    /* Direction-symmetric: falling must behave like rising. */
    CHECK(NEAR(mod_src_slew(1.0f, 0.0f, 0.5f), 0.5f), "slew moves down by the same fraction");
    CHECK(NEAR(mod_src_slew(0.0f, 1.0f, 0.5f), 0.5f), "slew moves up by the same fraction");

    /* ---- the LFO predicate -------------------------------------------- */
    CHECK(mod_src_is_lfo(MOD_SRC_LFO) == 1, "LFO is flagged as the LFO type");
    CHECK(mod_src_is_lfo(MOD_SRC_VELOCITY) == 0, "velocity is not the LFO type");
    CHECK(mod_src_is_lfo(MOD_SRC_CC) == 0, "cc is not the LFO type");

    printf(failures ? "FAIL\n" : "ALL PASS\n");
    return failures ? 1 : 0;
}
