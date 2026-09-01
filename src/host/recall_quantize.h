/*
 * The arithmetic behind a quantized snapshot recall.
 *
 * Header-only and dependency-free for the same reason as master_fx_key.h and
 * fx_midi_filter.h: so tests/host can compile and RUN it natively. Its caller
 * lives in schwung_shim.c, which cannot be built on the dev machine — which is
 * how boundary maths like this ends up shipped untested, and boundary maths is
 * wrong SILENTLY. An off-by-one division fires a beat early forever and just
 * feels like bad timing.
 *
 * Pure: no allocation, no I/O, no globals. The call site is the SPI callback.
 */
#ifndef RECALL_QUANTIZE_H
#define RECALL_QUANTIZE_H

#include "transport_grid.h"

/* MIDI clock is 24 PPQN; a 4/4 bar is 96. */
#define RECALL_PULSES_PER_BEAT TRANSPORT_PULSES_PER_BEAT

/*
 * The pulse count at which a recall armed *now* should land.
 *
 * The NEXT boundary, never the one we are standing on. Pressing exactly on the
 * downbeat should give you the following one — a press that fired instantly
 * would be indistinguishable from quantize being off, which is the one
 * outcome that makes the feature look broken rather than early or late.
 */
static inline int recall_next_boundary(int pulses, int div)
{
    if (div <= 0) return -1;
    if (pulses < 0) pulses = 0;

    /* The grid is offset: the downbeat is pulse 1, not 0 (transport_grid.h).
     * This used to return multiples of `div`, so every quantized recall fired
     * one pulse early — 20.8 ms at 120 BPM, 125 ms at 20. Inaudible for a
     * snapshot recall in a way it was not for the metronome, which is how it
     * survived: same counter, same off-by-one, only one of them complained. */
    int p = pulses - TRANSPORT_DOWNBEAT_PULSE_OFFSET;

    /* Before the first clock after Start, the next boundary IS that downbeat. */
    if (p < 0) return TRANSPORT_DOWNBEAT_PULSE_OFFSET;

    return (p / div) * div + div + TRANSPORT_DOWNBEAT_PULSE_OFFSET;
}

/*
 * How many pulses EARLY to start, so the writes finish on the boundary.
 *
 * A recall is ~13 param writes at ~2.8 ms — around 36 ms — so starting on the
 * boundary lands the change a fourteenth of a beat late at 120bpm.
 *
 * Derived from tempo, not fixed: one pulse is 20.8 ms at 120bpm and 41.7 ms at
 * 60, so a constant lead would be either useless or a whole beat early
 * depending on the track. Clamped below the division so a slow tempo can never
 * reach back past the PREVIOUS boundary and fire the moment it is armed.
 */
static inline int recall_lead_pulses(float bpm, int div, int write_ms)
{
    if (div <= 1) return 0;
    if (!(bpm > 20.0f) || !(bpm < 300.0f)) bpm = 120.0f;
    float pulse_ms = 60000.0f / (bpm * (float)RECALL_PULSES_PER_BEAT);
    int lead = (int)((float)write_ms / pulse_ms);
    if (lead < 0) lead = 0;
    if (lead >= div) lead = div - 1;
    return lead;
}

/* Has an armed recall come due? `target` < 0 means nothing is armed. */
static inline int recall_should_fire(int pulses, int target, int lead)
{
    if (target < 0) return 0;
    return pulses >= target - lead;
}

#endif /* RECALL_QUANTIZE_H */
