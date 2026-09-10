/*
 * schwung-probe -- load a module off-device and ask it two questions.
 *
 *   probe --contract <module-dir> [-o out.json]
 *   probe --render   <module-dir> --score S.json -o out.wav [options]
 *
 * See README.md for the contract table and the surprising parts. The rules
 * that are easy to get wrong, restated where they are enforced:
 *
 *   - Dispatch on the ENTRY SYMBOL. The wrong one loads cleanly and is silent.
 *   - A read has THREE answers; unserved is not empty.
 *   - Whole octaves only when fitting; a semitone would retune the patch.
 *   - Verify a fit MOVED the pitch. If it did not, the measurement was wrong
 *     and no shift is better than a wrong one.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <dlfcn.h>
#include "plugin_api_v1.h"
#include "audio_fx_api_v2.h"

#define BLK 128
#define SR  44100
#define CONTRACT_BUF (1 << 18)   /* 256 KB: the host's own contract ceiling */

/* Every module guards `if (host->fn)`, so an all-zero host is safe and is what
 * lets this run with no host at all. */
static host_api_v1_t g_host;

typedef enum { KIND_NONE, KIND_SYNTH, KIND_FX } kind_t;

typedef void (*fx_on_midi_fn)(void *, const uint8_t *, int, int);

typedef struct {
    kind_t kind;
    void *inst;
    plugin_api_v2_t   *s;
    audio_fx_api_v2_t *x;
    fx_on_midi_fn fx_on_midi;   /* dlsym'd, NEVER the struct field -- see below */
} mod_t;

static const char *kind_name(kind_t k) {
    return k == KIND_SYNTH ? "synth" : k == KIND_FX ? "audio_fx" : "unknown";
}

/* ---------------------------------------------------------------- loading */

static int mod_open(mod_t *m, const char *so, const char *dir) {
    void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "probe: dlopen %s: %s\n", so, dlerror()); return 0; }

    /* Dispatch on the symbol, never on the filename or the catalog's type. */
    if (dlsym(h, AUDIO_FX_INIT_V2_SYMBOL)) {
        m->kind = KIND_FX;
        audio_fx_init_v2_fn f = (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL);
        m->x = f(&g_host);
        if (!m->x) { fprintf(stderr, "probe: %s returned NULL\n", AUDIO_FX_INIT_V2_SYMBOL); return 0; }
        /*
         * MIDI TO AN AUDIO FX COMES FROM A dlsym, NEVER FROM `x->on_midi`.
         *
         * That field is past the end of the struct several shipped modules
         * actually return, and what is there is NON-NULL GARBAGE: measured
         * 0xc0000000000 on cloudseed, gate and tapescam, 0xc000000 on psxverb
         * -- four of seven audio FX sampled -- while every one of them reports
         * api_version 2. So `if (x->on_midi) x->on_midi(...)` passes its own
         * guard and jumps into nothing. This crashed the probe.
         *
         * chain_host.c has always done it this way (`inst->fx_on_midi[slot] =
         * dlsym(handle, "move_audio_fx_on_midi")`), which is why Schwung
         * itself is not affected. Same defect shape as the breakbeat header
         * drift in CLAUDE.md: a guard testing memory owned by someone else.
         */
        m->fx_on_midi = (fx_on_midi_fn)dlsym(h, "move_audio_fx_on_midi");
        m->inst = m->x->create_instance(dir, NULL);
    } else if (dlsym(h, "move_plugin_init_v2")) {
        m->kind = KIND_SYNTH;
        typedef plugin_api_v2_t* (*sfn)(const host_api_v1_t*);
        m->s = ((sfn)dlsym(h, "move_plugin_init_v2"))(&g_host);
        if (!m->s) { fprintf(stderr, "probe: move_plugin_init_v2 returned NULL\n"); return 0; }
        m->inst = m->s->create_instance(dir, NULL);
    } else {
        /* An ERROR, never an empty result: a module we cannot load must not be
         * published as a module with nothing to say. */
        fprintf(stderr, "probe: %s exports no known entry symbol\n", so);
        return 0;
    }
    if (!m->inst) { fprintf(stderr, "probe: create_instance returned NULL\n"); return 0; }
    return 1;
}

