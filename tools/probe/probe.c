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
/* The scratch buffer is shared by every measurement, so it is sized for the
 * LONGEST of them (the release tail), not the first one written. */
#define SCRATCH_SECONDS 5

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
 * How long does this patch take to get out of its own way? Hold a note,
 * release it, measure the decay to -40 dB.
 *
 * WHY IT MATTERS: the score fires a note every ~0.28 s. Measured across the
 * fleet, releases run from 0.00 s to 3.40 s -- so on a long-release preset a
 * dozen voices ring at once and the preview is mud, while the score itself
 * never holds more than three notes. Density has to follow the PRESET, not
 * the music.
 *
 * NOTE THE STRADDLE. `if (o == HOLD)` is wrong and silently so: o steps by
 * BLK and HOLD is rarely a multiple of it, so the note-off never fires and
 * every patch measures as an infinite release. This has now cost three
 * separate measurements; test the interval, never the instant.
 */
/*
 * Let whatever is still sounding die away.
 *
 * A module keeps state across presets, so the PREVIOUS preset's tail is still
 * ringing when the next one is measured -- and a measurement taken over it
 * reports the tail rather than the patch. Sub Bass measured 0.10 s alone and
 * 4.00 s in sequence for exactly this reason.
 */
static void settle(mod_t *m, int16_t *scratch) {
    uint8_t all_off[3] = { 0xB0, 123, 0 };      /* All Notes Off */
    mod_midi(m, all_off, 3);
    const int F = SR * 2;
    for (int o = 0; o + BLK <= F; o += BLK) mod_render(m, scratch + (size_t)(o % (SR)) * 2, BLK);
}

static double release_seconds(mod_t *m, int16_t *scratch) {
    const int HOLD = SR, F = SR * SCRATCH_SECONDS;
    settle(m, scratch);
    memset(scratch, 0, (size_t)F * 4);
    uint8_t on[3] = { 0x90, 60, 100 }, off[3] = { 0x80, 60, 0 };
    mod_midi(m, on, 3);
    for (int o = 0; o + BLK <= F; o += BLK) {
        if (o <= HOLD && o + BLK > HOLD) mod_midi(m, off, 3);
        mod_render(m, scratch + (size_t)o * 2, BLK);
    }
    double ref = 0; int n = 0;
    for (int i = HOLD - SR / 4; i < HOLD; i++) { ref += (double)scratch[i*2] * scratch[i*2]; n++; }
    ref = sqrt(ref / n);
    if (ref <= 0) return 0;
    const double thr = ref / 100.0;              /* -40 dB */
    for (int w = HOLD; w + SR/20 < F; w += SR/20) {
        double e = 0;
        for (int i = w; i < w + SR/20; i++) e += (double)scratch[i*2] * scratch[i*2];
        if (sqrt(e / (SR/20)) < thr) return (w - HOLD) / (double)SR;
    }
    return (F - HOLD) / (double)SR;
}

/* ---------------------------------------------------------- param probing */

/*
 * WHICH KNOB IS WORTH SWEEPING?
 *
 * A filter or an EQ held at one setting shows almost nothing -- the interest
 * is in the movement. But "sweep the cutoff" cannot be decided from the NAME:
 * a param called `cutoff` may be inert on a given preset while `tone` does
 * everything, and half the fleet does not use either word.
 *
 * So ask the audio. Render the same input with the param at its minimum and
 * at its maximum, and compare the two SPECTRA. The parameters that change the
 * spectrum most are the ones a preview should move.
 *
 * LEVEL IS NORMALISED OUT FIRST, deliberately. A gain or a mix control changes
 * every band by the same amount and would otherwise win every time while being
 * the least interesting thing to sweep -- what we want is a change of SHAPE.
 */
#define NBANDS 20

static void band_energies(const int16_t *x, int n, double *out) {
    for (int b = 0; b < NBANDS; b++) {
        double f = 40.0 * pow(2.0, b * 0.45);          /* 40 Hz .. ~16 kHz */
        if (f > SR / 2.2) { out[b] = 0; continue; }
        double w = 2 * M_PI * f / SR, c = 2 * cos(w), s1 = 0, s2 = 0;
        for (int i = 0; i < n; i++) { double s0 = x[i*2] + c*s1 - s2; s2 = s1; s1 = s0; }
        out[b] = sqrt(fabs(s1*s1 + s2*s2 - c*s1*s2)) / n;
    }
    /* Normalise to unit sum: compare SHAPE, not loudness. */
    double sum = 0;
    for (int b = 0; b < NBANDS; b++) sum += out[b];
    if (sum > 0) for (int b = 0; b < NBANDS; b++) out[b] /= sum;
}

