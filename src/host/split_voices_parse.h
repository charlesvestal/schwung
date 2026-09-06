/*
 * split_voices_parse.h — extract the flat ordered voice-id list a module
 * publishes as get_param("split_voices").
 *
 * Header-only so tests/host can run it; called from chain_host.c on the SPI
 * callback at synth-load time, so: no allocation, no I/O, bounded scan.
 *
 * A THREE-ANSWER READ. Callers must branch on the RAW value before parsing:
 *   JSON  the module answered
 *   ""    the channel served us, the key produced nothing (no split support)
 *   NULL  the read did not complete — SPLIT_VOICES_READ_FAILED
 * Collapsing NULL into "" is what makes a timed-out read latch as a verdict.
 * NULL is meaningful only for a caller reading through the SHM param channel,
 * where a request can genuinely time out or be claimed by someone else — an
 * in-process get_param() call (chain_host.c's own call site) cannot produce
 * it, only json/"".
 *
 * THE TABLE INDEX IS THE RENDER-BUFFER INDEX. bus_mix_build_table hands
 * voice_out[i] to the module for voice i, so entry i in this table must be
 * entry i in the module's JSON, always — never compacted. An entry whose id
 * is rejected (too long for id_len, or empty) is a HOLE: it still consumes
 * its slot and is still counted in n, stored as an empty string at its own
 * index. Compacting instead — dropping the entry and shifting everything
 * behind it up by one — is worse than the truncation this rejection exists to
 * prevent: a truncated id orphans one bus, visibly; a shifted index silently
 * re-points every voice behind the rejected one to the wrong render buffer,
 * with nothing on screen to say so.
 *
 * The scan is FLAT: it looks for the next literal "id" token and does not
 * track object nesting or braces. Entries must be flat objects with no
 * nested "id" key (e.g. no {"id":"kick","meta":{"id":"inner"}}) — a nested
 * "id" is harvested as a phantom voice. Ids are also expected to be
 * quote-free: an id containing an escaped `\"` truncates at the backslash,
 * silently, because the scan has no escape handling.
 */
#ifndef SPLIT_VOICES_PARSE_H
#define SPLIT_VOICES_PARSE_H

#include <stddef.h>
#include <string.h>

#define SPLIT_VOICES_READ_FAILED (-1)

/*
 * Parse [{"id":"kick",...},...] into ids[0..n). Returns the count, or
 * SPLIT_VOICES_READ_FAILED if json is NULL.
 *
 * An entry whose id does not fit id_len, or is empty, is SKIPPED as a value
 * (not truncated: a truncated id compares unequal to the one stored in a bus
 * config and would orphan the bus with no way to tell why) but its slot is
 * NOT compacted away — see the header comment above. ids[n] for a skipped
 * entry is an empty string, and n is still incremented, so the caller's
 * index always matches the module's.
 */
static inline int split_voices_parse(const char *json, void *ids_void,
                                     int max_ids, int id_len)
{
    if (!json) return SPLIT_VOICES_READ_FAILED;
    char *base = (char *)ids_void;
    int n = 0;
    const char *p = json;
    while (*p && n < max_ids) {
        const char *k = strstr(p, "\"id\"");
        if (!k) break;
        k += 4;
        while (*k == ' ' || *k == ':' || *k == '\t' || *k == '\r' || *k == '\n') k++;
        if (*k != '"') { p = k; continue; }
        k++;
        const char *end = strchr(k, '"');
        if (!end) break;
        int len = (int)(end - k);
        char *dst = base + (size_t)n * (size_t)id_len;
        if (len > 0 && len < id_len) {
            memcpy(dst, k, (size_t)len);
            dst[len] = '\0';
        } else {
            /* Hole: rejected id (too long or empty). Own index preserved,
             * still counted — see the header comment. */
            dst[0] = '\0';
        }
        n++;
        p = end + 1;
    }
    return n;
}

#endif /* SPLIT_VOICES_PARSE_H */
