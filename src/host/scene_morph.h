/*
 * scene_morph.h -- Octatrack-style SCENES: the table, its verbs, the formula.
 *
 * A scene is a set of parameter LOCKS. Two scenes sit at the ends of one
 * crossfader (A and B), and every parameter locked in either end morphs:
 *
 *     va = A locks it ? A.value : base
 *     vb = B locks it ? B.value : base
 *     float  lerp(va, vb, x)
 *     int    round(lerp(va, vb, x))
 *     enum   x < 0.5 ? va : vb
 *
 * A parameter locked in NEITHER end has no contribution at all -- nothing is
 * pinned -- and `base` is the knob, read LIVE by the consumer, so turning the
 * knob mid-morph moves the unlocked end.
 *
 * ONE table per scope (a chain slot, the Master FX bus, a send bus). A PAIR is
 * a (target, param) that any of the 16 scenes locks; its `mask` says which.
 * The cap is on PAIRS, counted across all scenes, because that is what costs a
 * modulation target downstream -- a lock past it is REFUSED, never dropped.
 *
 * A pair records the MODULE that was at `target` when it was locked. The
 * consumer skips a pair whose module is not the one loaded now (DORMANT: not
 * applied, not deleted, alive again if that module returns). A lock written
 * under a DIFFERENT module replaces the pair outright -- the old values were
 * another module's numbers for a key that merely shares a name.
 *
 * Header-only and pure (no allocation, no I/O, no logging), because the chain
 * host and the shim both consume it on the SPI callback, and because the rule
 * must exist ONCE -- the transport grid and recall-quantize each got a copied
 * fact wrong in two places at once. tests/host/test_scene_morph.c runs it.
 *
 * WIRE FORMAT of the verbs (set_param values, never JSON on the callback):
 *     lock    "<n> <target> <param> <value> <module>"
 *     unlock  "<n> <target> <param>"
 *     clear   "<n>"
 *     copy    "<src> <dst>"
 *     load    one lock per line, same as `lock`; ALL-OR-NOTHING
 *     dump    what load reads, in table order
 * Scenes are 0-based on the wire; the UI shows n+1.
 */
#ifndef SCENE_MORPH_H
#define SCENE_MORPH_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SCENE_COUNT        16
#define SCENE_MAX_PAIRS    64
#define SCENE_NONE         0xFF
#define SCENE_TARGET_LEN   16
#define SCENE_PARAM_LEN    32
#define SCENE_MODULE_LEN   32

/* How the value is shaped between the ends. */
#define SCENE_KIND_FLOAT   0
#define SCENE_KIND_INT     1
#define SCENE_KIND_ENUM    2

/* Edit-arm flags, pushed with the crossfader. */
#define SCENE_EDIT_UNLOCK  0x01   /* Delete is held: an armed write UNLOCKS */

/* A refusal the UI flashes on the arm badge (shadow_control_t.scene_flash). */
#define SCENE_FLASH_NONE   0
#define SCENE_FLASH_FULL   1   /* the scope's 64-pair budget is used up */
#define SCENE_FLASH_NA     2   /* not a parameter the component declares */

/* scene_lock results. */
#define SCENE_OK           0
#define SCENE_ERR_ARGS    -1
#define SCENE_ERR_FULL    -2

typedef struct {
    char target[SCENE_TARGET_LEN];
    char param[SCENE_PARAM_LEN];
    char module[SCENE_MODULE_LEN];
    uint16_t mask;                  /* bit n = scene n locks this pair */
    float values[SCENE_COUNT];
} scene_pair_t;

typedef struct {
    int count;
    scene_pair_t pairs[SCENE_MAX_PAIRS];
} scene_table_t;

static inline int scene_valid_index(int n) { return n >= 0 && n < SCENE_COUNT; }

static inline int scene_copy_str(char *dst, size_t cap, const char *src) {
    if (!src || !src[0]) return 0;
    size_t n = strlen(src);
    if (n >= cap) return 0;          /* refuse, never truncate: a truncated key names a different parameter */
    memcpy(dst, src, n + 1);
    return 1;
}

static inline int scene_find(const scene_table_t *t, const char *target, const char *param) {
    if (!t || !target || !param) return -1;
    for (int i = 0; i < t->count; i++) {
        if (strcmp(t->pairs[i].target, target) == 0 && strcmp(t->pairs[i].param, param) == 0)
            return i;
    }
    return -1;
}

