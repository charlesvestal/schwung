/*
 * Indexed component keys: "fx3:cutoff", "midi_fx2:rate".
 *
 * Header-only and dependency-free on purpose, so tests/host can compile and
 * RUN it natively (tests/host/test_chain_key_index.c) instead of only grepping
 * for it. The routing in chain_host.c is one branch per prefix now, so this
 * parser is the single point where an off-by-one would silently send a
 * parameter to the wrong FX slot.
 */
#ifndef CHAIN_KEY_INDEX_H
#define CHAIN_KEY_INDEX_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * Shared digit scan behind both spellings below: prefix + 1-based index, with
 * *end left at the first character after the digits so each wrapper can say
 * what is allowed to follow. Returns the 0-based index, or -1.
 *
 * Replaces two copy-paste branches that differed only by 0 vs 1 over
 * machinery that was already index-generic. Enumerating eight of them would
 * be eight times the same bug surface.
 *
 * `max` is a PARAMETER on purpose. Reading MAX_AUDIO_FX / MAX_MIDI_FX
 * directly would look like a simplification and would cost two things: this
 * header would stop being includable on its own (it would depend on
 * chain_internal.h, and so on include order), and the test could no longer
 * drive the parser at an arbitrary bound to prove it is cap-agnostic. The
 * caller names the cap because the caller knows which list it is indexing.
 *
 * Leading zeros are rejected ("fx01" is not an id we emit), and the digit
 * loop bails the moment it passes `max` — these strings arrive from patch and
 * preset JSON under /data/UserData, which a user can hand-edit, so an
 * unbounded accumulate would be signed overflow (UB) on input we do not
 * control.
 */
static inline int chain_fx_index_scan(const char *s, const char *prefix,
                                      int max, const char **end) {
    if (!s || !prefix || max < 1) return -1;
    size_t plen = strlen(prefix);
    if (strncmp(s, prefix, plen) != 0) return -1;
    const char *p = s + plen;
    if (*p < '1' || *p > '9') return -1;
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        if (n > max) return -1;  /* out of range, and keeps n from overflowing */
        p++;
    }
    *end = p;
    return n - 1;
}

/*
 * "fx3:cutoff" -> 2, and *subkey points at "cutoff". Returns -1 when the key
 * is not an fx key or the index is out of range.
 */
static inline int chain_fx_index_from_key(const char *key, const char *prefix,
                                          int max, const char **subkey) {
    const char *end = NULL;
    int idx = chain_fx_index_scan(key, prefix, max, &end);
    if (idx < 0) return -1;
    if (*end != ':') return -1;
    if (subkey) *subkey = end + 1;
    return idx;
}

/*
 * "fx3" -> 2: the reverse of chain_fx_component_id, for the BARE component ids
 * (no ":subkey") that knob mappings and modulation targets carry. Same bounds
 * and leading-zero rules as chain_fx_index_from_key; the two differ only in
 * what is allowed to follow the digits.
 *
 * Note "midi_fx1" does not match prefix "fx" (the prefix must match from the
 * start), so the two component families stay distinct without ordering rules.
 */
static inline int chain_fx_index_from_id(const char *id, const char *prefix,
                                         int max) {
    const char *end = NULL;
    int idx = chain_fx_index_scan(id, prefix, max, &end);
    if (idx < 0) return -1;
    if (*end != '\0') return -1;
    return idx;
}

/*
 * "fx3_module" -> 2, for the UNDERSCORE spelling the get_param readback uses.
 *
 * This is a third spelling of the same id and it is not decorative: the editor
 * asks "what is in position N" with `fxN_module`, and only `fx1_module` and
 * `fx2_module` were ever served. `fx_count` answered 3 correctly while
 * `fx3_module` answered nothing, an unserved key reads as "", and the reader
 * discards a trailing empty — so a third FX loaded, ran, made sound, and was
 * invisible in the editor. Observed on hardware 2026-08-20.
 */
static inline int chain_fx_index_from_suffixed(const char *key, const char *prefix,
                                               int max, const char *suffix) {
    const char *end = NULL;
    int idx = chain_fx_index_scan(key, prefix, max, &end);
    if (idx < 0) return -1;
    if (!suffix || strcmp(end, suffix) != 0) return -1;
    return idx;
}

/* "fx" + 3 -> "fx3": the component id chain_mod_* and the patch layer use. */
static inline void chain_fx_component_id(char *buf, size_t buf_len,
                                         const char *prefix, int index) {
    snprintf(buf, buf_len, "%s%d", prefix, index + 1);
}

#endif /* CHAIN_KEY_INDEX_H */