static int mod_get(mod_t *m, const char *key, char *buf, int len) {
    if (m->kind == KIND_SYNTH) return m->s->get_param ? m->s->get_param(m->inst, key, buf, len) : -1;
    return m->x->get_param ? m->x->get_param(m->inst, key, buf, len) : -1;
}
static void mod_set(mod_t *m, const char *key, const char *val) {
    if (m->kind == KIND_SYNTH) { if (m->s->set_param) m->s->set_param(m->inst, key, val); }
    else                       { if (m->x->set_param) m->x->set_param(m->inst, key, val); }
}
static void mod_midi(mod_t *m, const uint8_t *msg, int len) {
    /* The synth API's on_midi is a real field and is safe. The FX one is not
     * -- see mod_open. */
    if (m->kind == KIND_SYNTH) { if (m->s->on_midi) m->s->on_midi(m->inst, msg, len, 0); }
    else if (m->fx_on_midi)    { m->fx_on_midi(m->inst, msg, len, 0); }
}
static void mod_render(mod_t *m, int16_t *out, int frames) {
    if (m->kind == KIND_SYNTH) m->s->render_block(m->inst, out, frames);
    else                       m->x->process_block(m->inst, out, frames);
}

/* --------------------------------------------------------------- contract */

/* A read has THREE answers and the emitted JSON keeps them apart: a value,
 * "" (served, produced nothing), or null (unserved). Collapsing the last two
 * makes every audio FX look like it publishes an empty contract, because an
 * FX serves no chain_params at all -- that metadata lives in module.json. */
static void emit_key(FILE *f, mod_t *m, const char *key, char *buf, int cap, int last) {
    int n = mod_get(m, key, buf, cap);
    fprintf(f, "  \"%s\": ", key);
    if (n < 0) fputs("null", f);                       /* unserved */
    else {
        buf[n < cap ? n : cap - 1] = 0;
        /* Contracts are JSON documents; emit verbatim so the renderer parses
         * the module's own bytes rather than a re-encoding of them. */
        if (n == 0) fputs("\"\"", f); else fputs(buf, f);
    }
    fputs(last ? "\n" : ",\n", f);
}

static int cmd_contract(mod_t *m, const char *id, const char *out) {
    char *buf = malloc(CONTRACT_BUF);
    FILE *f = out ? fopen(out, "w") : stdout;
    if (!f) { perror(out); return 1; }
    fprintf(f, "{\n  \"id\": \"%s\",\n  \"contract\": \"%s\",\n  \"harness\": \"probe-isolated\",\n",
            id, kind_name(m->kind));
    emit_key(f, m, "chain_params", buf, CONTRACT_BUF, 0);
    emit_key(f, m, "ui_hierarchy", buf, CONTRACT_BUF, 0);
    emit_key(f, m, "split_voices", buf, CONTRACT_BUF, 1);
    fputs("}\n", f);
    if (out) fclose(f);
    free(buf);
    return 0;
}

/* ----------------------------------------------------------------- pitch  */

/* Autocorrelation, not zero crossings: a bright saw crosses zero many times
 * per cycle and reported 21 kHz for a clean note, which read as noise. */
static double detect_f0(const int16_t *x, int n) {
    double mean = 0;
    for (int i = 0; i < n; i++) mean += x[i * 2];
    mean /= n;
    double best = 0; int best_lag = 0;
    for (int lag = SR / 1200; lag < SR / 50 && lag < n / 2; lag++) {
        double num = 0, da = 0, db = 0;
        for (int i = 0; i + lag < n; i += 2) {
            double a = x[i * 2] - mean, b = x[(i + lag) * 2] - mean;
            num += a * b; da += a * a; db += b * b;
        }
        double r = num / (sqrt(da * db) + 1e-9);
        if (r > best) { best = r; best_lag = lag; }
    }
    return best_lag ? (double)SR / best_lag : 0.0;
}

/* Play one note and report what came out. */
static double probe_note(mod_t *m, int note, int16_t *scratch) {
    memset(scratch, 0, (size_t)SR * 4);
    uint8_t on[3]  = { 0x90, (uint8_t)note, 100 };
    uint8_t off[3] = { 0x80, (uint8_t)note, 0 };
    mod_midi(m, on, 3);
    for (int o = 0; o + BLK <= SR; o += BLK) mod_render(m, scratch + (size_t)o * 2, BLK);
    mod_midi(m, off, 3);
    return detect_f0(scratch + (SR / 2) * 2, 16384);
}