/* Remove pair i, keeping order (dump order is table order). */
static inline void scene_remove_pair(scene_table_t *t, int i) {
    if (!t || i < 0 || i >= t->count) return;
    for (int j = i; j < t->count - 1; j++) t->pairs[j] = t->pairs[j + 1];
    t->count--;
    memset(&t->pairs[t->count], 0, sizeof(t->pairs[0]));
}

static inline void scene_compact(scene_table_t *t) {
    for (int i = t->count - 1; i >= 0; i--)
        if (t->pairs[i].mask == 0) scene_remove_pair(t, i);
}

static inline int scene_lock(scene_table_t *t, int n, const char *target, const char *param,
                             float value, const char *module) {
    if (!t || !scene_valid_index(n) || !target || !param || !module) return SCENE_ERR_ARGS;
    if (!target[0] || !param[0] || !module[0] || !isfinite(value)) return SCENE_ERR_ARGS;
    if (strlen(target) >= SCENE_TARGET_LEN || strlen(param) >= SCENE_PARAM_LEN ||
        strlen(module) >= SCENE_MODULE_LEN) return SCENE_ERR_ARGS;

    int i = scene_find(t, target, param);
    if (i < 0) {
        if (t->count >= SCENE_MAX_PAIRS) return SCENE_ERR_FULL;
        i = t->count++;
        scene_pair_t *p = &t->pairs[i];
        memset(p, 0, sizeof(*p));
        scene_copy_str(p->target, sizeof(p->target), target);
        scene_copy_str(p->param, sizeof(p->param), param);
        scene_copy_str(p->module, sizeof(p->module), module);
    } else if (strcmp(t->pairs[i].module, module) != 0) {
        /* Another module's numbers under a shared key name: replace. */
        scene_pair_t *p = &t->pairs[i];
        p->mask = 0;
        memset(p->values, 0, sizeof(p->values));
        scene_copy_str(p->module, sizeof(p->module), module);
    }
    t->pairs[i].mask |= (uint16_t)(1u << n);
    t->pairs[i].values[n] = value;
    return SCENE_OK;
}

static inline int scene_unlock(scene_table_t *t, int n, const char *target, const char *param) {
    if (!t || !scene_valid_index(n)) return SCENE_ERR_ARGS;
    int i = scene_find(t, target, param);
    if (i < 0) return SCENE_OK;
    t->pairs[i].mask &= (uint16_t)~(1u << n);
    t->pairs[i].values[n] = 0.0f;
    if (t->pairs[i].mask == 0) scene_remove_pair(t, i);
    return SCENE_OK;
}

static inline int scene_clear(scene_table_t *t, int n) {
    if (!t || !scene_valid_index(n)) return SCENE_ERR_ARGS;
    for (int i = 0; i < t->count; i++) {
        t->pairs[i].mask &= (uint16_t)~(1u << n);
        t->pairs[i].values[n] = 0.0f;
    }
    scene_compact(t);
    return SCENE_OK;
}

/* dst becomes exactly src (locks src lacks are REMOVED from dst). */
static inline int scene_copy(scene_table_t *t, int src, int dst) {
    if (!t || !scene_valid_index(src) || !scene_valid_index(dst)) return SCENE_ERR_ARGS;
    if (src == dst) return SCENE_OK;
    for (int i = 0; i < t->count; i++) {
        scene_pair_t *p = &t->pairs[i];
        if (p->mask & (1u << src)) {
            p->mask |= (uint16_t)(1u << dst);
            p->values[dst] = p->values[src];
        } else {
            p->mask &= (uint16_t)~(1u << dst);
            p->values[dst] = 0.0f;
        }
    }
    scene_compact(t);
    return SCENE_OK;
}

/* Number of locks scene n holds. */
static inline int scene_lock_count(const scene_table_t *t, int n) {
    if (!t || !scene_valid_index(n)) return 0;
    int c = 0;
    for (int i = 0; i < t->count; i++) if (t->pairs[i].mask & (1u << n)) c++;
    return c;
}

/* ---- wire parsing ------------------------------------------------------ */

/* Next whitespace-delimited token from *s into out; 0 when none or too long. */
static inline int scene_token(const char **s, char *out, size_t cap) {
    const char *p = *s;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\n' || *p == '\r') { *s = p; return 0; }
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        if (n + 1 >= cap) return 0;
        out[n++] = *p++;
    }
    out[n] = '\0';
    *s = p;
    return 1;
}

