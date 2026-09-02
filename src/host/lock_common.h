/* lock_common.h - Parameter locks: per-step, per-parameter absolute overrides.
 *
 * An Elektron-style parameter lock. Hold a step button, turn an encoder, and
 * that value belongs to that step: "step 9 has a shorter Snappy". The base
 * value the user saved is never written — the lock is published as an ABSOLUTE
 * modulation source (chain_mod_emit_absolute) while its step is playing and
 * cleared when the step passes, so turning locks off restores exactly what was
 * saved.
 *
 * WHY THE STEP IS DERIVED AND NOT REPORTED. Nothing tells Schwung where Move is
 * inside its clip: host_api_v1 offers get_beat_position() — beats since
 * transport start — and there is no Song Position Pointer anywhere in the tree.
 * So the step is computed from beat position given a pattern length and a step
 * rate, and BOTH ARE EXPLICIT USER SETTINGS (lock:pattern_len, lock:rate_div)
 * rather than inferred. That is the whole design decision: a guessed 16-step
 * 1/16 pattern is right until the clip is 32 steps or a triplet feel, and then
 * every lock lands on the wrong step with nothing on screen to explain it.
 * Wrong-but-visible beats wrong-and-silent — the user can see 16 and change it.
 *
 * Divisions are lfo_divisions[] from lfo_common.h, deliberately: one table, one
 * migration story, and a lock at 1/16 means the same thing an LFO at 1/16 does.
 */

#ifndef LOCK_COMMON_H
#define LOCK_COMMON_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lfo_common.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Lanes are (target, param) pairs carrying locks, not locks. Sixteen is not
 * arbitrary: every lane that fires on the current step consumes one of the
 * chain's MAX_MOD_TARGETS (32) modulation entries, and the two LFOs want some
 * of those too. Only the CURRENT step's lanes are ever live at once. */
#define LOCK_MAX_LANES 16
#define LOCK_MAX_STEPS 64
#define LOCK_DEFAULT_STEPS 16

/* Index of "1/16" in lfo_divisions[] — the default step rate, and what Move's
 * own step sequencer does. Asserted in tests rather than trusted. */
#define LOCK_DEFAULT_RATE_DIV 23

#define LOCK_STEP_NONE (-1)

/* ============================================================================
 * State
 * ============================================================================ */

typedef struct {
    int active;
    char target[16];            /* "synth", "fx1", "midi_fx1", ... */
    char param[32];
    uint64_t mask;              /* bit N set = a lock exists on step N */
    float values[LOCK_MAX_STEPS];
} lock_lane_t;

typedef struct {
    lock_lane_t lanes[LOCK_MAX_LANES];
    int lane_count;

    int pattern_len;            /* steps in the loop, 1..LOCK_MAX_STEPS */
    int rate_div;               /* index into lfo_divisions[]: beats per step */

    int cur_step;               /* step currently published, or LOCK_STEP_NONE */
    int enabled;                /* master on/off; off clears every lock source */
} lock_state_t;

/* ============================================================================
 * Helpers
 * ============================================================================ */

static inline void lock_state_init(lock_state_t *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->pattern_len = LOCK_DEFAULT_STEPS;
    st->rate_div = LOCK_DEFAULT_RATE_DIV;
    st->cur_step = LOCK_STEP_NONE;
    st->enabled = 1;
}

static inline int lock_clamp_pattern_len(int len) {
    if (len < 1) return 1;
    if (len > LOCK_MAX_STEPS) return LOCK_MAX_STEPS;
    return len;
}

static inline int lock_clamp_rate_div(int div) {
    if (div < 0) return 0;
    if (div >= LFO_NUM_DIVISIONS) return LFO_NUM_DIVISIONS - 1;
    return div;
}

/* Which step of the loop `beat_position` falls in, or LOCK_STEP_NONE when no
 * transport is running (get_beat_position answers < 0 then).
 *
 * Pure function of song position, exactly as lfo_synced_phase is, so it cannot
 * drift: a lock is on step 9 at bar 400 for the same reason it was at bar 1. */
