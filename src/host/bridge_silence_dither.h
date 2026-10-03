/*
 * bridge_silence_dither.h -- never hand Move an all-zero audio-in block.
 *
 * The native resample bridge (OVERWRITE) replaces Move's audio input with
 * Schwung's mix so Move's own sampler records it. With nothing playing that
 * mix is EXACT digital silence, and a real converter never delivers that --
 * the hardware input has a noise floor in every block (0 all-zero blocks in
 * 3450, measured). Move's engine counts an all-zero input block as a dropout
 * and logs `audio-dropouts:N` to syslog 43 times a second, ~200 KB a minute
 * written to the device's storage, for as long as the bridge is on.
 *
 * Measured on Move 2.1.5b1 (2026-10-03): stock Move 0, bridge off 0, bridge
 * on ~1 per block, bridge on with this dither 0.
 *
 * A block that is all zero gets TPDF dither of +-1 LSB (about -90 dBFS) --
 * inaudible, and it keeps the block from being bit-identical silence. A block
 * with ANY nonzero sample is left untouched, so audio is never altered.
 *
 * Pure: no allocation, no I/O; the RNG state is the caller's.
 */
#ifndef BRIDGE_SILENCE_DITHER_H
#define BRIDGE_SILENCE_DITHER_H

#include <stddef.h>
#include <stdint.h>

/* Returns 1 when the block was silent and has been dithered, else 0. */
static inline int bridge_dither_if_silent(int16_t *buf, size_t samples, uint32_t *rng)
{
    for (size_t i = 0; i < samples; i++)
        if (buf[i]) return 0;
    uint32_t r = *rng;
    for (size_t i = 0; i < samples; i++) {
        r = r * 1664525u + 1013904223u;
        const int a = (int)((r >> 16) & 1u);
        r = r * 1664525u + 1013904223u;
        const int b = (int)((r >> 16) & 1u);
        buf[i] = (int16_t)(a - b);           /* triangular: -1, 0 or +1 */
    }
    *rng = r;
    return 1;
}

#endif /* BRIDGE_SILENCE_DITHER_H */