/*
 * How many WHOLE OCTAVES to shift a score so this patch lands in register.
 *
 * Whole octaves only -- the patch IS the artistic intent, and a semitone nudge
 * would retune it rather than reposition it. Measured across four obxd presets
 * the spread is ~3 octaves with octave_transpose at 0 on every one, so this is
 * the patch's doing and there is no parameter to "correct".
 *
 * THE SELF-CHECK IS THE POINT. Play the shifted note and confirm the pitch
 * actually moved by the amount expected. Two presets measured the SAME pitch
 * an octave up, which is the detector locking onto a subharmonic -- and a
 * wrong shift is worse than none, so that case returns 0.
 */
static int octave_fit(mod_t *m, int base_note, int16_t *scratch, char *why, int why_len) {
    double f = probe_note(m, base_note, scratch);
    if (f < 20) { snprintf(why, why_len, "no pitch (percussive or silent)"); return 0; }

    double target = 261.63 * pow(2.0, (base_note - 60) / 12.0);
    double octaves = log2(f / target);
    int k = (int)lround(octaves);
    if (fabs(octaves - k) > 0.25) {
        snprintf(why, why_len, "%.1f Hz is %+.2f oct from target, not an octave relationship", f, octaves);
        return 0;
    }
    if (k == 0) { snprintf(why, why_len, "%.1f Hz already in register", f); return 0; }

    int shifted = base_note - 12 * k;
    if (shifted < 0 || shifted > 127) { snprintf(why, why_len, "shift leaves MIDI range"); return 0; }

    double f2 = probe_note(m, shifted, scratch);
    double moved = (f2 > 20 && f > 20) ? log2(f2 / f) : 0;
    if (fabs(moved - (-k)) > 0.25) {
        snprintf(why, why_len,
                 "REJECTED: playing %+d semitones moved the pitch %+.2f oct, expected %+d "
                 "(subharmonic lock)", -12 * k, moved, -k);
        return 0;
    }
    snprintf(why, why_len, "%.1f Hz -> %.1f Hz, shift %+d oct (verified)", f, f2, -k);
    return -k;
}

/* ----------------------------------------------------------------- score  */

typedef struct { int sample, status, d1, d2; } event_t;
typedef struct { event_t *ev; int n, frames; } score_t;

/* The score is a small fixed-shape document this repo writes (mid2score.mjs),
 * so a scanner is honest here -- it is not parsing arbitrary JSON. */
static int score_load(score_t *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(len + 1);
    if (fread(b, 1, len, f) != (size_t)len) { fclose(f); free(b); return 0; }
    b[len] = 0; fclose(f);

    int cap = 1024; s->ev = malloc(sizeof(event_t) * cap); s->n = 0;
    char *q = strstr(b, "\"events\"");
    char *end = strstr(b, "\"frames\"");
    while (q && (q = strchr(q, '['))) {
        if (end && q > end) break;
        q++;
        int a, c, d, e;
        if (sscanf(q, "%d,%d,%d,%d", &a, &c, &d, &e) == 4) {
            if (s->n == cap) { cap *= 2; s->ev = realloc(s->ev, sizeof(event_t) * cap); }
            s->ev[s->n++] = (event_t){ a, c, d, e };
        }
    }
    s->frames = 0;
    if (end) sscanf(end + 9, "%*[^0-9]%d", &s->frames);
    if (s->frames <= 0) s->frames = SR * 30;
    free(b);
    return 1;
}

static void wav_write(const char *path, const int16_t *pcm, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t data = (uint32_t)frames * 4;
    unsigned char h[44] = {0};
    memcpy(h, "RIFF", 4);        *(uint32_t*)(h + 4)  = 36 + data;
    memcpy(h + 8, "WAVEfmt ", 8); *(uint32_t*)(h + 16) = 16;
    *(uint16_t*)(h + 20) = 1;     *(uint16_t*)(h + 22) = 2;
    *(uint32_t*)(h + 24) = SR;    *(uint32_t*)(h + 28) = SR * 4;
    *(uint16_t*)(h + 32) = 4;     *(uint16_t*)(h + 34) = 16;
    memcpy(h + 36, "data", 4);    *(uint32_t*)(h + 40) = data;
    fwrite(h, 1, 44, f); fwrite(pcm, 1, data, f); fclose(f);
}

