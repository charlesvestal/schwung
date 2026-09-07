/*
 * mod_src.h — what a mod route's SOURCE is, and the arithmetic that turns it
 * into the signal the modulation bus already knows how to spend.
 *
 * Header-only and dependency-free, like bus_route.h and send_fx_key.h, so
 * tests/host can compile and RUN it natively. Their preambles carry the reason:
 * scaling code that can only be built on the device is exactly the code that
 * ships untested.
 *
 * THE OUTPUT CONTRACT IS lfo_compute_shape's: bipolar -1..+1. That is what makes
 * a MIDI source a drop-in replacement for an LFO. chain_mod_emit_value already
 * folds a unipolar route by (signal + 1) * 0.5 and scales by the target's
 * declared range, so a source returning 0..1 here would be half-scaled twice —
 * quietly wrong over half the range rather than obviously wrong anywhere.
 *
 * MOD_SRC_LFO IS 0 ON PURPOSE. Every route in every patch on disk predates this
 * field, so it parses as absent and memsets to zero — and zero must mean "the
 * LFO it has always been". There is no migration step anywhere in this feature
 * because of that one line, and there is no version stamp to get wrong.
 *
 * Pure: no allocation, no I/O, no locks. Called from render_block and from the
 * param handler, i.e. from the SPI callback.
 */
#ifndef MOD_SRC_H
#define MOD_SRC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ============================================================================
 * Source types
 * ========================================================================= */

#define MOD_SRC_LFO       0
#define MOD_SRC_VELOCITY  1
#define MOD_SRC_PRESSURE  2
#define MOD_SRC_CC        3
#define MOD_SRC_NOTE      4
#define MOD_SRC_COUNT     5

/*
 * The WIRE names. Stored in the patch file and spoken by the param keys, so
 * they are part of the format: renaming one silently resets that route to LFO
 * on the next load, because mod_src_from_name falls back rather than failing.
 * That fallback is the right behaviour for an unknown name from a newer build,
 * and it is precisely what makes a rename here a data-loss bug.
 *
 * The UI's display words live in shadow_ui_slot_grid.mjs and may differ; these
 * are not shown to anyone.
 */
static const char *const mod_src_names[MOD_SRC_COUNT] = {
    "lfo", "velocity", "pressure", "cc", "note"
};

/* An out-of-range type reads as "lfo" rather than NULL. Every caller of this is
 * building a string to answer a param read, on the SPI callback; a NULL there
 * is a crash in exchange for nothing, and there is no caller that could do
 * something useful with one. */
static inline const char *mod_src_name(int type) {
    if (type < 0 || type >= MOD_SRC_COUNT) return mod_src_names[MOD_SRC_LFO];
    return mod_src_names[type];
}

static inline int mod_src_from_name(const char *name) {
    if (!name) return MOD_SRC_LFO;
    for (int i = 0; i < MOD_SRC_COUNT; i++) {
        if (strcmp(name, mod_src_names[i]) == 0) return i;
    }
    return MOD_SRC_LFO;
}

static inline int mod_src_is_lfo(int type) {
    return type == MOD_SRC_LFO;
}

/* ============================================================================
 * Latched MIDI state
 * ========================================================================= */

/*
 * What the slot last received, latched. One of these per chain instance.
 *
 * NOT PER VOICE, and that is structural rather than a shortcut. The modulation
 * bus moves a PARAMETER, through set_param, and a parameter belongs to the
 * plugin rather than to a note — so velocity here is last-note velocity and
 * pressure is channel pressure, exactly as an outboard mod matrix behaves. Poly
 * velocity has to live inside the synth; it cannot be expressed across the
 * plugin boundary, and pretending otherwise here would ship a feature that
 * sounds broken rather than one that is honestly absent.
 *
 * REST VALUES ARE NOT ZERO, which is the whole reason mod_input_reset exists
 * rather than a memset. A velocity route aimed at filter cutoff, on a slot
 * nobody has played yet, must not sit at the bottom of its range: the user hears
 * a dead synth and blames the target, not the unplayed source. Velocity and note
 * rest at CENTRE. Pressure and CC, which a player genuinely starts at zero, rest
 * at the BOTTOM — resting those at centre would be the same mistake inverted,
 * offsetting a parameter by half its range until the first touch moved it.
 */
