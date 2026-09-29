/*
 * shadow_scene_bus.c -- SCENES on Master FX and the send buses.
 * See shadow_scene_bus.h for the model; scene_morph.h for the formula.
 */
#include "shadow_scene_bus.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bus_mix.h"     /* BUS_MIX_SEND_LEVEL_MAX */
#include "lfo_common.h"  /* LFO_NUM_SHAPES, LFO_NUM_DIVISIONS */

#define SCENE_BUS_REVALIDATE_FRAMES 32
#define SCENE_BUS_INT_MIN_INTERVAL_MS 50   /* as chain_mod's MOD_INT_ENUM_MIN_INTERVAL_MS */

typedef struct {
    int active;
    char target[SCENE_TARGET_LEN];
    char param[SCENE_PARAM_LEN];
    char module[SCENE_MODULE_LEN];   /* whose base this is */
    float base;
    int kind;
    float min, max;
    float last;
    int has_last;
    uint64_t last_ms;
    scene_takeover_t takeover;       /* a live knob turn (scene_morph.h) */
} scene_drive_t;

typedef struct {
    scene_table_t table;
    scene_drive_t drives[SCENE_MAX_PAIRS];
    uint16_t rev;
    int dirty;
    int revalidate;
} scene_bus_t;

static scene_bus_t s_bus[SCENE_BUS_SCOPES];
static scene_table_t s_load_scratch;
static const scene_bus_io_t *s_io = NULL;
static const scene_host_io_t *s_host = NULL;
static uint8_t s_flash = SCENE_FLASH_NONE;

static void changed(int scope);
static scene_drive_t *find_drive(scene_bus_t *bus, const char *target, const char *param);

static uint8_t s_a = SCENE_NONE, s_b = SCENE_NONE, s_edit = SCENE_NONE, s_edit_flags = 0;
static float s_x = 0.0f;

static uint64_t scene_bus_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void shadow_scene_bus_bind(const scene_bus_io_t *io) { s_io = io; }
void shadow_scene_host_bind(const scene_host_io_t *io) { s_host = io; }

void shadow_scene_bus_reset(void) {
    /* A host override left on would outlive its table. */
    if (s_host) {
        for (int i = 0; i < SCENE_MAX_PAIRS; i++) {
            scene_drive_t *d = &s_bus[SCENE_HOST_SCOPE].drives[i];
            if (d->active) s_host->apply(d->target, d->param, 0, 0.0f);
        }
    }
    memset(s_bus, 0, sizeof(s_bus));
    for (int i = 0; i < SCENE_BUS_SCOPES; i++) s_bus[i].dirty = 1;
    s_flash = SCENE_FLASH_NONE;
    s_a = s_b = s_edit = SCENE_NONE;
    s_x = 0.0f;
}

int shadow_scene_bus_scope(const char *prefix, int n) {
    if (n == 9 && strncmp(prefix, "master_fx", 9) == 0) return 0;
    if (n == 5 && strncmp(prefix, "send1", 5) == 0) return 1;
    if (n == 5 && strncmp(prefix, "send2", 5) == 0) return 2;
    if (n == 4 && strncmp(prefix, "host", 4) == 0) return SCENE_HOST_SCOPE;
    return -1;
}

static int valid_scope(int scope) {
    if (scope == SCENE_HOST_SCOPE) return s_host != NULL;
    return scope >= 0 && scope < SCENE_BUS_SCOPES && s_io;
}

/* The positional verbs are the plugin buses' only. */
static int valid_bus_scope(int scope) { return scope >= 0 && scope < SCENE_HOST_SCOPE && s_io; }

/* ---- host settings -------------------------------------------------------- */

