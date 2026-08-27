/*
 * Build the `master_fx:modules` answer: every position's id and DSP path in
 * ONE response, in position order.
 *
 * WHY THIS EXISTS. The saver used to ask each position two separate questions
 * (`master_fx:fxN:name`, `master_fx:fxN:module`). That is 8 + N round trips at
 * ~2.8ms each on the frame that autosave already runs on — the frame the
 * one-slot-per-tick split was created to keep clean — and worse, the id and the
 * path were two INDEPENDENT reads. Either could fail on its own, and a state
 * file pairing one position's id with another's path restores the wrong module
 * silently: the boot loader parses `module_path` and never looks at
 * `module_id` (shadow_chain_mgmt.c, "Extract module_path").
 *
 * One snapshot fixes both: one round trip, and the pair cannot disagree because
 * it is copied out of one struct in one pass.
 *
 * Header-only and dependency-free for the same reason as master_fx_key.h and
 * fx_midi_filter.h: so tests/host can compile and RUN it natively. Its caller
 * lives in shadow_chain_mgmt.c, a shim translation unit that cannot be built on
 * the dev machine — which is how string building like this ends up shipped
 * untested.
 *
 * Pure: no allocation, no I/O, no locks. The call site is the param service on
 * the SPI callback, so none of those would be permissible.
 */
#ifndef MASTER_FX_SNAPSHOT_H
#define MASTER_FX_SNAPSHOT_H

#include <stddef.h>

/*
 * Append `src` to `dst` as the INSIDE of a JSON string (no surrounding quotes),
 * escaping the two characters that can appear in a filesystem path and would
 * otherwise end the string early.
 *
 * A module id or DSP path holding a quote is absurd and has never been seen.
 * It is escaped anyway because the failure is not "an odd name renders oddly":
 * an unescaped quote makes the WHOLE array unparseable, and the reader treats
 * unparseable exactly as it treats a failed read — so one strange filename
 * would silently disable the persistence this snapshot exists to drive, for
 * every position at once. Control characters are dropped rather than escaped;
 * a \u form would need four more bytes of budget for a case that cannot occur
 * on a path this side of a corrupted filesystem.
 *
 * Returns 1 on success, 0 if the whole escaped form did not fit — in which case
 * *len is left where it was, so a refusal never leaves half a token behind.
 */
static inline int mfx_snapshot_put_escaped(char *buf, size_t cap, size_t *len,
                                           const char *src)
{
    if (!buf || !len) return 0;
    if (!src) src = "";
    size_t at = *len;
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            if (at + 2 >= cap) return 0;
            buf[at++] = '\\';
            buf[at++] = (char)c;
        } else if (c < 0x20) {
            continue;
        } else {
            if (at + 1 >= cap) return 0;
            buf[at++] = (char)c;
        }
    }
    buf[at] = '\0';
    *len = at;
    return 1;
}

/* Literal append, same contract. */
static inline int mfx_snapshot_put(char *buf, size_t cap, size_t *len,
                                   const char *src)
{
    if (!buf || !len || !src) return 0;
    size_t at = *len;
    for (const char *p = src; *p; p++) {
        if (at + 1 >= cap) return 0;
        buf[at++] = *p;
    }
    buf[at] = '\0';
    *len = at;
    return 1;
}

/* Open the array. Resets *len — this owns the buffer from here. */
static inline int master_fx_snapshot_begin(char *buf, size_t cap, size_t *len)
{
    if (!buf || !len || cap == 0) return 0;
    *len = 0;
    buf[0] = '\0';
    return mfx_snapshot_put(buf, cap, len, "[");
}

/*
 * Append one position as {"id":"…","path":"…"}.
 *
 * `index` is the position's 0-based slot and is used only to decide the leading
 * comma, so the caller cannot produce a trailing one by skipping a position —
 * it must not skip any. An UNLOADED position appends empty strings rather than
 * being omitted: the array is positional, and a reader that indexed a
 * compacted array would attribute position 3's module to position 1 the moment
 * anything ahead of it was empty.
 */
static inline int master_fx_snapshot_append(char *buf, size_t cap, size_t *len,
                                            int index, const char *id,
                                            const char *path)
{
    size_t at = *len;
    if (index > 0 && !mfx_snapshot_put(buf, cap, len, ",")) goto fail;
    if (!mfx_snapshot_put(buf, cap, len, "{\"id\":\"")) goto fail;
    if (!mfx_snapshot_put_escaped(buf, cap, len, id)) goto fail;
    if (!mfx_snapshot_put(buf, cap, len, "\",\"path\":\"")) goto fail;
    if (!mfx_snapshot_put_escaped(buf, cap, len, path)) goto fail;
    if (!mfx_snapshot_put(buf, cap, len, "\"}")) goto fail;
    return 1;
fail:
    /* All or nothing. A partial entry is worse than no answer: it parses as
     * nothing, and the reader cannot tell a truncated array from a failed
     * read, so it would fall back either way — but only if the array is
     * INVALID rather than plausibly short. */
    *len = at;
    buf[at] = '\0';
    return 0;
}

static inline int master_fx_snapshot_end(char *buf, size_t cap, size_t *len)
{
    return mfx_snapshot_put(buf, cap, len, "]");
}

#endif /* MASTER_FX_SNAPSHOT_H */
