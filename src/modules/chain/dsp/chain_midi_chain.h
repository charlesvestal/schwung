/*
 * Running a message set through part of the MIDI FX chain.
 *
 * Extracted from chain_midi.c for the same reason as chain_pre_inject.h: the
 * surrounding TU cannot be compiled on a dev machine, so the ordering rules
 * here would otherwise only ever be verified by ear on hardware.
 *
 * WHY IT EXISTS. There are two ways a MIDI FX emits. `process_midi` is a
 * response to an incoming message and was always chained stage to stage. But an
 * arpeggiator does not answer the incoming note -- it holds it and GENERATES on
 * its own clock, through `tick`, and that output went straight to the synth,
 * skipping every FX after it. So two arps did not chain: both ran off the same
 * input in parallel and both fed the synth. Reported on hardware as "i'd expect
 * arp 1 to output notes that are fed into arp 2".
 *
 * The `from` parameter is what makes this safe rather than merely careful. The
 * caller passes stage + 1, so a stage can never feed itself or anything
 * upstream: there is no cycle to detect, no depth counter, no re-entrancy.
 * Cycles are impossible by construction rather than prevented by a check.
 */
#ifndef CHAIN_MIDI_CHAIN_H
#define CHAIN_MIDI_CHAIN_H

#include <stdint.h>

/* Is this stage loaded, and not bypassed? Holes are legal -- a false answer
 * means SKIP this stage, never stop the walk. */
typedef int (*chain_midi_stage_active_fn)(void *ctx, int stage);

/* Run one message through one stage. Returns how many messages it produced,
 * writing at most `max` of them. Mirrors midi_fx_api_v1_t.process_midi. */
typedef int (*chain_midi_stage_process_fn)(void *ctx, int stage,
                                           const uint8_t *in, int in_len,
                                           uint8_t (*out)[3], int *out_lens,
                                           int max);

/*
 * Transform `msgs`/`lens` in place through stages [from, stage_count).
 * Returns the resulting message count.
 *
 * `scratch` is the caller's staging buffer, `max` entries wide -- passed in
 * rather than declared here so ONE buffer is reused across every stage. Nesting
 * a buffer per stage would make stack use grow with chain length, and this runs
 * in the audio callback.
 */
static inline int chain_midi_run_from(int from, int stage_count,
                                      chain_midi_stage_active_fn active,
                                      chain_midi_stage_process_fn process,
                                      void *ctx,
                                      uint8_t msgs[][3], int lens[], int count,
                                      uint8_t scratch[][3], int scratch_lens[],
                                      int max) {
    if (!active || !process || max <= 0) return count;
    for (int fx = (from < 0 ? 0 : from); fx < stage_count; fx++) {
        /* A stage swallowed everything (an arp given a note-off can emit
         * nothing). There is no message left to carry, so stop rather than
         * calling the rest with an empty set. */
        if (count <= 0) break;
        if (!active(ctx, fx)) continue;

        int next_count = 0;
        for (int m = 0; m < count && next_count < max; m++) {
            int room = max - next_count;
            int produced = process(ctx, fx, msgs[m], lens[m],
                                   &scratch[next_count], &scratch_lens[next_count],
                                   room);
            /*
             * BELIEVE THE BUFFER, NOT THE PLUGIN. `produced` is a third party's
             * report of how much it wrote. A plugin that over-reports -- a bug,
             * or a plugin built against a different MIDI_FX_MAX_OUT_MSGS -- would
             * otherwise push next_count past `max`, and the copy-back below
             * would run off the end of the CALLER's array. That is memory
             * corruption in the audio callback, surfacing anywhere but here.
             */
            if (produced > room) produced = room;
            if (produced > 0) next_count += produced;
        }
        count = next_count;
        for (int i = 0; i < count; i++) {
            msgs[i][0] = scratch[i][0];
            msgs[i][1] = scratch[i][1];
            msgs[i][2] = scratch[i][2];
            lens[i] = scratch_lens[i];
        }
    }
    return count;
}

#endif /* CHAIN_MIDI_CHAIN_H */
