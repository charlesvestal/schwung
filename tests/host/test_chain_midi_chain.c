/*
 * chain_midi_run_from: generated MIDI travelling the rest of the chain.
 *
 * The bug: an arpeggiator does not emit from process_midi, it GENERATES on its
 * own clock via tick(), and that output went straight to the synth -- skipping
 * every FX after it. So two arps did not chain. Both ran off the same input in
 * parallel and both fed the synth. Reported on hardware as "i'd expect arp 1 to
 * output notes that are fed into arp 2".
 *
 * The rules worth pinning are the ones that are invisible by ear until they are
 * wrong: that a stage never feeds itself, that an inactive stage is skipped
 * rather than ending the walk, and that the output buffer cannot be overrun by
 * a stage that multiplies messages.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "chain_midi_chain.h"

#define MAXM 16

static int failures = 0;
static void fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); failures++; }

/* A fake chain. Each stage has a behaviour and records that it ran. */
typedef enum { PASS, DOUBLE, SWALLOW, FLOOD, LIAR } behave_t;

typedef struct {
    behave_t behave[8];
    int      active[8];
    int      calls[8];      /* how many messages each stage was handed */
    int      active_calls[8]; /* whether the walk even CONSIDERED this stage */
    int      stages;
} fake_t;

static int fake_active(void *ctx, int stage) {
    fake_t *f = (fake_t *)ctx;
    f->active_calls[stage]++;
    return f->active[stage];
}

static int fake_process(void *ctx, int stage, const uint8_t *in, int in_len,
                        uint8_t (*out)[3], int *out_lens, int max) {
    fake_t *f = (fake_t *)ctx;
    f->calls[stage]++;
    switch (f->behave[stage]) {
        case SWALLOW:
            return 0;
        case DOUBLE: {
            int n = 0;
            for (int i = 0; i < 2 && n < max; i++, n++) {
                out[n][0] = in[0];
                out[n][1] = (uint8_t)(in[1] + 1);  /* transpose, so order is visible */
                out[n][2] = in[2];
                out_lens[n] = in_len;
            }
            return n;
        }
        case FLOOD: {
            /* Always tries to emit more than the buffer can hold. */
            int n = 0;
            for (int i = 0; i < MAXM * 2 && n < max; i++, n++) {
                out[n][0] = in[0]; out[n][1] = in[1]; out[n][2] = in[2];
                out_lens[n] = in_len;
            }
            return n;
        }
        case LIAR:
            /* A MISBEHAVING plugin: writes what it was allowed, then reports
             * more than it wrote. The chain must believe the buffer, not the
             * plugin -- this is what the `next_count < max` bound defends. */
            if (max >= 1) {
                out[0][0] = in[0]; out[0][1] = in[1]; out[0][2] = in[2];
                out_lens[0] = in_len;
            }
            return max + 4;
        case PASS:
        default:
            if (max < 1) return 0;
            out[0][0] = in[0]; out[0][1] = in[1]; out[0][2] = in[2];
            out_lens[0] = in_len;
            return 1;
    }
}

static int run(fake_t *f, int from, uint8_t msgs[][3], int lens[], int count) {
    uint8_t scratch[MAXM][3];
    int scratch_lens[MAXM];
    return chain_midi_run_from(from, f->stages, fake_active, fake_process, f,
                               msgs, lens, count, scratch, scratch_lens, MAXM);
}

static void seed(uint8_t msgs[][3], int lens[], uint8_t note) {
    msgs[0][0] = 0x90; msgs[0][1] = note; msgs[0][2] = 100; lens[0] = 3;
}

