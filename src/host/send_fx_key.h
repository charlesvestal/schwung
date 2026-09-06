/*
 * send_fx_key.h — "send<N>:fx<M>:<param>" and "send<N>:<param>" routing.
 *
 * Header-only and dependency-free, like master_fx_key.h and bus_route.h, so
 * tests/host can compile and RUN it natively (tests/host/test_send_fx_key.c).
 * Its callers live in shadow_chain_mgmt.c, a shim translation unit that cannot
 * be built on the dev machine — which is exactly how routing like this ends up
 * shipped untested.
 *
 * The caps are PARAMETERS. SEND_BUSES and SEND_FX_SLOTS are named once, in
 * shadow_chain_mgmt.h, and this file holds no copy of either. That discipline
 * is inherited from master_fx_key.h, whose preamble is the war story: every
 * such route used to be a hand-written strncmp ladder that restated the cap
 * without naming it, a cap raise broke seven sibling sites silently, and one
 * else-branch ASSIGNED SLOT 0 — so an out-of-range key was not dropped but
 * routed into a different running module under a garbage param name.
 *
 * Hence: an unmatched key returns 0 with the out-params UNTOUCHED. It never
 * defaults a send or a slot, so a caller may pre-seed its own fallback and
 * know a rejection left it alone.
 *
 * Pure: no allocation, no I/O, no locks. Called from the param handler, which
 * runs on the SCHED_FIFO SPI callback.
 *
 * Design credit: PR #121 (legsmechanical), which established the send topology
 * these keys address.
 */
#ifndef SEND_FX_KEY_H
#define SEND_FX_KEY_H

#include <stddef.h>
#include "master_fx_key.h"   /* master_fx_parse_index, for the "fx<M>" half */

/* Buffer for a formatted "send%d" key or LFO target. Matches
 * lfo_state_t.target (char[16]) for the same reason MASTER_FX_TARGET_KEY_LEN
 * does: a truncated target compares unequal and silently stops modulating. */
#define SEND_TARGET_KEY_LEN 16

/*
 * Parse a leading "send<N>" with N a 1-based decimal index.
 *
 * Returns N (>= 1) and points *out_end at the first byte after the digits;
 * returns -1 on no match, leaving *out_end untouched. Leading zeros are
 * rejected ("send01" is not an id we emit), matching bus_route.h and
 * chain_key_index.h. Accumulation is clamped so a long digit run cannot
 * overflow into a plausible-looking index; anything absurd is simply out of
 * range for the caller's send_count.
 */
static inline int send_fx_parse_index(const char *key, const char **out_end)
{
    if (!key) return -1;
    if (key[0] != 's' || key[1] != 'e' || key[2] != 'n' || key[3] != 'd') return -1;
    const char *p = key + 4;
    if (*p < '1' || *p > '9') return -1;   /* at least one digit, no leading 0 */
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (n < 100000) n = n * 10 + (*p - '0');
        p++;
    }
    if (out_end) *out_end = p;
    return n;
}

/*
 * Route a send key.
 *
 *   "send1:fx3:cutoff" -> send 0, slot 2, param "cutoff"
 *   "send1:return"     -> send 0, slot -1, param "return"   (a bus-level key)
 *
 * Returns 1 on a match, 0 otherwise. On 0 NOTHING is written — not the send,
 * not the slot, not the param.
 *
 * slot -1 is the bus itself (its return level, its routing), and is the reason
 * a caller must branch on the slot before indexing the FX array rather than
 * treating a match as "this names an FX".
 */
static inline int send_fx_route(const char *key, int send_count, int slot_count,
                                int *out_send, int *out_slot, const char **out_param)
{
    const char *end = NULL;
    int n = send_fx_parse_index(key, &end);
    if (n < 1 || n > send_count) return 0;
    if (!end || *end != ':') return 0;     /* "send1" and "send1x:" name nothing */
    const char *rest = end + 1;

    const char *fx_end = NULL;
    int f = master_fx_parse_index(rest, &fx_end);
    if (f >= 1) {
        if (f > slot_count) return 0;      /* past the cap: reject, never clamp */
        if (!fx_end || *fx_end != ':') return 0;
        if (out_send)  *out_send  = n - 1;
        if (out_slot)  *out_slot  = f - 1;
        if (out_param) *out_param = fx_end + 1;
        return 1;
    }

    /* Anything fx-SHAPED that did not parse as an in-range position is
     * rejected here rather than handed back as a bus-level param name.
     * "fx0:" and "fx01:" fail master_fx_parse_index's leading-zero rule, and
     * without this line they would arrive at the bus level as a param
     * literally called "fx0:cutoff". No bus-level key begins with "fx". */
    if (rest[0] == 'f' && rest[1] == 'x') return 0;

    if (*rest == '\0') return 0;           /* "send1:" names nothing */
    if (out_send)  *out_send  = n - 1;
    if (out_slot)  *out_slot  = -1;
    if (out_param) *out_param = rest;
    return 1;
}

/*
 * ================= THE RETURN LEVEL A NEW SEND BUS GETS =====================
 *
 * A send bus has TWO levels between a voice and the speaker: how much is sent
 * (per bus, per voice) and how much comes back (this one). Both default to
 * zero, and the second one has no row on any screen the picker walks through
 * on the way to loading an effect -- it lives on the send editor's Settings
 * box, one box further along. So the whole gesture "put a reverb on Send A and
 * turn a send up" produced SILENCE, with nothing on screen saying a second
 * control existed. That was reported off hardware: send_levels.json read
 * `send1_return: 0` with a reverb loaded and a voice's send raised.
 *
 * A send with an effect in it and its return at zero is never what anyone
 * wants, so loading the FIRST effect into an EMPTY send bus opens the return.
 *
 * THE VALUE IS UNITY (BUS_MIX_SEND_LEVEL_MAX), not a cautious fraction. The
 * return is not a "how loud is the reverb" control in its own right -- the
 * SEND levels are, and they are still zero, so nothing becomes audible until
 * the user asks for it. A return below unity would only make every send level
 * mean less than it says, i.e. it would move the mis-calibration rather than
 * remove it. This follows Master FX's MIDI-channel default (All), chosen the
 * same way: a defaults question settled by asking which wrong answer is
 * SILENT, because a silent wrong answer is the one nobody can diagnose.
 *
 * THREE THINGS IT MUST NOT DO, which is why it is a function and not a literal
 * at the call site:
 *
 *  - not stomp a return the user deliberately set. `current_return != 0` is
 *    kept, whatever it is.
 *  - not fire again for the SECOND effect in the same send. That is what
 *    `bus_was_empty` is: the whole BUS, not this position -- loading into
 *    position 3 while position 1 holds a delay is not an empty send, and a
 *    user who pulled the return down after loading the delay must not have it
 *    pushed back up.
 *  - not fight persistence. Both restore paths write the levels AFTER the
 *    modules (C: shadow_send_levels_restore; JS: loadSendFxChainConfigForSet),
 *    so a stored return -- including a stored, deliberate 0 -- lands last and
 *    wins. This only decides what an unwritten one is.
 *
 * Returns the level the caller should now hold, so `= send_return_level_on_load(...)`
 * is total: there is no "leave it alone" sentinel to get wrong.
 */
#define SEND_RETURN_DEFAULT_ON_FIRST_LOAD 127

static inline int send_return_level_on_load(int bus_was_empty, int current_return)
{
    if (!bus_was_empty) return current_return;
    if (current_return != 0) return current_return;
    return SEND_RETURN_DEFAULT_ON_FIRST_LOAD;
}

#endif /* SEND_FX_KEY_H */
