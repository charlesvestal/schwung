/* Unit test for chain_pre_tick_should_inject — the Pre-mode inject skip
 * decision for MIDI FX *tick* output (chain_pre_inject.h).
 *
 * Regression test for the arp-hang bug: a tick FX (arp) retriggers a held-pad
 * pitch with note-off/note-on pairs. The note-ON is skipped (Move plays the
 * pad natively), but the note-OFF used to be injected anyway — which silences
 * Move's held pad AND echoes back un-refcounted, re-entering the FX as a pad
 * release that drains its held notes, so the arp plays once and stops.
 * The fix: only inject a note-OFF for a pitch we actually injected a note-ON
 * for; a held-pad pitch we never injected must have its note-OFF skipped too.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "modules/chain/dsp/chain_pre_inject.h"

static int failures = 0;

static void check(const char *name, int got, int want) {
    if (got != want) {
        printf("FAIL: %s — got %d, want %d\n", name, got, want);
        failures++;
    } else {
        printf("ok: %s\n", name);
    }
}

int main(void) {
    uint8_t held[128];      memset(held, 0, sizeof(held));
    uint8_t injected[128];  memset(injected, 0, sizeof(injected));

    /* 1: note-on, pitch neither held nor injected → inject (normal arp note) */
    {
        uint8_t m[3] = {0x90, 60, 100};
        check("note-on free pitch injects",
              chain_pre_tick_should_inject(held, injected, m, 3), 1);
    }

    /* 2: note-on on a held-pad pitch → skip (Move plays it via the pad) */
    {
        held[60] = 1;
        uint8_t m[3] = {0x90, 60, 100};
        check("note-on held-pad pitch skips",
              chain_pre_tick_should_inject(held, injected, m, 3), 0);
        held[60] = 0;
    }

    /* 3: note-off for a pitch we injected a note-on for → inject (release it) */
    {
        injected[67] = 1;
        uint8_t m[3] = {0x80, 67, 0};
        check("note-off injected pitch releases",
              chain_pre_tick_should_inject(held, injected, m, 3), 1);
        injected[67] = 0;
    }

    /* 4: THE BUG — note-off for a held-pad pitch we never injected → skip.
     * The arp's retrigger note-off for the held pitch must not reach Move. */
    {
        held[60] = 1;               /* pad holding pitch 60 */
        /* injected[60] == 0: its note-on was skipped, so we never injected it */
        uint8_t m[3] = {0x80, 60, 0};
        check("note-off held-pad-not-injected skips (arp-hang guard)",
              chain_pre_tick_should_inject(held, injected, m, 3), 0);
        held[60] = 0;
    }

    /* 5: running-status note-off (0x90 vel 0) for held-pad-not-injected → skip */
    {
        held[62] = 1;
        uint8_t m[3] = {0x90, 62, 0};
        check("vel0 note-off held-pad-not-injected skips",
              chain_pre_tick_should_inject(held, injected, m, 3), 0);
        held[62] = 0;
    }

    /* 6: a CC (non-note channel message) always passes through */
    {
        uint8_t m[3] = {0xB0, 74, 64};
        check("CC injects",
              chain_pre_tick_should_inject(held, injected, m, 3), 1);
    }

    if (failures) { printf("\n%d test(s) FAILED\n", failures); return 1; }
    printf("\nAll passed\n");
    return 0;
}
