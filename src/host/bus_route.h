/*
 * bus_route.h — "bus<N>:" parameter routing and voice-id resolution.
 *
 * Header-only and dependency-free so tests/host can run it natively; see
 * master_fx_key.h, whose failure modes this deliberately copies the fix for.
 * An unmatched key here returns 0 and leaves the out-params ALONE. It must
 * never fall through to bus 0: Master FX's handler had exactly that
 * else-branch, and an unmatched "fx5:cutoff" was not dropped but routed into
 * slot 0 with a garbage param key, writing to a different running module.
 *
 * The cap is a PARAMETER (bus_count). SLOT_BUSES is named once, in
 * chain_internal.h, and this file holds no copy of it.
 *
 * Pure: no allocation, no I/O, no locks. The PARSERS are the ones called on
 * the SPI callback. bus_route_target is control-path only -- it is the one
 * function here that pulls in <stdio.h>, and while snprintf into a stack
 * buffer allocates nothing, takes no FILE lock and makes no syscall, an RT
 * audit that greps for stdio should not have to re-derive that.
 */
#ifndef BUS_ROUTE_H
#define BUS_ROUTE_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Buffer for a formatted "bus%d" LFO target key. Matches lfo_state_t.target
 * (char[16]) for the same reason MASTER_FX_TARGET_KEY_LEN does: a truncated
 * target compares unequal and silently stops modulating. */
#define BUS_TARGET_KEY_LEN 16

/*
 * Parse a leading "bus<N>" with N a 1-based decimal index.
 *
 * Returns N (>= 1) and points *out_end at the first byte after the digits;
 * returns -1 on no match, leaving *out_end untouched. Leading zeros are
 * rejected, matching chain_key_index.h. Accumulation is clamped so a long
 * digit run cannot overflow into a plausible-looking index.
 */
static inline int bus_route_parse_index(const char *key, const char **out_end)
{
    if (!key) return -1;
    if (key[0] != 'b' || key[1] != 'u' || key[2] != 's') return -1;
    const char *p = key + 3;
    if (*p < '1' || *p > '9') return -1;   /* rejects "bus0" and "bus01" */
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (n < 100000) n = n * 10 + (*p - '0');
        p++;
    }
    if (out_end) *out_end = p;
    return n;
}

/*
 * Route "bus<N>:<rest>" to a 0-based bus index and the remainder.
 *
 * Returns 1 on a match, 0 otherwise. On 0 the out-params are untouched.
 */
static inline int bus_route_param_key(const char *key, int bus_count,
                                      int *out_bus, const char **out_rest)
{
    const char *end = NULL;
    int n = bus_route_parse_index(key, &end);
    if (n < 1 || n > bus_count) return 0;
    if (!end || *end != ':') return 0;
    if (out_bus) *out_bus = n - 1;
    if (out_rest) *out_rest = end + 1;
    return 1;
}

/* Format the "bus%d" LFO target for a 1-based index. Returns 1 on success, 0
 * if it would not fit — never a truncated key, because a truncated target
 * compares unequal to the one the LFO holds and silently stops modulating
 * rather than erroring. See BUS_TARGET_KEY_LEN above. */
static inline int bus_route_target(char *out, size_t out_len, int bus_1based)
{
    if (!out || out_len == 0 || bus_1based < 1) return 0;
    int n = snprintf(out, out_len, "bus%d", bus_1based);
    return (n > 0 && (size_t)n < out_len) ? 1 : 0;
}

/*
 * Resolve a voice id to its index in the module's flat split_voices list.
 *
 * Returns -1 on a miss. A miss is not an error: a bus config stores ids so a
 * module that adds a voice in a later version does not silently re-point every
 * existing bus, which means an id can legitimately no longer exist. The caller
 * reports the orphan rather than guessing.
 */
static inline int bus_voice_index(const char *const *ids, int n_ids, const char *id)
{
    /* !ids guards a caller holding a stale n_ids over a list that is gone --
     * the same orphan situation described above, one level up. The per-element
     * ids[i] check guards a partially populated list: split_voices_parse (the
     * next task) SKIPS an id too long for its buffer rather than truncating
     * it, so a hole is a state the producer can legitimately emit. */
    if (!ids || !id) return -1;
    for (int i = 0; i < n_ids; i++)
        if (ids[i] && strcmp(ids[i], id) == 0) return i;
    return -1;
}

#endif /* BUS_ROUTE_H */
