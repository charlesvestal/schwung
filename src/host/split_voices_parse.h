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
 * An entry whose id does not fit id_len is SKIPPED, not truncated: a truncated
 * id compares unequal to the one stored in a bus config and would orphan the
 * bus with no way to tell why.
 */
static inline int split_voices_parse(const char *json, void *ids_void,
                                     int max_ids, int id_len)
{
    if (!json) return SPLIT_VOICES_READ_FAILED;
    char (*ids)[1] = (char (*)[1])ids_void;
    int n = 0;
    const char *p = json;
    while (*p && n < max_ids) {
        const char *k = strstr(p, "\"id\"");
        if (!k) break;
        k += 4;
        while (*k == ' ' || *k == ':') k++;
        if (*k != '"') { p = k; continue; }
        k++;
        const char *end = strchr(k, '"');
        if (!end) break;
        int len = (int)(end - k);
        if (len > 0 && len < id_len) {
            char *dst = (char *)ids + (size_t)n * (size_t)id_len;
            memcpy(dst, k, (size_t)len);
            dst[len] = '\0';
            n++;
        }
        p = end + 1;
    }
    return n;
}

#endif /* SPLIT_VOICES_PARSE_H */