typedef struct { const char *param; int kind; float min, max; } host_meta_row_t;
static const host_meta_row_t HOST_SLOT[] = {
    { "volume", SCENE_KIND_FLOAT, 0.0f, 4.0f },
    { "pan",    SCENE_KIND_FLOAT, -1.0f, 1.0f },
    { NULL, 0, 0, 0 },
};
static const host_meta_row_t HOST_SEND1[] = {
    { "return",   SCENE_KIND_INT, 0, BUS_MIX_SEND_LEVEL_MAX },
    { "to_send2", SCENE_KIND_INT, 0, BUS_MIX_SEND_LEVEL_MAX },
    { NULL, 0, 0, 0 },
};
static const host_meta_row_t HOST_SEND2[] = {
    { "return", SCENE_KIND_INT, 0, BUS_MIX_SEND_LEVEL_MAX },
    { NULL, 0, 0, 0 },
};
static const host_meta_row_t HOST_LFO[] = {
    { "enabled",      SCENE_KIND_ENUM, 0, 1 },
    { "shape",        SCENE_KIND_ENUM, 0, LFO_NUM_SHAPES - 1 },
    { "rate_hz",      SCENE_KIND_FLOAT, 0.1f, 20.0f },
    { "rate_div",     SCENE_KIND_ENUM, 0, LFO_NUM_DIVISIONS - 1 },
    { "sync",         SCENE_KIND_ENUM, 0, 1 },
    { "depth",        SCENE_KIND_FLOAT, -1.0f, 1.0f },
    { "polarity",     SCENE_KIND_ENUM, 0, 1 },
    { "phase_offset", SCENE_KIND_FLOAT, 0.0f, 1.0f },
    { NULL, 0, 0, 0 },
};

int scene_host_meta(const char *target, const char *param, scene_bus_meta_t *out) {
    if (!target || !param) return 0;
    const host_meta_row_t *t = NULL;
    if (strncmp(target, "slot", 4) == 0 && target[4] >= '1' && target[4] <= '4' && !target[5]) t = HOST_SLOT;
    else if (strcmp(target, "send1") == 0) t = HOST_SEND1;
    else if (strcmp(target, "send2") == 0) t = HOST_SEND2;
    else if (strcmp(target, "mfx_lfo1") == 0 || strcmp(target, "mfx_lfo2") == 0) t = HOST_LFO;
    if (!t) return 0;
    for (int i = 0; t[i].param; i++) {
        if (strcmp(t[i].param, param) != 0) continue;
        if (out) {
            memset(out, 0, sizeof(*out));
            out->kind = t[i].kind;
            out->min = t[i].min;
            out->max = t[i].max;
            out->def = t[i].min;
        }
        return 1;
    }
    return 0;
}

static float host_clamp(const scene_bus_meta_t *m, float v) {
    if (v < m->min) v = m->min;
    if (v > m->max) v = m->max;
    return m->kind == SCENE_KIND_FLOAT ? v : roundf(v);
}

/* Every frame: overrides for what contributes, off for what stopped. */
static void tick_host(int a, int b, float x) {
    if (!s_host) return;
    scene_bus_t *bus = &s_bus[SCENE_HOST_SCOPE];
    for (int i = 0; i < SCENE_MAX_PAIRS; i++) {
        scene_drive_t *d = &bus->drives[i];
        if (!d->active) continue;
        int pi = scene_find(&bus->table, d->target, d->param);
        int ha, hb; float va, vb;
        if (pi >= 0 && scene_resolve(&bus->table.pairs[pi], a, b, &ha, &va, &hb, &vb)) continue;
        s_host->apply(d->target, d->param, 0, 0.0f);
        memset(d, 0, sizeof(*d));
    }
    for (int i = 0; i < bus->table.count; i++) {
        const scene_pair_t *p = &bus->table.pairs[i];
        scene_bus_meta_t m;
        if (!scene_host_meta(p->target, p->param, &m)) continue;
        int ha, hb; float va, vb;
        if (!scene_resolve(p, a, b, &ha, &va, &hb, &vb)) continue;
        float base = m.def;
        if (!s_host->get(p->target, p->param, &base)) continue;
        scene_drive_t *d = find_drive(bus, p->target, p->param);
        if (d && scene_takeover_expired(&d->takeover, x)) d->takeover.on = 0;
        const float v = host_clamp(&m, (d && d->takeover.on)
            ? scene_takeover_value(&d->takeover, ha ? va : base, hb ? vb : base, x, m.kind)
            : scene_morph_value(ha, va, hb, vb, base, x, m.kind));
        if (!d) {
            for (int j = 0; j < SCENE_MAX_PAIRS && !d; j++) if (!bus->drives[j].active) d = &bus->drives[j];
            if (!d) continue;
            memset(d, 0, sizeof(*d));
            d->active = 1;
            memcpy(d->target, p->target, sizeof(d->target));
            memcpy(d->param, p->param, sizeof(d->param));
            d->kind = m.kind;
        }
        if (d->has_last && fabsf(v - d->last) < 1e-6f) continue;
        s_host->apply(p->target, p->param, 1, v);
        d->last = v;
        d->has_last = 1;
    }
}

