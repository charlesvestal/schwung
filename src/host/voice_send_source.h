/*
 * voice_send_source.h — how the HOST reads a module's own per-voice send
 * levels, and how it turns the module's numbers into 0..127.
 *
 * ================= WHY THE MODULE OWNS THESE ================================
 *
 * There are three tiers of send and each is owned where the thing it sends
 * lives: a VOICE's send belongs to the module (it is a property of the pad, on
 * the module's own pages, saved in the module's own `state` blob), a BUS's send
 * belongs to the slot's bus row, a SLOT's send belongs to Slot Settings. The
 * host briefly owned the first tier too and drew faders for it on the Send
 * Mixer; dr32 already had per-pad `send1`/`send2` knobs beside pan and cutoff,
 * so the same concept appeared twice, in two places, meaning different things.
 * The mixer's voice faders and their `buses:voice<V>:send<M>` route are gone.
 * The AUDIO machinery — the solo partition in bus_mix.h, the fold-back, the
 * drain — is untouched: only the SOURCE of the numbers changed.
 *
 * ================= THE DECLARATION =========================================
 *
 * Beside its voices, a module publishes a key TEMPLATE per send:
 *
 *     get_param("split_voices")      [{"id":"pad1","label":"Kick"}, ...]
 *     get_param("voice_send_params") ["{id}_send1", "{id}_send2"]
 *
 * `{id}` is replaced by the voice id, so the host reads `pad1_send1`,
 * `pad1_send2`, ... straight off the module's own parameter surface.
 *
 * ARRAY POSITION IS THE SEND INDEX: entry 0 is Send A, entry 1 is Send B. A
 * shorter array declares fewer sends, which is a real and useful answer. A
 * LONGER one is an ERROR — the whole declaration is refused — because the
 * alternative is truncating it, and a module that believes it declared three
 * sends while the host silently kept two has a control that writes into
 * nothing with nothing on screen to say so.
 *
 * A module that declares NOTHING gets no per-voice sends. That is the correct
 * fallback and not a gap: put the voice in a bus and ride the bus's send.
 *
 * SUBSTITUTION IS VERBATIM. The host does not know, and must never guess, how a
 * module numbers its own parameters — if a module's split_voices ids do not
 * address its own params when substituted, that is the module's contract to
 * fix, and a host that "corrected" an off-by-one would be unpredictable for
 * every other module.
 *
 * ================= THE RANGE ===============================================
 *
 * A send level is 0..127 (BUS_MIX_SEND_LEVEL_MAX; 127 is exactly unity). A
 * module's own parameter is whatever the module says it is — dr32's is dB over
 * -70..+6 — so the host NEVER assumes 0..127. It reads the parameter's declared
 * range out of the module's own `chain_params` metadata, the same metadata the
 * knob grid draws the control from, and maps:
 *
 *   unit "dB"   level = 127 * 10^(v/20), clamped     (0 dB is unity, exactly)
 *   otherwise   level = 127 * (v - min) / (max - min)
 *
 * In both, a value AT OR BELOW the declared minimum is exactly 0, so the
 * control's own off position is off whatever floor the module chose.
 *
 * WHEN THE METADATA CANNOT BE FOUND, THE HOST REFUSES rather than picking a
 * scale. Assuming 0..127 silently mis-scales a 0..1 module by a factor of 127;
 * assuming 0..1 silently mutes a 0..127 one. A refusal is the same discipline
 * this tree applies to a failed read: never let a missing answer become a
 * plan, a default, or a level.
 *
 * The metadata is looked up under TWO spellings, in this order:
 *   1. the template with `{id}` and one adjacent `_` removed — `send1` — which
 *      is how a per-voice parameter is actually declared in a ui_hierarchy: it
 *      is authored ONCE on the child level and addressed by focus, because 32
 *      voices x N params would not fit chain_params;
 *   2. the fully substituted key for the first voice — `pad1_send1` — for a
 *      module that really does declare every one of them.
 *
 * ================= WHERE IT RUNS ===========================================
 *
 * Header-only and dependency-free (libm aside) for the reason bus_mix.h and
 * master_fx_key.h are: chain_host.c cannot be built on the dev machine, so
 * arithmetic living there is arithmetic nobody runs. Everything here is called
 * from the SPI callback — no allocation, no I/O, no locks, bounded scans.
 */
