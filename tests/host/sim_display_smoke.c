// SPDX-License-Identifier: MIT
//
// Phase 4 verification: write a 128×64 1-bit test pattern to the display SHM
// without touching CoreAudio. Combined with display-server + curl, this lets
// us confirm the full SHM → SSE path is working before any audio risk.
//
//   $ ./build/mac/display-server &       # serves SSE on :7681
//   $ ./build/mac/sim_display_smoke      # writes a checkerboard, exits
//   $ curl -s http://localhost:7681/stream | head -2
//   → should see "data: {...}" lines with base64 of the checkerboard.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host/sim_backend.h"

int main(void) {
    // 128 × 64 / 8 = 1024 bytes packed as the display protocol expects:
    // each byte is 8 vertical pixels (LSB top) for a column, rows packed in groups.
    // For a smoke test we don't care about exact orientation — just write a
    // recognizable pattern.
    // 8×8 block checkerboard. The framebuffer is packed as 8 row-groups of 128
    // columns, each byte holding 8 vertical pixels for one column (LSB = top).
    // For a visible 8×8 block checker, set all 8 vertical bits of a byte
    // (0xFF) when (group XOR (column/8)) is odd, otherwise 0.
    uint8_t frame[1024];
    for (int g = 0; g < 8; g++) {
        for (int x = 0; x < 128; x++) {
            int checker = ((g + (x / 8)) & 1);
            frame[g * 128 + x] = checker ? 0xFF : 0x00;
        }
    }

    if (schwung_sim_push_display(frame) != 0) {
        fprintf(stderr, "schwung_sim_push_display failed\n");
        return 1;
    }

    fprintf(stderr,
        "sim_display_smoke: wrote checkerboard to /schwung-display-live\n"
        "Run display-server in another shell and:\n"
        "    curl -s http://localhost:7681/stream | head -2\n");
    return 0;
}
