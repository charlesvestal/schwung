/*
 * The Chance GATE: what a slot does with each note Move plays into it.
 *
 * A note-on that matches a stored condition is rolled; a note-on that loses
 * is dropped and REMEMBERED, so its note-off is dropped too -- delivering the
 * off alone would be harmless to most synths and wrong to every one that
 * counts voices or latches (an arp's held set, a mono synth's note stack).
 *
 * One roll per TRIG per PASS: every note set on the same step (sc_entry_t.grp)
 * in the same pass shares the first note's result -- a chord played in live
 * starts a few ms apart per note, so the start itself cannot be the key -- so a chord drops as a unit
 * rather than thinning note by note. Elektron behaves the same way -- the
 * condition belongs to the trig, not to each note in it.
 *
 * Unknown phase delivers. A note is never dropped for want of a clock:
 * stopped transport, a clip Move has not placed, a live pad press with nothing
 * playing -- all pass straight through.
 *
 * Pure: tests/host drives it. RT-safe: fixed state, no allocation.
 */
#ifndef STEP_CHANCE_GATE_H
#define STEP_CHANCE_GATE_H

#include <stdint.h>
#include <string.h>
#include "step_chance_store.h"

typedef struct {
    uint8_t  dropped[16][16];   /* [channel][note/8] bitmask of dropped notes */
    int      last_valid;
    int      last_row;
    long     last_pass;
    double   last_grp;
    int      last_cond;         /* the decision is per CONDITION too */
    int      last_play;
    uint32_t rng;               /* xorshift32; 0 = unseeded */
    uint32_t matched;           /* note-ons that carried a condition */
    uint32_t dropped_n;         /* ...and of those, how many lost */
    int      transpose;         /* the slot's semitones, ALREADY applied by the
                                 * shim before the note reached us: the store
                                 * holds Move's pitch, so a match subtracts it */
} sc_gate_t;

static inline uint32_t sc_gate_rand(sc_gate_t *g)
{
    uint32_t x = g->rng ? g->rng : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->rng = x;
    return x;
}

/* 1 = deliver the message, 0 = drop it. `msg` is a 3-byte channel message. */
static inline int sc_gate(sc_gate_t *g, const sc_store_t *st, const uint8_t *msg, int len,
                          int phase_valid, double phase, double loop_start, double loop_len,
                          int row, long pass)
{
    if (!g || !msg || len < 3) return 1;
    const uint8_t type = msg[0] & 0xF0, ch = msg[0] & 0x0F, note = msg[1] & 0x7F;
    const uint8_t bit = (uint8_t)(1u << (note & 7));
    const int is_on = (type == 0x90 && msg[2] > 0);
    const int is_off = (type == 0x80 || (type == 0x90 && msg[2] == 0));

    if (is_off) {
        if (g->dropped[ch][note >> 3] & bit) {
            g->dropped[ch][note >> 3] &= (uint8_t)~bit;
            return 0;
        }
        return 1;
    }
    if (!is_on) return 1;
    /* A note-on for a pitch whose previous on we dropped: that one is over. */
    g->dropped[ch][note >> 3] &= (uint8_t)~bit;
    if (!st || !phase_valid || row < 0) return 1;

    /* SLOT TRANSPOSE: the shim moved the note before we saw it, and the store
     * holds the pitch MOVE played -- so match on that one. Matching on the
     * delivered pitch silenced chance on every transposed slot (hardware,
     * 2026-10-01: +12, "0 0" matched). The dropped set above stays keyed on
     * the DELIVERED pitch, which is what its note-off will carry. */
    const int move_note = (int)note - g->transpose;
    if (move_note < 0 || move_note > 127) return 1;

    double grp = 0.0;
    int wrap = 0;
    const int cond = sc_store_match_ex(st, row, move_note, phase, loop_start, loop_len, &grp, &wrap);
    if (cond == SC_ALWAYS) return 1;
    /* Matched across the wrap: the note starts the NEXT pass. */
    if (pass >= 0) pass += wrap;
    if (pass < 0 && wrap < 0) pass = -1;

    g->matched++;
    int play;
    /* One roll per step per pass so a chord rolls whole -- but only among
     * notes that share a CONDITION. Keyed without it, two drum voices on one
     * step with different conditions (a hat at 4:4 beside a tom at 1:5)
     * played whatever the FIRST one decided: the 4:4 hat sounded on pass 0
     * (hardware, 2026-10-04). */
    if (g->last_valid && g->last_row == row && g->last_pass == pass &&
        g->last_grp == grp && g->last_cond == cond) {
        play = g->last_play;
    } else {
        play = sc_should_play(cond, pass, sc_gate_rand(g));
        g->last_valid = 1; g->last_row = row; g->last_pass = pass;
        g->last_grp = grp; g->last_cond = cond; g->last_play = play;
    }
    if (!play) { g->dropped[ch][note >> 3] |= bit; g->dropped_n++; }
    return play;
}

#endif /* STEP_CHANCE_GATE_H */