int shadow_scene_host_edit_write(const char *target, const char *param, const char *val) {
    if (!s_host || s_edit == SCENE_NONE || !target || !param || !val) return 0;
    scene_bus_meta_t m;
    if (!scene_host_meta(target, param, &m)) return 0;
    char *end = NULL;
    float v = strtof(val, &end);
    if (!end || end == val) return 0;
    v = host_clamp(&m, v);
    scene_bus_t *bus = &s_bus[SCENE_HOST_SCOPE];
    if (s_edit_flags & SCENE_EDIT_UNLOCK) {
        if (scene_unlock(&bus->table, s_edit, target, param) == SCENE_OK) {
            changed(SCENE_HOST_SCOPE);
            tick_host(s_edit, SCENE_NONE, 0.0f);
        }
        return 1;
    }
    int rc = scene_lock(&bus->table, s_edit, target, param, v, SCENE_HOST_MODULE);
    if (rc == SCENE_ERR_FULL) { s_flash = SCENE_FLASH_FULL; return 0; }
    if (rc != SCENE_OK) return 0;
    changed(SCENE_HOST_SCOPE);
    tick_host(s_edit, SCENE_NONE, 0.0f);
    return 1;
}

void shadow_scene_host_note_write(const char *target, const char *param, const char *val) {
    if (!s_host || s_edit != SCENE_NONE || !target || !param || !val) return;
    scene_bus_t *bus = &s_bus[SCENE_HOST_SCOPE];
    scene_drive_t *d = find_drive(bus, target, param);
    if (!d) return;
    scene_bus_meta_t m;
    if (!scene_host_meta(target, param, &m)) return;
    char *end = NULL;
    float nb = strtof(val, &end);
    if (!end || end == val) return;
    nb = host_clamp(&m, nb);
    float old = m.def;
    if (!s_host->get(target, param, &old)) return;
    const float heard = d->has_last ? d->last : old;
    d->takeover.k = scene_takeover_k(heard, old, nb, m.kind, m.min, m.max);
    d->takeover.x0 = s_x;
    d->takeover.on = 1;
    d->has_last = 0;
}

int shadow_scene_host_read(const char *target, const char *param, char *buf, int len) {
    if (!s_host || s_edit == SCENE_NONE || !target || !param || !buf || len < 2) return -1;
    scene_bus_meta_t m;
    if (!scene_host_meta(target, param, &m)) return -1;
    scene_bus_t *bus = &s_bus[SCENE_HOST_SCOPE];
    int i = scene_find(&bus->table, target, param);
    if (i < 0 || !(bus->table.pairs[i].mask & (1u << s_edit))) return -1;
    float v = bus->table.pairs[i].values[s_edit];
    return m.kind == SCENE_KIND_FLOAT ? snprintf(buf, len, "%.4f", v)
                                      : snprintf(buf, len, "%d", (int)lroundf(v));
}

/* ---- chain_params reading ------------------------------------------------
 *
 * A flat scan, like the other chain_params readers in the shim: find the
 * object whose "key" is `param`, then read its fields inside that object's
 * braces. String-aware enough that a brace inside a name does not end it. */

