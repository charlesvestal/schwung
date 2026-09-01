/*
 * transport_grid.h — where the beats actually are on shadow_transport_pulses.
 *
 * ONE FACT, TWO CONSUMERS. The metronome and quantized snapshot recall both
 * turn `shadow_transport_pulses` into beat and bar boundaries, and both had the
 * same off-by-one because the fact below was never written down anywhere.
 *
 * Header-only and dependency-free so tests/host can compile and run the
 * consumers natively; the call sites are in schwung_shim.c, which cannot be
 * built on the dev machine.
 */
#ifndef TRANSPORT_GRID_H
#define TRANSPORT_GRID_H

/* MIDI clock is 24 PPQN. */
#define TRANSPORT_PULSES_PER_BEAT 24

/*
 * THE DOWNBEAT IS AT PULSE 1, NOT PULSE 0.
 *
 * `shadow_transport_pulses` is zeroed on MIDI Start (0xFA) and then
 * INCREMENTED by every clock (0xF8) — and per the MIDI spec the first clock
 * after Start IS the downbeat. So beats land at 24N+1, and anything firing on
 * `pulses % 24 == 0` is one pulse early, forever.
 *
 * The error is TEMPO-SCALED, which is what makes it worse than it sounds: one
 * pulse is 20.8 ms at 120 BPM but 125 ms at 20 BPM.
 *
 * MEASURED, not inferred. Metronome click against a sequenced hihat in one
 * `mailbox_out.pcm` capture, 2026-09-01, at two tempos so a phase error and a
 * latency could be told apart:
 *
 *      20 BPM   click early by 144.3 ms  (sd 0.7, n=7)
 *     120 BPM   click early by  40.4 ms  (sd 1.0, n=40)
 *
 *     125.00k + L = 144.3       ->  k = 0.997 pulses
 *      20.83k + L =  40.4           L = 19.6 ms (Link Audio transit)
 *
 * One tempo could not have separated those two terms: 144 ms at 20 BPM fits a
 * pulse error, a latency, or any mix of them.
 *
 * CORRECTED IN THE CONSUMERS, NOT IN THE COUNTER. `shadow_transport_pulses` is
 * a truthful count of clocks received since Start; it is the *interpretation*
 * of where beats sit that was wrong. Rebasing the counter so pulse 0 is the
 * downbeat also removes the 0 -> 1 transition the metronome needs to detect its
 * very first downbeat, so the first click of every take would go missing —
 * a fix that breaks the thing it was meant to repair.
 */
#define TRANSPORT_DOWNBEAT_PULSE_OFFSET 1

#endif /* TRANSPORT_GRID_H */