static inline int lock_step_at(double beat_position, int rate_div, int pattern_len) {
    if (beat_position < 0.0) return LOCK_STEP_NONE;

    pattern_len = lock_clamp_pattern_len(pattern_len);
    const double beats_per_step = (double)lfo_divisions[lock_clamp_rate_div(rate_div)].beats;
    if (!(beats_per_step > 0.0)) return LOCK_STEP_NONE;

    const double index = floor(beat_position / beats_per_step);
    double step = fmod(index, (double)pattern_len);
    if (step < 0.0) step += (double)pattern_len;
    return (int)step;
}

static inline int lock_lane_has_step(const lock_lane_t *lane, int step) {
    if (!lane || !lane->active || step < 0 || step >= LOCK_MAX_STEPS) return 0;
    return (lane->mask >> step) & 1u;
}

/* VALUE BEFORE MASK, and the order is the point.
 *
 * lock_tick runs on the audio thread while this runs on the set_param thread —
 * the same exposure the LFOs already have. Setting the bit first opens a window
 * where the tick sees "step 9 is locked" and reads the zero still sitting in
 * values[9], which is an audible click at the moment of placing a lock. The
 * other order's worst case is one tick that has not noticed the lock yet. */
static inline void lock_lane_set(lock_lane_t *lane, int step, float value) {
    if (!lane || step < 0 || step >= LOCK_MAX_STEPS) return;
    lane->values[step] = value;
    lane->mask |= ((uint64_t)1u << step);
}

/* Mask first here, for the mirror of the reason above: clearing the bit before
 * the value means a concurrent tick can never read the zero as a lock. */
static inline void lock_lane_clear(lock_lane_t *lane, int step) {
    if (!lane || step < 0 || step >= LOCK_MAX_STEPS) return;
    lane->mask &= ~((uint64_t)1u << step);
    lane->values[step] = 0.0f;
}

/* Find the lane for (target, param), or NULL. */
static inline lock_lane_t *lock_find_lane(lock_state_t *st, const char *target, const char *param) {
    if (!st || !target || !param) return NULL;
    for (int i = 0; i < st->lane_count && i < LOCK_MAX_LANES; i++) {
        lock_lane_t *lane = &st->lanes[i];
        if (!lane->active) continue;
        if (strcmp(lane->target, target) == 0 && strcmp(lane->param, param) == 0) return lane;
    }
    return NULL;
}

/* Find or create the lane for (target, param). NULL when all lanes are used. */
static inline lock_lane_t *lock_alloc_lane(lock_state_t *st, const char *target, const char *param) {
    lock_lane_t *lane = lock_find_lane(st, target, param);
    if (lane) return lane;
    if (!st || !target || !param || !target[0] || !param[0]) return NULL;

    for (int i = 0; i < LOCK_MAX_LANES; i++) {
        lane = &st->lanes[i];
        if (lane->active) continue;

        /* `active` LAST, then lane_count — same reason as lock_lane_set. A lane
         * published as active before it has a target is one the audio thread
         * can pick up and try to emit against an empty parameter name. */
        memset(lane, 0, sizeof(*lane));
        strncpy(lane->target, target, sizeof(lane->target) - 1);
        strncpy(lane->param, param, sizeof(lane->param) - 1);
        lane->active = 1;
        if (i >= st->lane_count) st->lane_count = i + 1;
        return lane;
    }
    return NULL;
}

/* Drop a lane that no longer holds any lock, so its slot and its modulation
 * entry are both returned. Safe to call on a lane that still has locks. */
static inline void lock_retire_lane_if_empty(lock_state_t *st, lock_lane_t *lane) {
    if (!st || !lane || !lane->active || lane->mask != 0) return;

    memset(lane, 0, sizeof(*lane));
    while (st->lane_count > 0 && !st->lanes[st->lane_count - 1].active) {
        st->lane_count--;
    }
}

/* Stable modulation source id for a lane: "plk0".."plk15".
 * Distinct per lane so one lane's lock never clears another's. */
static inline void lock_source_id(int lane_index, char *buf, int buf_len) {
    if (!buf || buf_len < 6) return;
    snprintf(buf, (size_t)buf_len, "plk%d", lane_index);
}