static inline int scene_parse_int(const char *tok, int *out) {
    char *end = NULL;
    long v = strtol(tok, &end, 10);
    if (!end || end == tok || *end) return 0;
    *out = (int)v;
    return 1;
}

static inline int scene_parse_float(const char *tok, float *out) {
    char *end = NULL;
    float v = strtof(tok, &end);
    if (!end || end == tok || *end || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* Only trailing whitespace may follow the last token. */
static inline int scene_at_line_end(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    return *s == '\0' || *s == '\n';
}

/* Parse one lock line: "<n> <target> <param> <value> <module>". */
static inline int scene_parse_lock(const char **s, int *n, char *target, char *param,
                                   float *value, char *module) {
    char tok[48];
    if (!scene_token(s, tok, sizeof(tok)) || !scene_parse_int(tok, n) || !scene_valid_index(*n)) return 0;
    if (!scene_token(s, target, SCENE_TARGET_LEN)) return 0;
    if (!scene_token(s, param, SCENE_PARAM_LEN)) return 0;
    if (!scene_token(s, tok, sizeof(tok)) || !scene_parse_float(tok, value)) return 0;
    if (!scene_token(s, module, SCENE_MODULE_LEN)) return 0;
    return scene_at_line_end(*s);
}

static inline int scene_apply_lock_verb(scene_table_t *t, const char *val) {
    if (!val) return SCENE_ERR_ARGS;
    int n; float v;
    char target[SCENE_TARGET_LEN], param[SCENE_PARAM_LEN], module[SCENE_MODULE_LEN];
    const char *s = val;
    if (!scene_parse_lock(&s, &n, target, param, &v, module)) return SCENE_ERR_ARGS;
    return scene_lock(t, n, target, param, v, module);
}

static inline int scene_apply_unlock_verb(scene_table_t *t, const char *val) {
    if (!val) return SCENE_ERR_ARGS;
    char tok[16], target[SCENE_TARGET_LEN], param[SCENE_PARAM_LEN];
    int n;
    const char *s = val;
    if (!scene_token(&s, tok, sizeof(tok)) || !scene_parse_int(tok, &n)) return SCENE_ERR_ARGS;
    if (!scene_token(&s, target, sizeof(target)) || !scene_token(&s, param, sizeof(param))) return SCENE_ERR_ARGS;
    if (!scene_at_line_end(s)) return SCENE_ERR_ARGS;
    return scene_unlock(t, n, target, param);
}

static inline int scene_apply_clear_verb(scene_table_t *t, const char *val) {
    if (!val) return SCENE_ERR_ARGS;
    char tok[16];
    int n;
    const char *s = val;
    if (!scene_token(&s, tok, sizeof(tok)) || !scene_parse_int(tok, &n) || !scene_at_line_end(s))
        return SCENE_ERR_ARGS;
    return scene_clear(t, n);
}

static inline int scene_apply_copy_verb(scene_table_t *t, const char *val) {
    if (!val) return SCENE_ERR_ARGS;
    char a[16], b[16];
    int src, dst;
    const char *s = val;
    if (!scene_token(&s, a, sizeof(a)) || !scene_parse_int(a, &src)) return SCENE_ERR_ARGS;
    if (!scene_token(&s, b, sizeof(b)) || !scene_parse_int(b, &dst)) return SCENE_ERR_ARGS;
    if (!scene_at_line_end(s)) return SCENE_ERR_ARGS;
    return scene_copy(t, src, dst);
}

/*
 * ALL-OR-NOTHING: parsed into `scratch` first and copied over `dst` only when
 * every line is good. A half-applied load is a bank nobody wrote. `scratch` is
 * the CALLER's (a static on the callback thread), because a table is ~10 KB
 * and this runs on the SPI callback's stack. An empty text is a valid, empty
 * bank.
 */
static inline int scene_load(scene_table_t *dst, scene_table_t *scratch, const char *text) {
    if (!dst || !scratch || !text) return SCENE_ERR_ARGS;
    memset(scratch, 0, sizeof(*scratch));
    const char *s = text;
    while (*s) {
        /* skip blank lines */
        const char *probe = s;
        while (*probe == ' ' || *probe == '\t' || *probe == '\r') probe++;
        if (*probe == '\n') { s = probe + 1; continue; }
        if (!*probe) break;

        int n; float v;
        char target[SCENE_TARGET_LEN], param[SCENE_PARAM_LEN], module[SCENE_MODULE_LEN];
        if (!scene_parse_lock(&s, &n, target, param, &v, module)) return SCENE_ERR_ARGS;
        int rc = scene_lock(scratch, n, target, param, v, module);
        if (rc != SCENE_OK) return rc;
        while (*s && *s != '\n') s++;
        if (*s == '\n') s++;
    }
    memcpy(dst, scratch, sizeof(*dst));
    return SCENE_OK;
}

/*
 * One lock per line, pair order then scene order. Returns bytes written
 * (excluding NUL), or -1 when it does not fit -- never a truncated bank, which
 * would read back as a valid smaller one.
 */
static inline int scene_dump(const scene_table_t *t, char *buf, int cap) {
    if (!t || !buf || cap < 1) return -1;
    int off = 0;
    buf[0] = '\0';
    for (int i = 0; i < t->count; i++) {
        const scene_pair_t *p = &t->pairs[i];
        for (int n = 0; n < SCENE_COUNT; n++) {
            if (!(p->mask & (1u << n))) continue;
            int w = snprintf(buf + off, (size_t)(cap - off), "%d %s %s %.9g %s\n",
                             n, p->target, p->param, (double)p->values[n], p->module);
            if (w < 0 || w >= cap - off) { buf[0] = '\0'; return -1; }
            off += w;
        }
    }
    return off;
}

/* ---- the formula -------------------------------------------------------- */

/*
 * Resolve one pair for the ends (a, b). Returns 0 when neither end locks it
 * -- NO contribution, the parameter is left alone. SCENE_NONE is a valid end
 * and locks nothing, so (NONE, n) morphs base -> scene n.
 */
static inline int scene_resolve(const scene_pair_t *p, int a, int b,
                                int *has_a, float *va, int *has_b, float *vb) {
    *has_a = (a != SCENE_NONE && scene_valid_index(a) && (p->mask & (1u << a))) ? 1 : 0;
    *has_b = (b != SCENE_NONE && scene_valid_index(b) && (p->mask & (1u << b))) ? 1 : 0;
    *va = *has_a ? p->values[a] : 0.0f;
    *vb = *has_b ? p->values[b] : 0.0f;
    return *has_a || *has_b;
}

static inline float scene_clamp01(float x) {
    if (!(x >= 0.0f)) return 0.0f;       /* also catches NaN */
    if (x > 1.0f) return 1.0f;
    return x;
}

static inline float scene_morph_value(int has_a, float va, int has_b, float vb,
                                      float base, float x, int kind) {
    float a = has_a ? va : base;
    float b = has_b ? vb : base;
    x = scene_clamp01(x);
    if (kind == SCENE_KIND_ENUM) return x < 0.5f ? a : b;
    float v = a + (b - a) * x;
    if (kind == SCENE_KIND_INT) v = roundf(v);
    return v;
}

/* Fader position <-> the uint16 the control struct carries. */
static inline float scene_xfade_from_q(uint16_t q) { return (float)q / 65535.0f; }
static inline uint16_t scene_xfade_to_q(float x) {
    x = scene_clamp01(x);
    return (uint16_t)lroundf(x * 65535.0f);
}

/*
 * THE EDIT-ARM WRITE FILTER, the pure half: may a write to this SUBKEY (the
 * part after "synth:" / "fx2:" / "midi_fx1:") become a lock? The caller also
 * requires the key to be one the component's chain_params declares -- this
 * only rules out what is never a knob however it is declared: module
 * identity, whole-state blobs, presets, bypass, and every suffixed view
 * (":effective", ":base", ":held"), which contain a colon.
 * Mirrors control_target.mjs NOT_PARAMS so Learn and Scenes agree.
 */
static inline int scene_edit_subkey_eligible(const char *subkey) {
    if (!subkey || !subkey[0]) return 0;
    if (strlen(subkey) >= SCENE_PARAM_LEN) return 0;
    if (strchr(subkey, ':') || strchr(subkey, ' ')) return 0;
    static const char *const not_params[] = {
        "module", "state", "preset", "preset_name", "bypassed", "plugin_id",
        "load", "save", "ui_hierarchy", "chain_params", "ui_pages", "view",
        "name", "error", NULL
    };
    for (int i = 0; not_params[i]; i++)
        if (strcmp(subkey, not_params[i]) == 0) return 0;
    return 1;
}

#endif /* SCENE_MORPH_H */