#ifndef VOICE_SEND_SOURCE_H
#define VOICE_SEND_SOURCE_H

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "bus_mix.h"

/* A template such as "{id}_send1". Long enough for a descriptive suffix and
 * short enough that BUS_MIX_SENDS of them cost nothing in the instance. */
#define VOICE_SEND_TMPL_LEN 48
/* A substituted key: SPLIT_VOICE_ID_LEN (32) worth of id plus the template. */
#define VOICE_SEND_KEY_LEN 80

/* The declaration was unusable — too many entries, an entry that does not fit,
 * or an entry with no "{id}" in it. Refused whole; see the header comment. */
#define VOICE_SEND_PARAMS_INVALID (-1)

#define VOICE_SEND_ID_TOKEN "{id}"
#define VOICE_SEND_ID_TOKEN_LEN 4

/*
 * Parse `["{id}_send1","{id}_send2"]` into tmpl[0..n), preserving ORDER —
 * entry i is send i, always, never compacted, for the same reason
 * split_voices_parse never compacts: the position IS the meaning.
 *
 * Returns the count (0 for "" or an empty array, which is the module saying it
 * has no per-voice sends), or VOICE_SEND_PARAMS_INVALID.
 *
 * NULL json is INVALID rather than 0. This is only ever called on a direct
 * in-process get_param whose buffer cannot be NULL, so it cannot arise there;
 * a caller reading through the SHM param channel, where a request can time
 * out, must branch on the raw value before it gets here.
 */
static inline int voice_send_params_parse(const char *json, void *tmpl_void,
                                          int max_tmpl, int tmpl_len)
{
    if (!json || !tmpl_void || max_tmpl <= 0 || tmpl_len <= 0)
        return VOICE_SEND_PARAMS_INVALID;
    char *base = (char *)tmpl_void;
    int n = 0;
    const char *p = json;
    while (*p) {
        if (*p != '"') { p++; continue; }
        p++;
        const char *end = strchr(p, '"');
        if (!end) return VOICE_SEND_PARAMS_INVALID;   /* unterminated */
        int len = (int)(end - p);
        /* One entry past the cap is the error the whole declaration is refused
         * for — never a quiet truncation to BUS_MIX_SENDS. */
        if (n >= max_tmpl) return VOICE_SEND_PARAMS_INVALID;
        if (len <= 0 || len >= tmpl_len) return VOICE_SEND_PARAMS_INVALID;
        char *dst = base + (size_t)n * (size_t)tmpl_len;
        memcpy(dst, p, (size_t)len);
        dst[len] = '\0';
        /* No "{id}" means one key for every voice — every pad writing the same
         * level. Refused, because it would look like a working feature. */
        if (!strstr(dst, VOICE_SEND_ID_TOKEN)) return VOICE_SEND_PARAMS_INVALID;
        n++;
        p = end + 1;
    }
    return n;
}

/*
 * Substitute: "{id}_send1" + "pad1" -> "pad1_send1". Only the FIRST occurrence
 * is substituted; a second `{id}` is left literal, which makes the key miss
 * loudly rather than address something.
 *
 * Returns 1 on success, 0 when the result would not fit or there is no token.
 */
static inline int voice_send_key_build(const char *tmpl, const char *id,
                                       char *out, int out_len)
{
    if (!tmpl || !id || !out || out_len <= 0) return 0;
    const char *tok = strstr(tmpl, VOICE_SEND_ID_TOKEN);
    if (!tok) return 0;
    size_t head = (size_t)(tok - tmpl);
    size_t idl = strlen(id);
    const char *tail = tok + VOICE_SEND_ID_TOKEN_LEN;
    size_t taill = strlen(tail);
    if (head + idl + taill + 1 > (size_t)out_len) return 0;
    memcpy(out, tmpl, head);
    memcpy(out + head, id, idl);
    memcpy(out + head + idl, tail, taill);
    out[head + idl + taill] = '\0';
    return 1;
}

