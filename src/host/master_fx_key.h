/*
 * Master FX slot key routing: "fx<N>:<param>" writes and "fx<N>" LFO targets.
 *
 * Header-only and dependency-free for the same reason as shadow_fx_key.h and
 * chain_key_index.h: so tests/host can compile and RUN it natively
 * (tests/host/test_master_fx_slot_routing.c). Its callers all live in
 * shadow_chain_mgmt.c, a shim translation unit with a dependency web that
 * cannot be built on the dev machine — which is exactly how routing like this
 * ends up shipped untested.
 *
 * WHY THIS EXISTS AT ALL: every one of these routes used to be a hand-written
 * ladder of strncmp("fx1:")..strncmp("fx4:"), or a character-range test on
 * target[2] between '1' and '4'. Each ladder restated the cap without naming
 * it, so a grep for MASTER_FX_SLOTS — or for "<= 4" — found none of them. The
 * chain side of this codebase raised its cap once and broke seven such sibling
 * sites, every one of them SILENTLY. The failure modes here were worse than
 * "does nothing":
 *
 *   - shadow_direct_set_param (web set-ring): an unmatched key left mfx_slot
 *     at -1 and the write was dropped without a word.
 *   - the shadow_param handler: its else-branch assigns slot 0, so an
 *     unmatched "fx5:cutoff" was not dropped — it was routed INTO SLOT 0 with
 *     a garbage param key, writing to a different running module.
 *   - shadow_master_fx_lfo_tick: a target past the range left target_slot at
 *     -1, fell into `else { continue; }`, and the LFO just stopped modulating.
 *
 * So the slot_count is a PARAMETER here, passed by the caller as
 * MASTER_FX_SLOTS. The cap is named exactly once, in shadow_chain_mgmt.h, and
 * this file holds no copy of it. Raising it there is meant to be the whole
 * change.
 *
 * These functions are pure: no allocation, no I/O, no locks. Two of the three
 * call sites run on the SCHED_FIFO 90 SPI callback.
 */
#ifndef MASTER_FX_KEY_H
#define MASTER_FX_KEY_H

#include <stddef.h>

/* Buffer size for a formatted "fx%d" LFO target key. Matches
 * lfo_state_t.target (char[16]), which is what these are strcmp'd against —
 * a truncated target_key would compare unequal and silently un-modulate the
 * slot, so the two sizes must not drift apart. 16 bytes holds "fx" plus 13
 * digits; see the _Static_assert next to MASTER_FX_SLOTS's users. */
#define MASTER_FX_TARGET_KEY_LEN 16

/*
 * Parse a leading "fx<N>" with N a 1-based decimal index.
 *
 * Returns N (>= 1) on success and points *out_end at the first byte after the
 * digits; returns -1 on no match, leaving *out_end untouched.
 *
 * Leading zeros are rejected ("fx01" is not an id we emit), matching
 * chain_key_index.h. Multi-digit indices parse properly rather than being
 * read out of a single character — the old char-range form ('1'..'4') both
 * capped the range at 9 forever AND mis-parsed "fx12" as slot 1, since it
 * never looked past target[2]. Accumulation is clamped so a long digit run
 * can't overflow into a plausible-looking index; anything absurd is simply
 * out of range for the caller's slot_count.
 */
static inline int master_fx_parse_index(const char *s, const char **out_end)
{
    if (!s || s[0] != 'f' || s[1] != 'x') return -1;
    const char *p = s + 2;
    if (*p < '1' || *p > '9') return -1;   /* at least one digit, no leading 0 */
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (n < 1000000) n = n * 10 + (*p - '0');
        p++;
    }
    if (out_end) *out_end = p;
    return n;
}

/*
 * Route a Master FX param key of the form "fx<N>:<param>".
 *
 * On match: returns 1, *out_slot is the 0-based slot, *out_param points at the
 * param key inside `key` (no copy). On no match: returns 0 and the outputs are
 * left alone, so a caller can pre-seed its fallback.
 *
 * N must be within 1..slot_count. Out-of-range must NOT match: *out_slot
 * indexes shadow_master_fx_slots[] directly.
 */
static inline int master_fx_route_param_key(const char *key, int slot_count,
                                            int *out_slot, const char **out_param)
{
    const char *end = NULL;
    int n = master_fx_parse_index(key, &end);
    if (n < 1 || n > slot_count) return 0;
    if (*end != ':') return 0;
    if (out_slot) *out_slot = n - 1;
    if (out_param) *out_param = end + 1;
    return 1;
}

/*
 * Route an LFO target string, which is the bare component key "fx<N>" with no
 * param suffix ("lfo1"/"lfo2" and "" are handled by the caller).
 *
 * Returns the 0-based slot, or -1 if this is not an in-range FX target.
 * Trailing junk ("fx1x") is rejected — the old char-range test accepted it.
 */
static inline int master_fx_route_target(const char *target, int slot_count)
{
    const char *end = NULL;
    int n = master_fx_parse_index(target, &end);
    if (n < 1 || n > slot_count) return -1;
    if (*end != '\0') return -1;
    return n - 1;
}

#endif /* MASTER_FX_KEY_H */