/*
 * Render the score. The chain form is source synth -> effect, block by block,
 * exactly as a slot runs it: an FX rendered ALONE processes a silent buffer
 * and would publish 30 s of nothing. `fx` may be NULL for a plain synth.
 */
static void render_chain(mod_t *src, mod_t *fx, const score_t *s, int shift, int16_t *pcm) {
    memset(pcm, 0, (size_t)s->frames * 4);
    int e = 0;
    for (int o = 0; o + BLK <= s->frames; o += BLK) {
        while (e < s->n && s->ev[e].sample < o + BLK) {
            int note = s->ev[e].d1 + 12 * shift;
            if (note >= 0 && note <= 127) {
                uint8_t msg[3] = { (uint8_t)s->ev[e].status, (uint8_t)note, (uint8_t)s->ev[e].d2 };
                mod_midi(src, msg, 3);
                /* A ducker or a sidechain FX is driven by the same notes. */
                if (fx) mod_midi(fx, msg, 3);
            }
            e++;
        }
        mod_render(src, pcm + (size_t)o * 2, BLK);
        if (fx) mod_render(fx, pcm + (size_t)o * 2, BLK);
    }
}

static double rms_dbfs(const int16_t *pcm, long n) {
    double sq = 0;
    for (long i = 0; i < n; i++) sq += (double)pcm[i] * pcm[i];
    double r = sqrt(sq / n);
    return r > 0 ? 20 * log10(r / 32768.0) : -999.0;
}

/* ------------------------------------------------------------------ main  */

static void usage(void) {
    fputs(
      "usage:\n"
      "  probe --contract <module-dir> --so <file> [--id ID] [-o out.json]\n"
      "  probe --render   <module-dir> --so <file> --score S.json -o out.wav\n"
      "         [--presets N] [--octave-fit] [--set k=v ...] [--id ID]\n"
      "\n"
      "  --presets N   render N presets spread across the module's own bank.\n"
      "                A module declaring none gets one render; no bank is invented.\n"
      "  --octave-fit  shift the score by whole octaves so the patch lands in\n"
      "                register, and REVERT if the shift does not verify.\n"
      "  --set k=v     applied after the preset, before rendering.\n", stderr);
}