int main(void) {
    /* ---- 1. A stage never feeds itself, and never feeds upstream --------- *
     *
     * This is the whole safety argument: `from` is stage + 1, so there is no
     * cycle to detect. If stage 0 ran here, an arp would re-arpeggiate its own
     * output forever. */
    {
        fake_t f = {0};
        f.stages = 3;
        for (int i = 0; i < 3; i++) { f.active[i] = 1; f.behave[i] = PASS; }
        uint8_t msgs[MAXM][3]; int lens[MAXM];
        seed(msgs, lens, 60);
        run(&f, 1, msgs, lens, 1);
        if (f.calls[0] != 0) fail("stage 0 ran when starting from 1 - a stage can feed itself");
        if (f.calls[1] != 1 || f.calls[2] != 1) fail("downstream stages did not both run");
    }

    /* ---- 2. Serial, not parallel: stage 2 sees what stage 1 produced ----- */
    {
        fake_t f = {0};
        f.stages = 3;
        f.active[1] = f.active[2] = 1;
        f.behave[1] = DOUBLE; f.behave[2] = DOUBLE;
        uint8_t msgs[MAXM][3]; int lens[MAXM];
        seed(msgs, lens, 60);
        int n = run(&f, 1, msgs, lens, 1);
        if (n != 4) {
            fprintf(stderr, "  got %d messages\n", n);
            fail("two doubling stages should give 4 - if it is 2, the second stage "
                 "ran on the ORIGINAL input and the stages are parallel, not chained");
        }
        /* Transposed twice, so the value proves it went THROUGH both. */
        if (n >= 1 && msgs[0][1] != 62) fail("the message did not pass through both stages");
        if (f.calls[2] != 2) fail("stage 2 was not handed both of stage 1 outputs");
    }

    /* ---- 3. An inactive stage is SKIPPED, not a stop -------------------- *
     *
     * Holes are legal in this list. Stopping at one would silently drop every
     * stage after a bypassed or empty position. */
    {
        fake_t f = {0};
        f.stages = 4;
        f.active[1] = 0;                 /* bypassed or a hole */
        f.active[2] = 1; f.behave[2] = PASS;
        f.active[3] = 1; f.behave[3] = PASS;
        uint8_t msgs[MAXM][3]; int lens[MAXM];
        seed(msgs, lens, 60);
        int n = run(&f, 1, msgs, lens, 1);
        if (f.calls[2] != 1 || f.calls[3] != 1) fail("an inactive stage stopped the walk instead of being skipped");
        if (n != 1) fail("a skipped stage changed the message count");
    }

    /* ---- 4. A stage that swallows everything ends the walk --------------- *
     *
     * An arp handed a note-off can legitimately emit nothing. There is then no
     * message to carry, and the stages after it must not be called with an
     * empty set. */
    {
        fake_t f = {0};
        f.stages = 3;
        f.active[1] = 1; f.behave[1] = SWALLOW;
        f.active[2] = 1; f.behave[2] = PASS;
        uint8_t msgs[MAXM][3]; int lens[MAXM];
        seed(msgs, lens, 60);
        int n = run(&f, 1, msgs, lens, 1);
        if (n != 0) fail("a swallowing stage should leave no messages");
        if (f.calls[2] != 0) fail("a stage after a swallow was called with an empty set");
        /* The message-count check above passes either way -- with count 0 the
         * inner loop does nothing regardless. What the early break actually
         * changes is whether the walk keeps CONSIDERING stages, so assert that
         * instead; otherwise removing the break is invisible. */
        if (f.active_calls[2] != 0) {
            fail("the walk kept going after every message was swallowed - "
                 "there is nothing left to carry");
        }
    }

    /* ---- 5. The buffer cannot be overrun -------------------------------- *
     *
     * This is the one the user asked about: chaining arps multiplies notes.
     * Multiplication is intended; writing past the buffer is not. */
    {
        fake_t f = {0};
        f.stages = 4;
        for (int i = 1; i < 4; i++) { f.active[i] = 1; f.behave[i] = FLOOD; }
        uint8_t msgs[MAXM][3]; int lens[MAXM];
        seed(msgs, lens, 60);
        int n = run(&f, 1, msgs, lens, 1);
        if (n > MAXM) fail("the message count ran past the buffer");
        if (n != MAXM) fail("a flooding chain should fill the buffer exactly, then clamp");
    }

    /* ---- 5b. A plugin that LIES about how much it wrote ------------------ *
     *
     * The buffer bound is not just arithmetic hygiene: a third-party MIDI FX
     * that returns a count larger than it wrote would otherwise walk the copy
     * loop past the end of the caller message array. Canaries either side make
     * that visible as a failure rather than as memory corruption somewhere
     * else entirely. */
    {
        struct { uint8_t before[8]; uint8_t msgs[MAXM][3]; uint8_t after[8]; } box;
        int lens[MAXM];
        memset(&box, 0xAA, sizeof(box));
        fake_t f = {0};
        f.stages = 3;
        f.active[1] = 1; f.behave[1] = LIAR;
        f.active[2] = 1; f.behave[2] = PASS;
        seed(box.msgs, lens, 60);
        int n = run(&f, 1, box.msgs, lens, 1);
        if (n > MAXM) fail("believed a plugin that over-reported its output count");
        for (int i = 0; i < 8; i++) {
            if (box.before[i] != 0xAA || box.after[i] != 0xAA) {
                fail("wrote past the caller message buffer - a lying plugin "
                     "corrupted memory around it");
                break;
            }
        }
    }

    /* ---- 6. Degenerate inputs ------------------------------------------- */
    {
        fake_t f = {0};
        f.stages = 2;
        f.active[0] = f.active[1] = 1;
        uint8_t msgs[MAXM][3]; int lens[MAXM];
        seed(msgs, lens, 60);
        /* from past the end: nothing to do, count unchanged. */
        if (run(&f, 5, msgs, lens, 1) != 1) fail("from past the last stage changed the count");
        if (f.calls[0] || f.calls[1]) fail("from past the last stage ran a stage anyway");
        /* A null callback must not crash or invent messages. */
        uint8_t scratch[MAXM][3]; int scratch_lens[MAXM];
        if (chain_midi_run_from(0, 2, NULL, fake_process, &f, msgs, lens, 1,
                                scratch, scratch_lens, MAXM) != 1) {
            fail("a null active callback changed the count");
        }
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d chain_midi_run_from check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: chain_midi_run_from - generated notes chain downstream only, "
           "holes skip, swallow stops, buffer clamps\n");
    return 0;
}