static const char *skip_string(const char *p) {
    /* p at opening quote; returns just past the closing one */
    p++;
    while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
    return *p ? p + 1 : p;
}

static const char *object_end(const char *start) {
    int depth = 0;
    for (const char *p = start; *p; ) {
        if (*p == '"') { p = skip_string(p); continue; }
        if (*p == '{') depth++;
        else if (*p == '}') { if (--depth == 0) return p; }
        p++;
    }
    return NULL;
}

/* The value text after "name": inside [start, end), or NULL. */
static const char *field(const char *start, const char *end, const char *name) {
    char pat[48];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", name);
    if (n <= 0 || n >= (int)sizeof(pat)) return NULL;
    for (const char *p = start; p && p < end; ) {
        const char *hit = strstr(p, pat);
        if (!hit || hit >= end) return NULL;
        const char *q = hit + n;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (q < end && *q == ':') {
            q++;
            while (q < end && isspace((unsigned char)*q)) q++;
            return q;
        }
        p = hit + n;
    }
    return NULL;
}

static int field_string_eq(const char *v, const char *end, const char *want) {
    if (!v || *v != '"') return 0;
    size_t n = strlen(want);
    return (v + 1 + n < end) && strncmp(v + 1, want, n) == 0 && v[1 + n] == '"';
}

static const char *find_param_object(const char *json, const char *param, const char **obj_end) {
    if (!json || !param || !param[0]) return NULL;
    for (const char *p = json; (p = strchr(p, '{')) != NULL; ) {
        const char *e = object_end(p);
        if (!e) return NULL;
        const char *k = field(p, e, "key");
        if (field_string_eq(k, e, param)) { *obj_end = e; return p; }
        p = p + 1;
    }
    return NULL;
}