typedef struct {
    uint8_t velocity;   /* last note-on velocity, 0..127 */
    uint8_t pressure;   /* channel aftertouch, 0..127 */
    uint8_t note;       /* last note-on number, 0..127 */
    uint8_t cc[128];    /* last value seen per controller */
} mod_input_t;

#define MOD_SRC_VELOCITY_REST 64
#define MOD_SRC_PRESSURE_REST 0
#define MOD_SRC_NOTE_REST     64
#define MOD_SRC_CC_REST       0

static inline void mod_input_reset(mod_input_t *in) {
    if (!in) return;
    memset(in, 0, sizeof(*in));
    in->velocity = MOD_SRC_VELOCITY_REST;
    in->pressure = MOD_SRC_PRESSURE_REST;
    in->note     = MOD_SRC_NOTE_REST;
    for (int i = 0; i < 128; i++) in->cc[i] = MOD_SRC_CC_REST;
}

/* ============================================================================
 * The mapping
 * ========================================================================= */

/* 7-bit byte -> bipolar. 63.5 rather than 63 or 64 so that 0 and 127 both land
 * exactly on the rails; the midpoint then falls between 63 and 64, which is what
 * a symmetric 128-step span actually is. Dividing by 64 would leave 127 at
 * +0.984 — inaudible on a filter, and a visible gap on anything that draws the
 * value's position in its range. */
static inline float mod_src_7bit_bipolar(uint8_t v) {
    return ((float)v / 63.5f) - 1.0f;
}

/*
 * The route's signal, bipolar -1..+1.
 *
 * cc_num is read only for MOD_SRC_CC, and is CLAMPED rather than trusted: it
 * arrives from a patch file and from a knob, and this runs on the SPI callback.
 *
 * MOD_SRC_LFO RETURNS 0 HERE BY DESIGN. An LFO's signal comes from
 * lfo_compute_shape, which owns the phase accumulator; computing it here as well
 * would give a route two phases advancing at different times. The caller
 * branches on mod_src_is_lfo() — see chain_host.c's mod_tick.
 */
static inline float mod_src_signal(int type, const mod_input_t *in, int cc_num) {
    if (!in) return 0.0f;
    switch (type) {
    case MOD_SRC_VELOCITY: return mod_src_7bit_bipolar(in->velocity);
    case MOD_SRC_PRESSURE: return mod_src_7bit_bipolar(in->pressure);
    case MOD_SRC_NOTE:     return mod_src_7bit_bipolar(in->note);
    case MOD_SRC_CC:
        if (cc_num < 0) cc_num = 0;
        if (cc_num > 127) cc_num = 127;
        return mod_src_7bit_bipolar(in->cc[cc_num]);
    default:               return 0.0f;   /* LFO, and anything unknown */
    }
}

/*
 * One-pole slew toward a target, per block.
 *
 * `coeff` is the fraction of the remaining distance KEPT each block: 0 jumps (no
 * slew at all) and 1 never arrives. Expressed that way round because the UI
 * offers a "Slew" amount where more means slower; inverting it here keeps the
 * one place that knows the direction next to the one place that documents it.
 *
 * IT MUST SETTLE ON THE TARGET EXACTLY, which the form below already does: once
 * the difference underflows, `target + 0 * coeff` is bit-exactly `target` and
 * stays there. That matters because chain_mod_apply_effective_value only skips a
 * write when the value has not moved by MOD_FLOAT_CHANGE_EPSILON — a slew that
 * merely approached its target would emit a set_param string write on every
 * block, for every route, forever, on the SPI callback.
 *
 * There was an `if (current == target) return current;` guard here for that. It
 * is deleted: the arithmetic gives the same answer, so the branch was
 * unreachable-by-observation, and a mutation check found that removing it failed
 * no assertion. A line no test can see is a claim, not a safeguard.
 */
static inline float mod_src_slew(float current, float target, float coeff) {
    if (coeff <= 0.0f) return target;
    if (coeff >= 1.0f) return current;
    return target + (current - target) * coeff;
}

#endif /* MOD_SRC_H */
