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
    uint8_t frame[1024];
    for (int i = 0; i < 1024; i++) {
        // Checkerboard at byte granularity: alternating 0xAA / 0x55 by row.
        int row = i / 128;
        frame[i] = (row & 1) ? 0xAA : 0x55;
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