/* ============================================================================
 * Serialisation
 *
 * The on-disk shape lives HERE, next to the struct, so the writer and the
 * reader cannot drift apart — and so a round trip can be tested without a
 * device, a chain instance or a patch file. chain_host.c serialises through
 * this and chain_patch.c parses through it; neither owns the format.
 *
 *   {"pattern_len":16,"rate_div":23,"enabled":1,"division_table_version":27,
 *    "lanes":[{"target":"synth","param":"snappy","steps":[[9,0.200000]]}]}
 *
 * Sparse: only lanes holding a lock, only the steps they lock.
 * ============================================================================ */

/* Read `"key":<number>` inside [begin,end). Returns 1 and writes *out on a
 * hit, 0 otherwise. Deliberately literal — this parses what lock_to_json
 * writes plus whatever survives a human editing a patch file by hand. */
static inline int lock_json_number(const char *begin, const char *end,
                                   const char *key, double *out) {
    if (!begin || !end || !key || !out) return 0;

    char pat[48];
    const int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return 0;

    for (const char *p = begin; p && p < end; ) {
        const char *hit = strstr(p, pat);
        if (!hit || hit >= end) return 0;

        const char *c = hit + n;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        if (c < end && *c == ':') {
            *out = strtod(c + 1, NULL);
            return 1;
        }
        p = hit + n;   /* a longer key ending in this one — keep looking */
    }
    return 0;
}

/* Read `"key":"value"` inside [begin,end). Returns 1 on a hit. */
static inline int lock_json_string(const char *begin, const char *end,
                                   const char *key, char *out, int out_len) {
    if (!begin || !end || !key || !out || out_len < 1) return 0;
    out[0] = '\0';

    char pat[48];
    const int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return 0;

    for (const char *p = begin; p && p < end; ) {
        const char *hit = strstr(p, pat);
        if (!hit || hit >= end) return 0;

        const char *c = hit + n;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        if (c < end && *c == ':') {
            c++;
            while (c < end && (*c == ' ' || *c == '\t')) c++;
            if (c < end && *c == '"') {
                const char *q1 = c + 1;
                const char *q2 = (const char *)memchr(q1, '"', (size_t)(end - q1));
                if (!q2) return 0;
                int len = (int)(q2 - q1);
                if (len > out_len - 1) len = out_len - 1;
                memcpy(out, q1, (size_t)len);
                out[len] = '\0';
                return 1;
            }
            return 0;
        }
        p = hit + n;
    }
    return 0;
}

/* Serialise into buf. Returns bytes written (excluding the NUL), or the length
 * it would have needed — callers pass SHADOW_PARAM_VALUE_LEN (64 KB) and the
 * worst case (16 lanes x 64 steps) is ~13 KB, so truncation does not arise in
 * practice; the bounds are respected regardless. */
static inline int lock_to_json(const lock_state_t *st, char *buf, int buf_len) {
    if (!st || !buf || buf_len < 2) return 0;

    int off = 0;
    off += snprintf(buf + off, (size_t)(buf_len - off),
                    "{\"pattern_len\":%d,\"rate_div\":%d,\"enabled\":%d,"
                    "\"division_table_version\":%d,\"lanes\":[",
                    st->pattern_len, st->rate_div, st->enabled, LFO_NUM_DIVISIONS);

    int first_lane = 1;
    for (int i = 0; i < st->lane_count && i < LOCK_MAX_LANES; i++) {
        const lock_lane_t *lane = &st->lanes[i];
        if (!lane->active || lane->mask == 0) continue;
        if (off >= buf_len) break;

        off += snprintf(buf + off, (size_t)(buf_len - off),
                        "%s{\"target\":\"%s\",\"param\":\"%s\",\"steps\":[",
                        first_lane ? "" : ",", lane->target, lane->param);
        first_lane = 0;

        int first_step = 1;
        for (int stp = 0; stp < LOCK_MAX_STEPS; stp++) {
            if (!lock_lane_has_step(lane, stp)) continue;
            if (off >= buf_len) break;
            off += snprintf(buf + off, (size_t)(buf_len - off), "%s[%d,%.6f]",
                            first_step ? "" : ",", stp, lane->values[stp]);
            first_step = 0;
        }
        if (off < buf_len) off += snprintf(buf + off, (size_t)(buf_len - off), "]}");
    }
    if (off < buf_len) off += snprintf(buf + off, (size_t)(buf_len - off), "]}");
    return off;
}

