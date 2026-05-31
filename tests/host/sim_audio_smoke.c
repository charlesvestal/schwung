// SPDX-License-Identifier: MIT
//
// Phase 3b verification: 440 Hz sine wave through the full path
//   tone gen → mailbox[256..] → CoreAudio → speakers
//
// Run: ./build/mac/sim_audio_smoke
// Expect: a quiet 2-second 440 Hz tone from the default output device.

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "host/audio_backend.h"
#include "host/sim_backend.h"
#include "schwung_spi_lib.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static volatile int running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

static void *render_thread(void *arg) {
    uint8_t *mailbox = (uint8_t *)arg;
    int16_t *out = (int16_t *)(mailbox + SCHWUNG_OFF_OUT_AUDIO);
    double phase = 0.0;
    const double freq = 440.0;
    const double sr   = (double)SCHWUNG_SAMPLE_RATE;
    const double step = 2.0 * M_PI * freq / sr;
    const double amp  = 0.10;  // quiet — 10% scale to int16 max

    while (running) {
        // Generate 128 frames of stereo sine.
        for (int i = 0; i < SCHWUNG_AUDIO_FRAMES; i++) {
            int16_t s = (int16_t)(sin(phase) * amp * 32767.0);
            out[2 * i + 0] = s;
            out[2 * i + 1] = s;
            phase += step;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        // Block until audio backend's render callback fires the tick pipe.
        if (schwung_sim_ioctl_wait() != 0) break;
    }
    return NULL;
}

int main(void) {
    signal(SIGINT, on_sigint);

    if (schwung_sim_open() < 0) {
        fprintf(stderr, "sim_open failed\n");
        return 1;
    }
    uint8_t *mailbox = schwung_sim_mmap();
    if (!mailbox) {
        fprintf(stderr, "sim_mmap failed\n");
        return 1;
    }

    pthread_t t;
    pthread_create(&t, NULL, render_thread, mailbox);

    if (schwung_audio_start(mailbox, schwung_sim_get_tick_fd()) != 0) {
        fprintf(stderr, "audio_start failed\n");
        running = 0;
        pthread_join(t, NULL);
        return 1;
    }

    fprintf(stderr, "playing 440 Hz for 2s — listen for a tone...\n");
    sleep(2);
    fprintf(stderr, "done.\n");

    running = 0;
    schwung_audio_stop();
    // Render thread is blocked on read(tick_pipe); audio_stop won't wake it.
    // Force-exit; the OS reaps the thread.
    return 0;
}

