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
 * Pure: no allocation, no I/O, no locks; called on the SPI callback.
 *
 * THERE IS NO TARGET FORMATTER HERE. There was one -- bus_route_target, with a
 * BUS_TARGET_KEY_LEN to size it -- and its only caller was its own test: a bus
 * insert chain declares `hasLfos: false`, so no bus LFO target is ever
 * formatted. It is deleted rather than kept "for symmetry", because a helper
 * exercised only by tests is a claim that something uses it. Restore it (and
 * its <stdio.h>) when a bus can actually be an LFO target.
 */
#ifndef BUS_ROUTE_H
#define BUS_ROUTE_H

#include <stddef.h>
#include <string.h>

/*
 * Parse a leading "bus<N>" with N a 1-based decimal index.
 *
 * Returns N (>= 1) and points *out_end at the first byte after the digits;
 * returns -1 on no match, leaving *out_end untouched. Leading zeros are
 * rejected, matching chain_key_index.h. Accumulation is clamped so a long
 * digit run cannot overflow into a plausible-looking index.
 */
static inline int bus_route_parse_prefixed(const char *key, const char *prefix,
                                          const char **out_end)
{
    if (!key || !prefix) return -1;
    size_t n = strlen(prefix);
    if (strncmp(key, prefix, n) != 0) return -1;
    const char *p = key + n;
    if (*p < '1' || *p > '9') return -1;   /* rejects "<prefix>0" and "<prefix>01" */
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        if (v < 100000) v = v * 10 + (*p - '0');
        p++;
    }
    if (out_end) *out_end = p;
    return v;
}

static inline int bus_route_parse_index(const char *key, const char **out_end)
{
    return bus_route_parse_prefixed(key, "bus", out_end);
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

/*
 * THERE IS NO "voice<V>:send<M>" ROUTE. One lived here for as long as the host
 * owned per-voice send levels and drew faders for them on the Send Mixer. The
 * MODULE owns them now — they are its own parameters, on its own pages, written
 * as `synth:<key>` like every other one — so a second spelling that set the same
 * number would be a second source of truth. See voice_send_source.h.
 */

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
     * the same orphan situation described above, one level up.
     *
     * A hole from split_voices_parse is an EMPTY STRING at the entry's own
     * index, never a compacted-away slot -- the index is the render-buffer
     * index, so a rejected id (too long, or empty) still consumes its slot.
     * The chain host stores the table as char[N][32], whose entries can
     * never BE NULL; the ids[i] NULL check below additionally tolerates a
     * caller that built its own pointer array with gaps, which is a
     * different producer than split_voices_parse.
     *
     * An empty stored id must never match: skip it explicitly so a caller
     * asking for "" (however that could arise) cannot resolve to the first
     * hole -- strcmp("", "") would otherwise match. */
    if (!ids || !id) return -1;
    for (int i = 0; i < n_ids; i++)
        if (ids[i] && ids[i][0] != '\0' && strcmp(ids[i], id) == 0) return i;
    return -1;
}

#endif /* BUS_ROUTE_H */