/*
 * The METADATA spelling of a template: "{id}_send1" -> "send1".
 *
 * The token and ONE adjacent separator go — the one after it if there is one,
 * else the one before ("pad_{id}" -> "pad"). That is the focus-addressed form
 * a per-voice parameter is actually declared under in a ui_hierarchy.
 *
 * Returns 1 on success, 0 when it does not fit or the result would be empty.
 */
static inline int voice_send_meta_key(const char *tmpl, char *out, int out_len)
{
    if (!tmpl || !out || out_len <= 0) return 0;
    const char *tok = strstr(tmpl, VOICE_SEND_ID_TOKEN);
    if (!tok) return 0;
    size_t head = (size_t)(tok - tmpl);
    const char *tail = tok + VOICE_SEND_ID_TOKEN_LEN;
    if (*tail == '_' || *tail == '.' || *tail == ':') tail++;
    else if (head > 0 && (tmpl[head - 1] == '_' || tmpl[head - 1] == '.' ||
                          tmpl[head - 1] == ':')) head--;
    size_t taill = strlen(tail);
    if (head + taill == 0) return 0;
    if (head + taill + 1 > (size_t)out_len) return 0;
    memcpy(out, tmpl, head);
    memcpy(out + head, tail, taill);
    out[head + taill] = '\0';
    return 1;
}

/*
 * The SUFFIX of a template — everything after `{id}` — which is what a key has
 * to end with to be one of this module's per-voice send keys. Used to notice a
 * write going past on the way to the module, so the next frame re-reads the
 * whole table instead of waiting for the background sweep to come round.
 *
 * A suffix match is deliberately LOOSE: it also catches dr32's focus-addressed
 * `pad_send1`, which names a pad the host cannot resolve and which the exact
 * substituted keys therefore cannot match. It costs one extra sweep and never
 * a wrong level, because the sweep asks the module rather than believing the
 * key.
 */
static inline int voice_send_key_has_suffix(const char *tmpl, const char *key)
{
    if (!tmpl || !key) return 0;
    const char *tok = strstr(tmpl, VOICE_SEND_ID_TOKEN);
    if (!tok) return 0;
    const char *suffix = tok + VOICE_SEND_ID_TOKEN_LEN;
    size_t sl = strlen(suffix);
    if (sl == 0) return 0;   /* "{id}" alone matches every key; refuse */
    size_t kl = strlen(key);
    if (kl < sl) return 0;
    return strcmp(key + (kl - sl), suffix) == 0;
}

/* Case-insensitive "dB". Spelled out rather than pulled from <strings.h> so
 * this header stays as portable as bus_mix.h. */
static inline int voice_send_unit_is_db(const char *unit)
{
    if (!unit) return 0;
    if (!((unit[0] == 'd' || unit[0] == 'D') && (unit[1] == 'b' || unit[1] == 'B')))
        return 0;
    return unit[2] == '\0';
}

/*
 * Map one of the module's own values onto 0..127.
 *
 * Returns 1 and writes *out on success; 0 — a REFUSAL, leaving *out alone —
 * when the declared range cannot carry a mapping (max <= min). See the header
 * comment for why a refusal beats a guessed scale.
 */
static inline int voice_send_level_map(double v, double min, double max,
                                       int is_db, int *out)
{
    if (!(max > min)) return 0;
    double lvl;
    /* AT OR BELOW THE FLOOR IS EXACTLY OFF, whatever floor the module chose —
     * -70 dB and 0.0 alike. Without this the dB law leaves 10^(-70/20)*127 =
     * 0.04, which rounds to 0 today and would not for a module whose floor is
     * -40 dB (1.27 -> 1): a control at its own minimum still sending. */
    if (v <= min) { if (out) *out = 0; return 1; }
    if (is_db) lvl = pow(10.0, v / 20.0) * (double)BUS_MIX_SEND_LEVEL_MAX;
    else       lvl = (v - min) / (max - min) * (double)BUS_MIX_SEND_LEVEL_MAX;
    int n = (int)(lvl + 0.5);
    if (n < 0) n = 0;
    if (n > BUS_MIX_SEND_LEVEL_MAX) n = BUS_MIX_SEND_LEVEL_MAX;
    if (out) *out = n;
    return 1;
}

#endif /* VOICE_SEND_SOURCE_H */