/*
 * What does this parameter DO? Sample it across its range and report two
 * quantities, because they mean different things and one hides the other:
 *
 *   shape  -- how much the normalised spectrum moves. A filter sweep, a tone
 *             control, a wavefolder. This is what is worth animating.
 *   level  -- how much the loudness moves, in dB. A gain or a mix control.
 *
 * NORMALISING LEVEL OUT OF `shape` IS NECESSARY AND NOT SUFFICIENT. Without
 * it a gain control wins every comparison while being the least interesting
 * thing to sweep. With it ALONE, a parameter that MUTES the signal reads as
 * inert -- filter's `cutoff` at 0 renders -80.9 dBFS, the most effective
 * control on the module, and scored 0.03 because the residue still had a
 * shape. Both numbers, always.
 *
 * SAMPLED ACROSS THE RANGE, not just at the ends: an endpoint is often
 * degenerate (silence, self-oscillation) and two extremes can coincidentally
 * resemble each other while everything between them differs.
 */
#define SWEEP_STEPS 5

typedef struct { double shape, level_db; } sweep_t;

static sweep_t spectral_change(mod_t *src, mod_t *fx, const char *key,
                               double lo, double hi, int16_t *scratch) {
    double band[SWEEP_STEPS][NBANDS], rms[SWEEP_STEPS];
    const int N = SR / 2;
    mod_t *target = fx ? fx : src;

    /*
     * REMEMBER WHERE THIS PARAM WAS, AND PUT IT BACK.
     *
     * Without this, each scan leaves its parameter at the MAXIMUM and every
     * later scan runs on top of it. It reads as a confident, wrong answer
     * rather than as an error: five of 4k-eq's bands and three of cloudseed's
     * controls scored exactly 0.000 -- masked by whatever had been swept
     * before them, not inert.
     */
    char saved[128] = "";
    int have_saved = 0;
    {
        int n = mod_get(target, key, saved, (int)sizeof saved - 1);
        if (n > 0) { saved[n] = 0; have_saved = 1; }
    }
    for (int step = 0; step < SWEEP_STEPS; step++) {
        double v = lo + (hi - lo) * step / (double)(SWEEP_STEPS - 1);
        char buf[64]; snprintf(buf, sizeof buf, "%g", v);
        mod_set(fx ? fx : src, key, buf);
        settle(src, scratch);
        if (fx) settle(fx, scratch);
        memset(scratch, 0, (size_t)N * 4);
        uint8_t on[3] = { 0x90, 60, 100 }, off[3] = { 0x80, 60, 0 };
        mod_midi(src, on, 3);
        for (int o = 0; o + BLK <= N; o += BLK) {
            mod_render(src, scratch + (size_t)o * 2, BLK);
            if (fx) mod_render(fx, scratch + (size_t)o * 2, BLK);
        }
        mod_midi(src, off, 3);
        const int a = SR / 16, n = N - a;
        band_energies(scratch + (size_t)a * 2, n, band[step]);
        double sq = 0;
        for (int i = a; i < N; i++) sq += (double)scratch[i*2] * scratch[i*2];
        rms[step] = sqrt(sq / n);
    }
    sweep_t r = { 0, 0 };
    /* Total variation along the sweep, so a monotonic control and a control
     * that returns to where it started are told apart. */
    for (int step = 1; step < SWEEP_STEPS; step++) {
        double d = 0;
        for (int i = 0; i < NBANDS; i++) d += fabs(band[step][i] - band[step-1][i]);
        r.shape += d / 2.0;
    }
    if (have_saved) { mod_set(target, key, saved); settle(src, scratch); if (fx) settle(fx, scratch); }
    double lo_r = 1e30, hi_r = 0;
    for (int step = 0; step < SWEEP_STEPS; step++) {
        if (rms[step] < lo_r) lo_r = rms[step];
        if (rms[step] > hi_r) hi_r = rms[step];
    }
    r.level_db = (hi_r > 0 && lo_r > 0) ? 20 * log10(hi_r / lo_r)
               : (hi_r > 0 ? 99.0 : 0.0);
    return r;
}

/*
 * Render the score. The chain form is source synth -> effect, block by block,
 * exactly as a slot runs it: an FX rendered ALONE processes a silent buffer
 * and would publish 30 s of nothing. `fx` may be NULL for a plain synth.
 */
/*
 * A knob moving while the score plays.
 *
 * WHERE it moves matters. The sweep runs over the CHORD section, because that
 * is the only stretch of the score with a sustained tone to hear it on -- a
 * filter opening under an arpeggio is heard as the arpeggio changing, not as
 * the filter. The arp and melody keep the patch's static character, so a
 * preview shows both.
 *
 * It goes lo -> hi -> lo so the clip ends where it started and the module is
 * not left mis-set for whatever renders next.
 */