/* Parse the object `json` points into. `st` is fully initialised first, so a
 * missing key means the default and never the previous patch's value.
 * `migrate_division` maps a pre-27-entry rate_div; pass NULL to skip. */
static inline void lock_from_json(lock_state_t *st, const char *json,
                                  int (*migrate_division)(int)) {
    if (!st) return;
    lock_state_init(st);
    if (!json) return;

    const char *begin = strchr(json, '{');
    if (!begin) return;

    /* Bound to this object, so a "lanes" key elsewhere in a larger document
     * cannot be picked up by the scan below. */
    const char *end = begin + 1;
    int depth = 1;
    while (*end && depth > 0) {
        if (*end == '{') depth++;
        else if (*end == '}') depth--;
        if (depth > 0) end++;
    }

    double v = 0.0;
    if (lock_json_number(begin, end, "pattern_len", &v)) st->pattern_len = lock_clamp_pattern_len((int)v);
    if (lock_json_number(begin, end, "rate_div", &v))    st->rate_div = lock_clamp_rate_div((int)v);
    if (lock_json_number(begin, end, "enabled", &v))     st->enabled = ((int)v) ? 1 : 0;

    /* A rate_div written before the division table grew from 14 to 27 entries
     * names a different division now — the same migration the LFOs carry, for
     * the same table. Absence of the version field IS the old format. */
    if (!lock_json_number(begin, end, "division_table_version", &v) && migrate_division) {
        st->rate_div = lock_clamp_rate_div(migrate_division(st->rate_div));
    }

    const char *lanes = strstr(begin, "\"lanes\"");
    if (!lanes || lanes >= end) return;

    const char *p = strchr(lanes, '[');
    if (!p) return;
    p++;

    while (p < end) {
        const char *obj = (const char *)memchr(p, '{', (size_t)(end - p));
        if (!obj) break;

        const char *obj_end = obj + 1;
        int d = 1;
        while (obj_end < end && d > 0) {
            if (*obj_end == '{') d++;
            else if (*obj_end == '}') d--;
            if (d > 0) obj_end++;
        }
        if (obj_end >= end) break;

        char target[16], param[32];
        if (lock_json_string(obj, obj_end, "target", target, sizeof(target)) &&
            lock_json_string(obj, obj_end, "param", param, sizeof(param)) &&
            target[0] && param[0]) {

            lock_lane_t *lane = lock_alloc_lane(st, target, param);
            const char *steps = lane ? strstr(obj, "\"steps\"") : NULL;
            if (steps && steps < obj_end) {
                const char *sp = strchr(steps, '[');
                if (sp) {
                    sp++;                       /* into the outer array */
                    while (sp < obj_end) {
                        const char *pair = (const char *)memchr(sp, '[', (size_t)(obj_end - sp));
                        if (!pair) break;
                        const char *pair_end = (const char *)memchr(pair, ']', (size_t)(obj_end - pair));
                        if (!pair_end) break;

                        const int step = (int)strtol(pair + 1, NULL, 10);
                        const char *comma = (const char *)memchr(pair, ',', (size_t)(pair_end - pair));
                        if (comma && step >= 0 && step < LOCK_MAX_STEPS) {
                            lock_lane_set(lane, step, (float)strtod(comma + 1, NULL));
                        }
                        sp = pair_end + 1;
                    }
                }
            }
            /* A lane whose steps did not parse holds nothing, and would
             * otherwise occupy a slot and a modulation entry for the life of
             * the patch. */
            if (lane) lock_retire_lane_if_empty(st, lane);
        }

        p = obj_end + 1;
        while (p < end && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
        if (p >= end || *p == ']') break;
    }
}

#endif /* LOCK_COMMON_H */
