/*
 * render_cli — drive a Schwung chain off-device and write a WAV.
 *
 * This exists so the port's core can be proven, and regressed, without a DAW,
 * a GUI or an audio device in the way:
 *
 *   schwung-render --modules <root> --synth braids [--fx freeverb] -o out.wav
 *
 * It is the same code path the plugin takes — schwung_desktop.c driving
 * chain/dsp.so — with a fixed score instead of live MIDI, so a failure here is
 * a failure in the port rather than in the wrapper.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "schwung_desktop.h"

static void logger(const char *msg) { fprintf(stderr, "schwung: %s\n", msg); }

static void wav_write(const char *path, const int16_t *pcm, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t data = (uint32_t)frames * 4;
    unsigned char h[44] = {0};
    memcpy(h, "RIFF", 4);         *(uint32_t*)(h + 4)  = 36 + data;
    memcpy(h + 8, "WAVEfmt ", 8); *(uint32_t*)(h + 16) = 16;
    *(uint16_t*)(h + 20) = 1;     *(uint16_t*)(h + 22) = 2;
    *(uint32_t*)(h + 24) = SCHWUNG_RATE;
    *(uint32_t*)(h + 28) = SCHWUNG_RATE * 4;
    *(uint16_t*)(h + 32) = 4;     *(uint16_t*)(h + 34) = 16;
    memcpy(h + 36, "data", 4);    *(uint32_t*)(h + 40) = data;
    fwrite(h, 1, 44, f); fwrite(pcm, 1, data, f); fclose(f);
}

typedef struct { int frame; uint8_t b[3]; } ev_t;

int main(int argc, char **argv) {
    const char *root = NULL, *synth = NULL, *fx = NULL, *out = "out.wav";
    const char *getk[16]; int nget = 0;
    const char *setkv[16]; int nset = 0;
    double seconds = 4.0;
    double bpm = 120.0;
    int playing = 0;
    int in_tone = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--modules") && i + 1 < argc) root  = argv[++i];
        else if (!strcmp(argv[i], "--synth")   && i + 1 < argc) synth = argv[++i];
        else if (!strcmp(argv[i], "--fx")      && i + 1 < argc) fx    = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-o")        && i + 1 < argc) out   = argv[++i];
        else if (!strcmp(argv[i], "--get")     && i + 1 < argc && nget < 16) getk[nget++] = argv[++i];
        else if (!strcmp(argv[i], "--bpm")     && i + 1 < argc) bpm = atof(argv[++i]);
        else if (!strcmp(argv[i], "--set")     && i + 1 < argc && nset < 16) setkv[nset++] = argv[++i];
        else if (!strcmp(argv[i], "--play")) playing = 1;
        else if (!strcmp(argv[i], "--in-tone")) in_tone = 1;
        else { fprintf(stderr, "usage: %s --modules <root> --synth <id> [--fx <id>] [--play] [--bpm N] [--seconds N] [-o out.wav]\n", argv[0]); return 2; }
    }
    if (!root || (!synth && nget == 0)) {
        fprintf(stderr, "usage: %s --modules <root> --synth <id> [--fx <id>] [--play] [--bpm N] [--seconds N] [-o out.wav]\n", argv[0]);
        return 2;
    }

    schwung_desktop_t *sd = schwung_desktop_create(root, logger);
    if (!sd) { fprintf(stderr, "render: chain did not come up\n"); return 1; }

    schwung_desktop_set_param(sd, "synth:module", synth);
    if (fx) schwung_desktop_set_param(sd, "fx1:module", fx);

    /* --set key=value, applied after the modules load. */
    for (int i = 0; i < nset; i++) {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", setkv[i]);
        char *eq = strchr(tmp, '=');
        if (!eq) { fprintf(stderr, "render: --set needs key=value: %s\n", setkv[i]); continue; }
        *eq = 0;
        schwung_desktop_set_param(sd, tmp, eq + 1);
    }

    /* Read back what the chain says is loaded.
     *
     * THE READ KEY IS NOT THE WRITE KEY. A synth is SET with "synth:module"
     * and READ with "synth_module" -- chain_host.c:1702 against the "synth:"
     * prefix handling above it. Asking with the write key returns -1, which is
     * correct behaviour and not an error to route around: a -1 is a FAILED
     * READ, never "nothing is loaded", and a caller that collapses the two
     * turns a mistyped key into a confident wrong answer. */
    char buf[256];
    int n = schwung_desktop_get_param(sd, "synth_module", buf, sizeof(buf));
    if (n < 0) fprintf(stderr, "render: synth:module read did not complete\n");
    else       fprintf(stderr, "render: synth_module = '%s'\n", buf);

    /* --get: report a parameter and stop. The THREE answers are kept apart --
     * a failed read and an empty answer are different facts, and a caller that
     * merges them cannot tell "no such key" from "nothing is loaded". */
    if (nget > 0) {
        char *big = malloc(262144);
        for (int g = 0; g < nget; g++) {
            int r = schwung_desktop_get_param(sd, getk[g], big, 262144);
            if (r < 0)       printf("%-26s <read did not complete>\n", getk[g]);
            else if (r == 0) printf("%-26s <served, empty>\n", getk[g]);
            else             printf("%-26s %d bytes | %.160s\n", getk[g], r, big);
        }
        free(big);
        schwung_desktop_destroy(sd);
        return 0;
    }

    int total = (int)(seconds * SCHWUNG_RATE);
    total -= total % SCHWUNG_BLOCK;

    /* A chord in, held for half the take, then released so the tail is real
     * audio rather than a hard cut. */
    int note_off = (total / 2) - ((total / 2) % SCHWUNG_BLOCK);
    ev_t events[] = {
        {0,        {0x90, 60, 100}}, {0, {0x90, 64, 100}}, {0, {0x90, 67, 100}},
        {note_off, {0x80, 60, 0}},   {note_off, {0x80, 64, 0}}, {note_off, {0x80, 67, 0}},
    };
    const int nev = (int)(sizeof(events) / sizeof(events[0]));

    int16_t *pcm = calloc((size_t)total * 2, sizeof(int16_t));
    if (!pcm) { schwung_desktop_destroy(sd); return 1; }

    /*
     * THE TRANSPORT IS PART OF THE INSTRUMENT, NOT A DETAIL.
     *
     * A clock-gated module does nothing at all while the transport is
     * stopped, and it is RIGHT to do nothing -- breakbeat checks
     * get_clock_status() == RUNNING in five places before it will play. A
     * harness that never starts a clock therefore reports every one of them as
     * silent, which reads as "the module is broken" rather than "the test
     * never pressed play". That mistake cost a whole fleet sweep.
     *
     * --play advances a real beat position at --bpm, exactly as a DAW's
     * playhead does.
     */
    double beat = 0.0;
    double next_tick = 0.0;
    int started = 0;
    const double beats_per_block = (double)SCHWUNG_BLOCK / (double)SCHWUNG_RATE * bpm / 60.0;

    int ei = 0;
    for (int off = 0; off < total; off += SCHWUNG_BLOCK) {
        schwung_desktop_set_transport(sd, bpm, playing ? beat : -1.0, playing);

        /* THE REALTIME CLOCK IS A SEPARATE CHANNEL, and it is the one that
         * decides whether a clock-gated module plays at all. The chain
         * overrides get_clock_status with its own, answered from 0xFA/0xF8/
         * 0xFC bytes it has actually received -- setting the transport above
         * is invisible to it. */
        if (playing) {
            if (!started) {
                uint8_t start = 0xFA;
                schwung_desktop_midi(sd, &start, 1);
                started = 1;
                next_tick = 0.0;
            }
            double end_tick = (beat + beats_per_block) * 24.0;
            int guard = 0;
            while (next_tick <= end_tick && guard++ < 96) {
                uint8_t tick = 0xF8;
                schwung_desktop_midi(sd, &tick, 1);
                next_tick += 1.0;
            }
            beat += beats_per_block;
        }
        /* Fire every event whose frame falls INSIDE this block. Testing
         * `frame == off` silently never fires unless the frame happens to be a
         * multiple of the block size -- the probe harness lost three separate
         * measurements to exactly that (tools/probe, trap 2). */
        while (ei < nev && events[ei].frame >= off && events[ei].frame < off + SCHWUNG_BLOCK) {
            schwung_desktop_midi(sd, events[ei].b, 3);
            ei++;
        }
        /* --in-tone: a 220 Hz sine into the SPI mailbox's audio-in region,
         * which is where a line-input module reads from. Written immediately
         * before the render it belongs to. */
        if (in_tone) {
            static double ph = 0.0;
            int16_t inbuf[SCHWUNG_BLOCK * 2];
            for (int i = 0; i < SCHWUNG_BLOCK; i++) {
                int16_t v = (int16_t)(0.5 * 32767.0 * sin(ph));
                ph += 2.0 * M_PI * 220.0 / (double)SCHWUNG_RATE;
                inbuf[i * 2] = v; inbuf[i * 2 + 1] = v;
            }
            schwung_desktop_set_audio_in(sd, inbuf);
        }
        schwung_desktop_render(sd, pcm + (size_t)off * 2);
    }

    /* Report level, so "it ran" and "it made sound" are different claims. */
    double sum = 0.0; int peak = 0;
    for (int i = 0; i < total * 2; i++) {
        double s = pcm[i] / 32768.0;
        sum += s * s;
        int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    double rms = sqrt(sum / (total * 2));
    fprintf(stderr, "render: %d frames  rms %.1f dBFS  peak %.1f dBFS\n",
            total,
            rms  > 0 ? 20.0 * log10(rms) : -999.0,
            peak > 0 ? 20.0 * log10(peak / 32768.0) : -999.0);

    wav_write(out, pcm, total);
    fprintf(stderr, "render: wrote %s\n", out);

    free(pcm);
    schwung_desktop_destroy(sd);
    return 0;
}