typedef struct {
    const char *key;
    double lo, hi, t0, t1;
    int active;
} sweep_plan_t;

static void apply_sweep(mod_t *m, const sweep_plan_t *sw, double t) {
    if (!sw->active || t < sw->t0 || t > sw->t1) return;
    double u = (t - sw->t0) / (sw->t1 - sw->t0);      /* 0..1 across the window */
    double tri = u < 0.5 ? u * 2 : (1 - u) * 2;       /* up then back down */
    char buf[64];
    snprintf(buf, sizeof buf, "%g", sw->lo + (sw->hi - sw->lo) * tri);
    mod_set(m, sw->key, buf);
}

static void render_chain(mod_t *src, mod_t *fx, const score_t *s, int shift,
                         double stretch, const sweep_plan_t *sw, int16_t *pcm) {
    memset(pcm, 0, (size_t)s->frames * 4);
    int e = 0;
    for (int o = 0; o + BLK <= s->frames; o += BLK) {
        /* Stretching SPREADS the same phrase over the same window, so a
         * long-release patch simply plays fewer notes rather than a slower
         * tune -- events past the end are dropped. */
        while (e < s->n && (long)(s->ev[e].sample * stretch) < o + BLK) {
            int note = s->ev[e].d1 + 12 * shift;
            if (note >= 0 && note <= 127) {
                uint8_t msg[3] = { (uint8_t)s->ev[e].status, (uint8_t)note, (uint8_t)s->ev[e].d2 };
                mod_midi(src, msg, 3);
                /* A ducker or a sidechain FX is driven by the same notes. */
                if (fx) mod_midi(fx, msg, 3);
            }
            e++;
        }
        if (sw && sw->active) apply_sweep(fx ? fx : src, sw, o / (double)SR);
        mod_render(src, pcm + (size_t)o * 2, BLK);
        if (fx) mod_render(fx, pcm + (size_t)o * 2, BLK);
    }
    /* Leave it where it was found. */
    if (sw && sw->active) {
        char buf[64]; snprintf(buf, sizeof buf, "%g", sw->lo);
        mod_set(fx ? fx : src, sw->key, buf);
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
      "  probe --sweep-scan <module-dir> --so <file> --try key:lo:hi [--try ...]\n"
      "         [--source-dir D --source-so F]\n"
      "  probe --render   <module-dir> --so <file> --score S.json -o out.wav\n"
      "         [--presets N] [--octave-fit] [--set k=v ...] [--id ID]\n"
      "\n"
      "  --presets N   render N presets spread across the module's own bank.\n"
      "                A module declaring none gets one render; no bank is invented.\n"
      "  --adaptive-density  spread the phrase to suit the preset's measured release.\n"
      "  --octave-fit  shift the score by whole octaves so the patch lands in\n"
      "                register, and REVERT if the shift does not verify.\n"
      "  --set k=v     applied after the preset, before rendering.\n"
      "  --sweep key:lo:hi[:t0:t1]  move a knob while the score plays; defaults to\n"
      "                the chord section, the only part with a sustained tone.\n", stderr);
}