int scene_bus_param_meta(const char *json, const char *param, scene_bus_meta_t *out) {
    const char *e = NULL;
    const char *o = find_param_object(json, param, &e);
    if (!o || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->kind = SCENE_KIND_FLOAT;
    out->min = 0.0f;
    out->max = 1.0f;
    const char *t = field(o, e, "type");
    const char *opts = field(o, e, "options");
    if (opts && *opts == '[') {
        int count = 0;
        for (const char *p = opts + 1; p < e && *p != ']'; ) {
            if (*p == '"') { count++; p = skip_string(p); continue; }
            p++;
        }
        out->option_count = count;
    }
    if (field_string_eq(t, e, "enum") || (out->option_count > 0 && !field_string_eq(t, e, "int"))) {
        out->kind = SCENE_KIND_ENUM;
        out->min = 0.0f;
        out->max = out->option_count > 0 ? (float)(out->option_count - 1) : 0.0f;
    } else if (field_string_eq(t, e, "int")) {
        out->kind = SCENE_KIND_INT;
    }
    const char *v;
    if (out->kind != SCENE_KIND_ENUM || out->option_count == 0) {
        if ((v = field(o, e, "min")) != NULL) out->min = strtof(v, NULL);
        if ((v = field(o, e, "max")) != NULL) out->max = strtof(v, NULL);
    }
    out->def = out->min;
    if ((v = field(o, e, "default")) != NULL && *v != '"') out->def = strtof(v, NULL);
    if (out->max < out->min) { float tmp = out->max; out->max = out->min; out->min = tmp; }
    return 1;
}

int scene_bus_option_index(const char *json, const char *param, const char *name) {
    const char *e = NULL;
    const char *o = find_param_object(json, param, &e);
    if (!o || !name) return -1;
    const char *opts = field(o, e, "options");
    if (!opts || *opts != '[') return -1;
    int idx = 0;
    size_t n = strlen(name);
    for (const char *p = opts + 1; p < e && *p != ']'; ) {
        if (*p == '"') {
            if (strncmp(p + 1, name, n) == 0 && p[1 + n] == '"') return idx;
            idx++;
            p = skip_string(p);
            continue;
        }
        p++;
    }
    return -1;
}

/* ---- positions ----------------------------------------------------------- */

static int target_pos(const char *target) {
    if (strncmp(target, "fx", 2) != 0 || !target[2]) return -1;
    for (const char *d = target + 2; *d; d++) if (!isdigit((unsigned char)*d)) return -1;
    return atoi(target + 2) - 1;
}

static void *slot_for(int scope, const char *target) {
    int pos = target_pos(target);
    if (pos < 0 || pos >= s_io->positions(scope)) return NULL;
    return s_io->slot_at(scope, pos);
}

/* A pair is LIVE when its module is the one loaded at its position. */
static void *live_slot(int scope, const scene_pair_t *p) {
    void *slot = slot_for(scope, p->target);
    if (!slot) return NULL;
    const char *m = s_io->module_id(slot);
    return (m && strcmp(m, p->module) == 0) ? slot : NULL;
}

static int parse_value(void *slot, const char *param, const char *val, const scene_bus_meta_t *meta,
                       float *out) {
    char *end = NULL;
    float v = strtof(val, &end);
    if (end && end != val) { *out = v; return 1; }
    if (meta->kind == SCENE_KIND_ENUM) {
        int i = scene_bus_option_index(s_io->chain_params(slot), param, val);
        if (i >= 0) { *out = (float)i; return 1; }
    }
    return 0;
}

static void write_value(void *slot, const char *param, float v, int kind) {
    char buf[32];
    if (kind == SCENE_KIND_FLOAT) snprintf(buf, sizeof(buf), "%.6f", v);
    else snprintf(buf, sizeof(buf), "%d", (int)lroundf(v));
    s_io->set_param(slot, param, buf);
}

static scene_drive_t *find_drive(scene_bus_t *bus, const char *target, const char *param) {
    for (int i = 0; i < SCENE_MAX_PAIRS; i++) {
        scene_drive_t *d = &bus->drives[i];
        if (d->active && strcmp(d->target, target) == 0 && strcmp(d->param, param) == 0) return d;
    }
    return NULL;
}

/* Hand the parameter back to its knob -- but only to the module the base was
 * taken from. After a swap the position holds a different module, and writing
 * the old one's number into it would be a write nobody made. */
static void release_drive(int scope, scene_drive_t *d) {
    void *slot = slot_for(scope, d->target);
    if (slot && d->has_last) {
        const char *m = s_io->module_id(slot);
        if (m && strcmp(m, d->module) == 0) write_value(slot, d->param, d->base, d->kind);
    }
    memset(d, 0, sizeof(*d));
}

static scene_drive_t *engage_drive(scene_bus_t *bus, void *slot, const scene_pair_t *p,
                                   const scene_bus_meta_t *meta) {
    scene_drive_t *d = find_drive(bus, p->target, p->param);
    if (d) return d;
    for (int i = 0; i < SCENE_MAX_PAIRS; i++) {
        if (bus->drives[i].active) continue;
        d = &bus->drives[i];
        memset(d, 0, sizeof(*d));
        d->active = 1;
        memcpy(d->target, p->target, sizeof(d->target));
        memcpy(d->param, p->param, sizeof(d->param));
        memcpy(d->module, p->module, sizeof(d->module));
        d->kind = meta->kind;
        d->min = meta->min;
        d->max = meta->max;
        /* The base is the knob as it stands when the scene takes over. */
        char buf[64];
        float base = meta->def;
        if (s_io->get_param(slot, p->param, buf, sizeof(buf)) > 0) {
            float v;
            if (parse_value(slot, p->param, buf, meta, &v)) base = v;
        }
        d->base = base < meta->min ? meta->min : base > meta->max ? meta->max : base;
        return d;
    }
    return NULL;
}

static void tick_bus(int scope, int a, int b, float x) {
    scene_bus_t *bus = &s_bus[scope];
    if (++bus->revalidate >= SCENE_BUS_REVALIDATE_FRAMES) { bus->revalidate = 0; bus->dirty = 1; }
    if (!bus->dirty) return;
    bus->dirty = 0;

    /* 1. Release every drive whose pair no longer contributes. */
    for (int i = 0; i < SCENE_MAX_PAIRS; i++) {
        scene_drive_t *d = &bus->drives[i];
        if (!d->active) continue;
        int pi = scene_find(&bus->table, d->target, d->param);
        int keep = 0;
        if (pi >= 0 && live_slot(scope, &bus->table.pairs[pi])) {
            int ha, hb; float va, vb;
            keep = scene_resolve(&bus->table.pairs[pi], a, b, &ha, &va, &hb, &vb);
        }
        if (!keep) release_drive(scope, d);
    }

    /* 2. Drive every live, contributing pair. */
    uint64_t now = 0;
    for (int i = 0; i < bus->table.count; i++) {
        const scene_pair_t *p = &bus->table.pairs[i];
        void *slot = live_slot(scope, p);
        if (!slot) continue;
        int ha, hb; float va, vb;
        if (!scene_resolve(p, a, b, &ha, &va, &hb, &vb)) continue;
        /* The JSON is read only to ENGAGE a drive: an engaged one already
         * carries kind and range, and a fader sweep dirties every frame --
         * scanning chain_params per pair per frame on the callback would be
         * the expensive part of the whole feature. */
        scene_drive_t *d = find_drive(bus, p->target, p->param);
        if (!d) {
            scene_bus_meta_t meta;
            if (!scene_bus_param_meta(s_io->chain_params(slot), p->param, &meta)) continue;
            d = engage_drive(bus, slot, p, &meta);
            if (!d) continue;
        }
        if (scene_takeover_expired(&d->takeover, x)) d->takeover.on = 0;
        float v = d->takeover.on
            ? scene_takeover_value(&d->takeover, ha ? va : d->base, hb ? vb : d->base, x, d->kind)
            : scene_morph_value(ha, va, hb, vb, d->base, x, d->kind);
        if (v < d->min) v = d->min;
        if (v > d->max) v = d->max;
        if (d->has_last) {
            if (d->kind == SCENE_KIND_FLOAT ? fabsf(v - d->last) < 1e-6f
                                           : lroundf(v) == lroundf(d->last)) continue;
        }
        if (d->kind != SCENE_KIND_FLOAT) {
            if (!now) now = scene_bus_now_ms();
            if (d->has_last && now - d->last_ms < SCENE_BUS_INT_MIN_INTERVAL_MS) {
                bus->dirty = 1;      /* not dropped: retried next frame */
                continue;
            }
            d->last_ms = now;
        }
        write_value(slot, p->param, v, d->kind);
        d->last = v;
        d->has_last = 1;
    }
}

void shadow_scene_bus_tick(uint8_t a, uint8_t b, float x, uint8_t edit, uint8_t edit_flags) {
    if (!s_io && !s_host) return;
    s_edit_flags = edit_flags;
    if (a != s_a || b != s_b || edit != s_edit) {
        /* Another scene, or an edit: every live anchor goes. */
        for (int i = 0; i < SCENE_BUS_SCOPES; i++)
            for (int j = 0; j < SCENE_MAX_PAIRS; j++) {
                /* has_last is left alone: it is what tells a RELEASE the
                 * plugin holds a scene value and must be handed the knob. */
                s_bus[i].drives[j].takeover.on = 0;
            }
    }
    if (a != s_a || b != s_b || edit != s_edit || fabsf(x - s_x) > 1e-6f) {
        s_a = a; s_b = b; s_edit = edit; s_x = x;
        for (int i = 0; i < SCENE_BUS_SCOPES; i++) s_bus[i].dirty = 1;
    }
    int ea = a, eb = b;
    float ex = x;
    if (edit != SCENE_NONE) { ea = edit; eb = SCENE_NONE; ex = 0.0f; }
    for (int i = 0; i < SCENE_HOST_SCOPE; i++) if (s_io) tick_bus(i, ea, eb, ex);
    tick_host(ea, eb, ex);
}

uint16_t shadow_scene_bus_rev(void) {
    uint16_t r = 0;
    for (int i = 0; i < SCENE_BUS_SCOPES; i++) r = (uint16_t)(r + s_bus[i].rev);
    return r;
}

uint8_t shadow_scene_bus_take_flash(void) {
    uint8_t f = s_flash;
    s_flash = SCENE_FLASH_NONE;
    return f;
}

static void changed(int scope) { s_bus[scope].rev++; s_bus[scope].dirty = 1; }

int shadow_scene_bus_set_verb(int scope, const char *verb, const char *val) {
    if (!valid_scope(scope) || !verb) return 0;
    scene_table_t *t = &s_bus[scope].table;
    int rc;
    if (strcmp(verb, "lock") == 0) {
        rc = scene_apply_lock_verb(t, val);
        if (rc == SCENE_ERR_FULL) s_flash = SCENE_FLASH_FULL;
    } else if (strcmp(verb, "unlock") == 0) rc = scene_apply_unlock_verb(t, val);
    else if (strcmp(verb, "clear") == 0) rc = scene_apply_clear_verb(t, val);
    else if (strcmp(verb, "copy") == 0) rc = scene_apply_copy_verb(t, val);
    else if (strcmp(verb, "load") == 0) rc = scene_load(t, &s_load_scratch, val ? val : "");
    else return 0;
    if (rc == SCENE_OK) changed(scope);
    return 1;
}

int shadow_scene_bus_get_verb(int scope, const char *verb, char *buf, int len) {
    if (!valid_scope(scope) || !verb || !buf || len < 2) return -1;
    scene_table_t *t = &s_bus[scope].table;
    if (strcmp(verb, "dump") == 0) return scene_dump(t, buf, len);
    if (strcmp(verb, "count") == 0) return snprintf(buf, len, "%d", t->count);
    if (strcmp(verb, "rev") == 0) return snprintf(buf, len, "%u", (unsigned)s_bus[scope].rev);
    if (strcmp(verb, "locks") == 0) {
        int off = 0;
        for (int n = 0; n < SCENE_COUNT; n++) {
            int w = snprintf(buf + off, (size_t)(len - off), n ? ",%d" : "%d", scene_lock_count(t, n));
            if (w < 0 || w >= len - off) return -1;
            off += w;
        }
        return off;
    }
    return -1;
}

int shadow_scene_bus_edit_write(int scope, int pos, const char *param, const char *val) {
    if (!valid_bus_scope(scope) || s_edit == SCENE_NONE || !param || !val) return 0;
    if (!scene_edit_subkey_eligible(param)) return 0;
    if (pos < 0 || pos >= s_io->positions(scope)) return 0;
    void *slot = s_io->slot_at(scope, pos);
    if (!slot) return 0;
    const char *module = s_io->module_id(slot);
    if (!module || !module[0]) return 0;
    scene_bus_meta_t meta;
    if (!scene_bus_param_meta(s_io->chain_params(slot), param, &meta)) {
        s_flash = SCENE_FLASH_NA;
        return 0;
    }
    float v;
    if (!parse_value(slot, param, val, &meta, &v)) return 0;
    if (v < meta.min) v = meta.min;
    if (v > meta.max) v = meta.max;
    if (meta.kind != SCENE_KIND_FLOAT) v = roundf(v);
    char target[SCENE_TARGET_LEN];
    snprintf(target, sizeof(target), "fx%d", pos + 1);
    /* Delete held: the turn picks a parameter to REMOVE from the armed scene. */
    if (s_edit_flags & SCENE_EDIT_UNLOCK) {
        if (scene_unlock(&s_bus[scope].table, s_edit, target, param) == SCENE_OK) {
            changed(scope);
            tick_bus(scope, s_edit, SCENE_NONE, 0.0f);
        }
        return 1;
    }
    int rc = scene_lock(&s_bus[scope].table, s_edit, target, param, v, module);
    if (rc == SCENE_ERR_FULL) { s_flash = SCENE_FLASH_FULL; return 0; }
    if (rc != SCENE_OK) return 0;
    changed(scope);
    /* Heard now, not a frame later: the knob and the sound move together. */
    int ea = s_edit;
    tick_bus(scope, ea, SCENE_NONE, 0.0f);
    return 1;
}

int shadow_scene_bus_read(int scope, int pos, const char *param, char *buf, int len) {
    if (!valid_bus_scope(scope) || !param || !buf || len < 2) return -1;
    char target[SCENE_TARGET_LEN];
    snprintf(target, sizeof(target), "fx%d", pos + 1);
    scene_bus_t *bus = &s_bus[scope];
    if (s_edit != SCENE_NONE) {
        int i = scene_find(&bus->table, target, param);
        if (i >= 0 && (bus->table.pairs[i].mask & (1u << s_edit)) && live_slot(scope, &bus->table.pairs[i])) {
            scene_drive_t *d = find_drive(bus, target, param);
            int kind = d ? d->kind : SCENE_KIND_FLOAT;
            float v = bus->table.pairs[i].values[s_edit];
            return kind == SCENE_KIND_FLOAT ? snprintf(buf, len, "%.6f", v)
                                            : snprintf(buf, len, "%d", (int)lroundf(v));
        }
    }
    /* A DRIVEN param answers its base: the plugin holds the morph, which is
     * not what the user set (#276, the same rule the chain follows). */
    scene_drive_t *d = find_drive(bus, target, param);
    if (!d) return -1;
    return d->kind == SCENE_KIND_FLOAT ? snprintf(buf, len, "%.6f", d->base)
                                       : snprintf(buf, len, "%d", (int)lroundf(d->base));
}

void shadow_scene_bus_state_begin(int scope, int pos) {
    if (!valid_bus_scope(scope)) return;
    char target[SCENE_TARGET_LEN];
    snprintf(target, sizeof(target), "fx%d", pos + 1);
    void *slot = slot_for(scope, target);
    if (!slot) return;
    for (int i = 0; i < SCENE_MAX_PAIRS; i++) {
        scene_drive_t *d = &s_bus[scope].drives[i];
        if (d->active && d->has_last && strcmp(d->target, target) == 0)
            write_value(slot, d->param, d->base, d->kind);
    }
}

void shadow_scene_bus_state_end(int scope, int pos) {
    if (!valid_bus_scope(scope)) return;
    char target[SCENE_TARGET_LEN];
    snprintf(target, sizeof(target), "fx%d", pos + 1);
    void *slot = slot_for(scope, target);
    if (!slot) return;
    for (int i = 0; i < SCENE_MAX_PAIRS; i++) {
        scene_drive_t *d = &s_bus[scope].drives[i];
        if (d->active && d->has_last && strcmp(d->target, target) == 0)
            write_value(slot, d->param, d->last, d->kind);
    }
}

void shadow_scene_bus_note_write(int scope, int pos, const char *param, const char *val) {
    if (!valid_bus_scope(scope) || !param || !val) return;
    char target[SCENE_TARGET_LEN];
    snprintf(target, sizeof(target), "fx%d", pos + 1);
    scene_drive_t *d = find_drive(&s_bus[scope], target, param);
    if (!d) return;
    void *slot = slot_for(scope, target);
    if (!slot) return;
    scene_bus_meta_t meta = { d->kind, d->min, d->max, d->min, 0 };
    float v;
    if (!parse_value(slot, param, val, &meta, &v)) return;
    v = v < d->min ? d->min : v > d->max ? d->max : v;
    /* THE LIVE TAKEOVER: the turn is heard, from what was heard, and anchored
     * at the fader (scene_morph.h). Not while an edit is armed -- that write
     * was a lock, and never reaches here. */
    if (s_edit == SCENE_NONE) {
        const float heard = d->has_last ? d->last : d->base;
        d->takeover.k = scene_takeover_k(heard, d->base, v, d->kind, d->min, d->max);
        d->takeover.x0 = s_x;
        d->takeover.on = 1;
    }
    d->base = v;
    d->has_last = 0;                 /* the plugin now holds the base: re-apply */
    s_bus[scope].dirty = 1;
}
