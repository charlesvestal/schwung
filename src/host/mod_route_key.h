/*
 * mod_route_key.h — "mod<N>:<param>" and legacy "lfo<N>:<param>" routing.
 *
 * Header-only and dependency-free, like bus_route.h, send_fx_key.h and
 * master_fx_key.h, so tests/host can compile and RUN it natively. Its caller
 * lives in chain_mod_routes.c, a translation unit that needs the whole of
 * chain_internal.h — which is exactly how routing like this ends up shipped
 * untested.
 *
 * THE CAP IS A PARAMETER. MOD_ROUTE_COUNT is named once, in chain_internal.h,
 * and this file holds no copy of it. That discipline is inherited from
 * master_fx_key.h, whose preamble is the war story: every such route used to be
 * a hand-written strncmp ladder that restated the cap without naming it, a cap
 * raise broke seven sibling sites silently, and one else-branch ASSIGNED SLOT 0
 * — so an out-of-range key was not dropped but routed into a different running
 * module under a garbage param name.
 *
 * Hence: an unmatched key returns -1 with *out_rest UNTOUCHED. It never
 * defaults to route 0 and never clamps an out-of-range index onto the last real
 * route, so a caller may pre-seed its own fallback and know a rejection left it
 * alone.
 *
 * TWO SPELLINGS, ONE STORAGE. "lfo<N>:" is what every patch written before the
 * mod-route sources existed carries, and it resolves to the SAME route as
 * "mod<N>:" — routes 1 and 2 simply have two names. If the alias resolved to
 * separate storage, a set loaded twice would end up with four routes running.
 * It is capped at 2 independently of the real count because that is where the
 * on-disk format froze it: "lfo5" was never something anyone could have
 * written, so accepting it would invent a key rather than honour one.
 *
 * Pure: no allocation, no I/O, no locks. Called from the param handler, which
 * runs on the SCHED_FIFO SPI callback.
 */
#ifndef MOD_ROUTE_KEY_H
#define MOD_ROUTE_KEY_H

#include <stddef.h>
#include <string.h>

/* How many routes the legacy "lfo<N>:" spelling could ever address. A frozen
 * property of the on-disk format, not a cap that tracks anything. */
#define MOD_ROUTE_LEGACY_COUNT 2

/*
 * Resolve a "mod<N>:" or legacy "lfo<N>:" prefix to a 0-based route index.
 *
 * Returns the index (0-based) and points *out_rest at the byte after the colon;
 * returns -1 on no match, leaving *out_rest untouched. Leading zeros are
 * rejected ("mod01" is not an id we emit), matching bus_route.h and
 * chain_key_index.h. Accumulation is clamped so a long digit run cannot
 * overflow into a plausible-looking index.
 */
static inline int mod_route_parse_key(const char *key, int route_count,
                                      const char **out_rest)
{
    if (!key) return -1;

    int max;
    const char *p;
    if (strncmp(key, "mod", 3) == 0) {
        p = key + 3;
        max = route_count;
    } else if (strncmp(key, "lfo", 3) == 0) {
        p = key + 3;
        max = MOD_ROUTE_LEGACY_COUNT;
        if (max > route_count) max = route_count;
    } else {
        return -1;
    }

    if (*p < '1' || *p > '9') return -1;   /* rejects "mod0" and "mod01" */
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (n < 100000) n = n * 10 + (*p - '0');
        p++;
    }
    if (n < 1 || n > max) return -1;
    if (*p != ':') return -1;

    if (out_rest) *out_rest = p + 1;
    return n - 1;
}

#endif /* MOD_ROUTE_KEY_H */
