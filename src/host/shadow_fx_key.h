/*
 * Recognises an FX module-slot write: "fx<N>:module" / "midi_fx<N>:module".
 *
 * Header-only and dependency-free on purpose, for the same reason as
 * chain_key_index.h: so tests/host can compile and RUN it natively
 * (tests/host/test_shadow_fx_key.c) instead of only grepping for it. Its one
 * caller lives in shadow_chain_mgmt.c, a shim translation unit with a large
 * dependency web that cannot be compiled on the dev machine — which is exactly
 * how this function would otherwise end up untested.
 *
 * It is load-bearing for whether a chain slot makes sound. A slot activates
 * when a module lands in it, and this decides whether a given param write was
 * such a landing. Say yes too readily and unrelated writes mark empty slots
 * active; say no and the slot stays silent with a module loaded in it, which
 * is the bug this whole area exists to fix.
 */
#ifndef SHADOW_FX_KEY_H
#define SHADOW_FX_KEY_H

#include <string.h>

/*
 * Deliberately UNBOUNDED in N — there is no cap parameter here, and that is
 * not an oversight.
 *
 * Bounding it would mean naming MAX_AUDIO_FX / MAX_MIDI_FX somewhere under
 * src/host/, and those live in src/modules/chain/dsp/chain_internal.h with no
 * include path between the two. A copy of a cap is precisely the bug class
 * that made fx3..fx8 silent in the first place: the caps moved from 2 to 8 and
 * the shim's hand-written key lists did not.
 *
 * It does not need bounding, because matching the shape is not the decision.
 * The caller confirms every match against the chain DSP's own reported list
 * lengths (shadow_slot_has_loaded_component), and the DSP rejects an
 * out-of-range index outright. So "fx99:module" matches here, loads nothing
 * there, and correctly leaves the slot inactive. The shape test is a cheap
 * filter in front of an authoritative one, not a substitute for it.
 *
 * Note that "midi_fx" is tested BEFORE "fx" but the two cannot be confused
 * anyway: the prefix must match from the start, so "midi_fx1:module" never
 * matches bare "fx". Order here is readability, not correctness.
 *
 * Leading zeros are rejected ("fx01:module" is not an id we emit), matching
 * chain_key_index.h. No digit accumulation happens at all, so an absurdly long
 * run of digits is merely rejected downstream rather than overflowing here.
 *
 * "synth:module" deliberately does NOT match. The synth has its own activation
 * branch in the caller, which additionally adopts the module's declared
 * default forward channel; folding it in here would silently drop that.
 */
static inline int shadow_key_is_fx_module(const char *key)
{
    if (!key) return 0;
    const char *p = key;
    if (strncmp(p, "midi_fx", 7) == 0) p += 7;
    else if (strncmp(p, "fx", 2) == 0) p += 2;
    else return 0;
    if (*p < '1' || *p > '9') return 0;   /* at least one digit, no leading 0 */
    while (*p >= '0' && *p <= '9') p++;
    return strcmp(p, ":module") == 0;
}

#endif /* SHADOW_FX_KEY_H */
