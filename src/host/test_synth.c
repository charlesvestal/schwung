// SPDX-License-Identifier: MIT
// See test_synth.h.

#include "test_synth.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NVOICES         8
#define SAMPLE_RATE_HZ  44100.0
#define VOICE_AMP       0.10f      // quiet — 10% scale per voice
#define TWO_PI          6.283185307179586f

typedef struct {
    uint8_t note;          // MIDI note number, 0 if free
    float   phase;
    float   phase_step;    // 2π * freq / SR
} voice_t;

static voice_t g_voices[NVOICES] = {0};

// Equal-tempered: f = 440 * 2^((n-69)/12).
static float note_to_step(uint8_t note) {
    float f = 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
    return TWO_PI * f / (float)SAMPLE_RATE_HZ;
}

static voice_t *find_voice(uint8_t note) {
    for (int i = 0; i < NVOICES; i++) {
        if (g_voices[i].note == note) return &g_voices[i];
    }
    return NULL;
}

static voice_t *alloc_voice(void) {
    for (int i = 0; i < NVOICES; i++) {
        if (g_voices[i].note == 0) return &g_voices[i];
    }
    // Steal voice 0 if nothing free.
    return &g_voices[0];
}

void schwung_test_synth_on_midi(const uint8_t *bytes) {
    if (!bytes) return;
    uint8_t status = bytes[0] & 0xF0;
    uint8_t note   = bytes[1];
    uint8_t vel    = bytes[2];

    if (status == 0x90 && vel > 0) {
        // Note-on. Reuse existing voice for the same note, else allocate.
        voice_t *v = find_voice(note);
        if (!v) v = alloc_voice();
        v->note = note ? note : 1;  // we use note 0 as "free" sentinel
        v->phase = 0.0f;
        v->phase_step = note_to_step(note);
        fprintf(stderr, "test_synth: note-on %d step=%.4f\n", note, v->phase_step);
    } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
        voice_t *v = find_voice(note);
        fprintf(stderr, "test_synth: note-off %d → voice=%s\n",
                note, v ? "FOUND" : "NOT FOUND");
        if (v) {
            v->note = 0;
            v->phase = 0.0f;
            v->phase_step = 0.0f;
        }
    }
}

int schwung_test_synth_active(void) {
    for (int i = 0; i < NVOICES; i++) {
        if (g_voices[i].note != 0) return 1;
    }
    return 0;
}

void schwung_test_synth_render(int16_t *out_lr, int frames) {
    if (!out_lr) return;
    for (int i = 0; i < frames; i++) {
        float sum = 0.0f;
        for (int v = 0; v < NVOICES; v++) {
            if (g_voices[v].note == 0) continue;
            sum += sinf(g_voices[v].phase) * VOICE_AMP;
            g_voices[v].phase += g_voices[v].phase_step;
            if (g_voices[v].phase >= TWO_PI) g_voices[v].phase -= TWO_PI;
        }
        // Clip and convert. Mono → write to both L and R.
        if (sum >  1.0f) sum =  1.0f;
        if (sum < -1.0f) sum = -1.0f;
        int16_t s = (int16_t)(sum * 32767.0f);
        int dst = i * 2;
        // Add to existing buffer (caller may have zeroed; chain module audio
        // would also feed in here on device).
        int32_t l = out_lr[dst]     + s; if (l >  32767) l =  32767; if (l < -32768) l = -32768;
        int32_t r = out_lr[dst + 1] + s; if (r >  32767) r =  32767; if (r < -32768) r = -32768;
        out_lr[dst]     = (int16_t)l;
        out_lr[dst + 1] = (int16_t)r;
    }
}