int main(int argc, char **argv) {
    const char *dir = NULL, *so = NULL, *score_path = NULL, *out = NULL, *id = "module";
    int want_contract = 0, want_render = 0, npresets = 1, fit = 0;
    const char *sets[32]; int nsets = 0;
    const char *src_dir = NULL, *src_so = NULL;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--contract") && i + 1 < argc) { want_contract = 1; dir = argv[++i]; }
        else if (!strcmp(argv[i], "--render")   && i + 1 < argc) { want_render   = 1; dir = argv[++i]; }
        else if (!strcmp(argv[i], "--so")       && i + 1 < argc) so = argv[++i];
        else if (!strcmp(argv[i], "--score")    && i + 1 < argc) score_path = argv[++i];
        else if (!strcmp(argv[i], "--id")       && i + 1 < argc) id = argv[++i];
        else if (!strcmp(argv[i], "-o")         && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--presets")  && i + 1 < argc) npresets = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--octave-fit")) fit = 1;
        else if (!strcmp(argv[i], "--source-dir") && i + 1 < argc) src_dir = argv[++i];
        else if (!strcmp(argv[i], "--source-so")  && i + 1 < argc) src_so  = argv[++i];
        else if (!strcmp(argv[i], "--set") && i + 1 < argc && nsets < 32) sets[nsets++] = argv[++i];
        else { usage(); return 2; }
    }
    if (!dir || !so || (!want_contract && !want_render)) { usage(); return 2; }

    mod_t m = {0};
    if (!mod_open(&m, so, dir)) return 1;
    fprintf(stderr, "probe: %s loaded as %s\n", id, kind_name(m.kind));

    if (want_contract) return cmd_contract(&m, id, out);

    if (!score_path || !out) { usage(); return 2; }
    score_t s = {0};
    if (!score_load(&s, score_path)) return 1;

    /* An FX is previewed THROUGH a source instrument. Without one it processes
     * a silent buffer and we would publish 30 s of nothing. */
    mod_t source = {0};
    int chained = 0;
    if (src_dir && src_so) {
        if (!mod_open(&source, src_so, src_dir)) return 1;
        if (source.kind != KIND_SYNTH) { fprintf(stderr, "probe: --source must be a synth\n"); return 1; }
        chained = 1;
        fprintf(stderr, "probe: source instrument loaded, rendering through the effect\n");
    } else if (m.kind == KIND_FX) {
        fprintf(stderr, "probe: refusing to render an audio FX with no --source "
                        "(it would process silence)\n");
        return 1;
    }

    /* How many presets does the MODULE say it has? Only the module can answer
     * "which presets"; cloudseed answers "none" by declaring no such params. */
    /* Presets belong to whichever module is the SUBJECT of the preview: the
     * synth normally, and still the synth when it is only the source for an
     * FX -- an FX with a preset bank steps its own below. */
    mod_t *subject = chained ? &m : &m;
    char buf[4096];
    int count = 1, has_presets = 0;
    if (mod_get(subject, "preset_count", buf, sizeof buf) > 0) {
        count = atoi(buf);
        if (count > 0) has_presets = 1; else count = 1;
    }
    int k = has_presets ? (npresets < count ? npresets : count) : 1;
    fprintf(stderr, "probe: %s\n", has_presets ? "stepping the module's own preset bank"
                                               : "no preset bank declared -- one render");

    int16_t *pcm     = calloc((size_t)s.frames * 2, sizeof(int16_t));
    int16_t *scratch = calloc((size_t)SR * 2, sizeof(int16_t));

    printf("[\n");
    for (int j = 0; j < k; j++) {
        int idx = (k > 1) ? (int)((double)j * (count - 1) / (k - 1)) : 0;
        char name[192] = "(default)";
        if (has_presets) {
            char v[16]; snprintf(v, sizeof v, "%d", idx);
            mod_set(&m, "preset", v);
            /* Read into a bounded slice: a preset name longer than `name`
             * is truncated deliberately, and gcc should not have to guess
             * that from a 4 KB source buffer. */
            int n = mod_get(&m, "preset_name", buf, (int)sizeof name);
            if (n > 0) { buf[n < (int)sizeof name ? n : (int)sizeof name - 1] = 0;
                         memcpy(name, buf, strlen(buf) + 1); }
        }
        for (int si = 0; si < nsets; si++) {
            char tmp[256]; snprintf(tmp, sizeof tmp, "%s", sets[si]);
            char *eq = strchr(tmp, '=');
            if (eq) { *eq = 0; mod_set(&m, tmp, eq + 1); }
        }

        char why[256] = "not requested";
        int shift = fit ? octave_fit(chained ? &source : &m, 60, scratch, why, sizeof why) : 0;

        render_chain(chained ? &source : &m, chained ? &m : NULL, &s, shift, pcm);
        double db = rms_dbfs(pcm, (long)s.frames * 2);

        char path[1024];
        if (k > 1) {
            const char *dot = strrchr(out, '.');
            int stem = dot ? (int)(dot - out) : (int)strlen(out);
            snprintf(path, sizeof path, "%.*s-p%d%s", stem, out, j, dot ? dot : "");
        } else snprintf(path, sizeof path, "%s", out);
        wav_write(path, pcm, s.frames);

        /* preset index is null when the module declares no bank -- "which
         * preset" has no answer there, and 0 would invent one. */
        printf("  {\"slot\": %d, \"preset\": ", j);
        if (has_presets) printf("%d", idx); else printf("null");
        printf(", \"name\": \"%s\", \"file\": \"%s\", "
               "\"rms_dbfs\": %.1f, \"octave_shift\": %d, \"fit\": \"%s\"}%s\n",
               name, path, db, shift, why, j + 1 < k ? "," : "");
        fprintf(stderr, "  [%d] preset %-4d %-24s %6.1f dBFS  oct %+d  (%s)\n",
                j, idx, name, db, shift, why);
    }
    printf("]\n");
    return 0;
}