int main(int argc, char **argv) {
    const char *dir = NULL, *so = NULL, *score_path = NULL, *out = NULL, *id = "module";
    int want_contract = 0, want_render = 0, npresets = 1, fit = 0, adapt = 0, want_sweepscan = 0;
    const char *sweeps[64]; int nsweeps = 0;
    const char *sweep_spec = NULL;
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
        else if (!strcmp(argv[i], "--adaptive-density")) adapt = 1;
        else if (!strcmp(argv[i], "--sweep-scan") && i + 1 < argc) { want_sweepscan = 1; dir = argv[++i]; }
        else if (!strcmp(argv[i], "--try") && i + 1 < argc && nsweeps < 64) sweeps[nsweeps++] = argv[++i];
        else if (!strcmp(argv[i], "--sweep") && i + 1 < argc) sweep_spec = argv[++i];
        else if (!strcmp(argv[i], "--source-dir") && i + 1 < argc) src_dir = argv[++i];
        else if (!strcmp(argv[i], "--source-so")  && i + 1 < argc) src_so  = argv[++i];
        else if (!strcmp(argv[i], "--set") && i + 1 < argc && nsets < 32) sets[nsets++] = argv[++i];
        else { usage(); return 2; }
    }
    if (!dir || !so || (!want_contract && !want_render && !want_sweepscan)) { usage(); return 2; }

    mod_t m = {0};
    if (!mod_open(&m, so, dir)) return 1;
    fprintf(stderr, "probe: %s loaded as %s\n", id, kind_name(m.kind));

    if (want_contract) return cmd_contract(&m, id, out);

    if (want_sweepscan) {
        /* An FX needs an instrument in front of it or every param scores zero
         * for the same reason a silent render does. */
        mod_t src = {0};
        int chained = 0;
        if (src_dir && src_so) {
            if (!mod_open(&src, src_so, src_dir)) return 1;
            chained = 1;
        } else if (m.kind == KIND_FX) {
            fprintf(stderr, "probe: --sweep-scan on an audio FX needs --source\n");
            return 1;
        }
        int16_t *scratch = calloc((size_t)SR * SCRATCH_SECONDS * 2, sizeof(int16_t));
        printf("[\n");
        int first = 1;
        for (int i = 0; i < nsweeps; i++) {
            char spec[256]; snprintf(spec, sizeof spec, "%s", sweeps[i]);
            /* key:lo:hi */
            char *c1 = strchr(spec, ':'); if (!c1) continue; *c1 = 0;
            char *c2 = strchr(c1 + 1, ':'); if (!c2) continue; *c2 = 0;
            sweep_t r = spectral_change(chained ? &src : &m, chained ? &m : NULL,
                                        spec, atof(c1 + 1), atof(c2 + 1), scratch);
            /* A sweep is worth animating when it moves the SHAPE. A big level
             * move with a flat shape is a gain control -- reported, but not a
             * candidate, or every module would "need" its output swept. */
            const char *verdict = r.shape > 0.35 ? "SWEEP"
                                : r.level_db > 12 ? "gain-like"
                                : r.shape > 0.15 ? "mild" : "inert";
            printf("%s  {\"key\": \"%s\", \"shape\": %.3f, \"level_db\": %.1f, \"verdict\": \"%s\"}",
                   first ? "" : ",\n", spec, r.shape, r.level_db, verdict);
            first = 0;
            fprintf(stderr, "  %-20s shape %.3f  level %5.1f dB   %s\n", spec, r.shape, r.level_db, verdict);
        }
        printf("\n]\n");
        return 0;
    }

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

    /* --sweep key:lo:hi[:t0:t1] -- defaults to the chord section of the demo
     * score, which is the part with a sustained tone. */
    sweep_plan_t sweep = { NULL, 0, 0, 8.4, 16.8, 0 };
    if (sweep_spec) {
        static char sb[256];
        snprintf(sb, sizeof sb, "%s", sweep_spec);
        char *f[5] = { sb, NULL, NULL, NULL, NULL };
        int nf = 1;
        for (char *q = sb; *q && nf < 5; q++) if (*q == ':') { *q = 0; f[nf++] = q + 1; }
        if (nf >= 3) {
            sweep.key = f[0]; sweep.lo = atof(f[1]); sweep.hi = atof(f[2]);
            if (nf >= 5) { sweep.t0 = atof(f[3]); sweep.t1 = atof(f[4]); }
            sweep.active = 1;
            fprintf(stderr, "probe: sweeping %s %g..%g over %.1f-%.1fs\n",
                    sweep.key, sweep.lo, sweep.hi, sweep.t0, sweep.t1);
        }
    }
    int16_t *pcm     = calloc((size_t)s.frames * 2, sizeof(int16_t));
    int16_t *scratch = calloc((size_t)SR * SCRATCH_SECONDS * 2, sizeof(int16_t));

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
        char density[128] = "1.00";
        int shift = fit ? octave_fit(chained ? &source : &m, 60, scratch, why, sizeof why) : 0;

            /* Density follows the preset's own release. Spacing of ~0.28 s
         * against a 3.4 s release is a dozen overlapping voices. */
        double stretch = 1.0;
        if (adapt) {
            double rel = release_seconds(chained ? &source : &m, scratch);
            stretch = rel / 0.28;
            if (stretch < 1.0) stretch = 1.0;
            if (stretch > 4.0) stretch = 4.0;
            snprintf(density, sizeof density, "release %.2fs -> spacing x%.2f", rel, stretch);
        }
        render_chain(chained ? &source : &m, chained ? &m : NULL, &s, shift, stretch, &sweep, pcm);
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
               "\"rms_dbfs\": %.1f, \"octave_shift\": %d, \"fit\": \"%s\", \"density\": \"%s\"}%s\n",
               name, path, db, shift, why, density, j + 1 < k ? "," : "");
        fprintf(stderr, "  [%d] preset %-4d %-24s %6.1f dBFS  oct %+d  (%s)\n",
                j, idx, name, db, shift, why);
    }
    printf("]\n");
    return 0;
}
