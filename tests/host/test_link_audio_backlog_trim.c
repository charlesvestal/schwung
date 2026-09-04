/*
 * The Move->Schwung link-in ring has no set-point: producer and consumer both
 * run at one block per SPI frame, so `avail` keeps whatever offset a
 * disturbance gives it, for the session. Measured on a Move loading jp8000 with
 * Move->Schwung on -- 10.97 ms mean before, 85.05 ms mean after, both stable to
 * the millisecond over six 5 s windows.
 *
 * link_audio_in_trim_pos() removes a backlog that has been high for a WHILE.
 * The "for a while" is the whole design: trimming on a single elevated read is
 * how the old 35 ms catch-up came to discard the burst that covers Move's next
 * stall.
 */
#include "link_audio.h"
#include <stdio.h>
#include <stdint.h>

static int fails = 0;
static void check(int cond, const char *what) {
    printf("  %-4s %s\n", cond ? "ok:" : "FAIL", what);
    if (!cond) fails++;
}

#define CEIL   ((uint32_t)LINK_AUDIO_IN_TRIM_CEILING)
#define TGT    ((uint32_t)LINK_AUDIO_IN_TRIM_TARGET)
#define SUS    ((uint32_t)LINK_AUDIO_IN_TRIM_SUSTAIN)

/* Drive n consecutive reads at a fixed avail; return how many trims fired. */
static int drive(uint32_t avail, uint32_t n, uint32_t *run, uint32_t *last_out) {
    int trims = 0;
    const uint32_t rp = 1000;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t out;
        if (link_audio_in_trim_pos(rp + avail, rp, run, &out)) { trims++; if (last_out) *last_out = out; }
    }
    return trims;
}

int main(void) {
    uint32_t run = 0, out = 0;
    printf("link audio backlog trim\n");

    /* A backlog held above the ceiling for the sustain window trims ONCE. */
    run = 0;
    check(drive(CEIL + 1, SUS - 1, &run, &out) == 0,
          "no trim before the sustain window elapses");
    check(drive(CEIL + 1, 1, &run, &out) == 1,
          "trims once the backlog has been high for the whole window");
    check(out == (uint32_t)(1000 + CEIL + 1 - TGT),
          "…leaving exactly TARGET pending");
    check((uint32_t)(1000 + CEIL + 1) - out == TGT, "…i.e. TARGET behind the writer");

    /* Move's stall-and-burst: elevated, then it drops. Must NOT trim -- this is
     * the case the old 35 ms catch-up got wrong. */
    run = 0;
    check(drive(CEIL + 1, SUS / 2, &run, &out) == 0, "a burst alone does not trim");
    check(drive(TGT, 1, &run, &out) == 0, "…and a dip below the ceiling…");
    check(run == 0, "…resets the run, so the burst cannot accumulate across dips");
    check(drive(CEIL + 1, SUS - 1, &run, &out) == 0,
          "…so the next elevated stretch starts counting from zero");

    /* Healthy operation never trims. Measured baseline was ~11 ms mean,
     * 12.38 ms max; the ceiling must sit well above that. */
    run = 0;
    check(drive(TGT, SUS * 3, &run, &out) == 0, "a healthy backlog never trims");
    check(CEIL > (uint32_t)(12 * 2 * 44),
          "ceiling sits above the 12.4 ms healthy max measured on the device");

    /* Never rewind: below the target there is nothing to drop. */
    run = 0;
    { uint32_t r2 = SUS; uint32_t o2;
      check(link_audio_in_trim_pos(1000 + TGT / 2, 1000, &r2, &o2) == 0,
            "a backlog under the target never rewinds the reader"); }

    /* The 32-bit wrap: both positions free-run. */
    { const uint32_t rp = 0xFFFFFF00u, wp = rp + CEIL + 64;
      uint32_t r3 = SUS - 1, o3;
      check(wp < rp, "…the test really does straddle the wrap");
      check(link_audio_in_trim_pos(wp, rp, &r3, &o3) == 1,
            "a backlog straddling the wrap still trims");
      check(o3 == (uint32_t)(wp - TGT), "…and lands TARGET behind the writer"); }

    /* Margins. */
    check(TGT < CEIL, "TARGET below CEILING, or trimming does nothing");
    check(CEIL < (uint32_t)LINK_AUDIO_IN_CATCHUP_SAMPLES,
          "CEILING below catch-up, which handles the runaway case");
    check(SUS >= 300, "sustain is ~1 s of reads, not a handful");

    /* THE STARVATION FLOOR, MEASURED. Worst producer gap on the device was
     * 15.9 ms and `avail` swings ~10 ms below its own mean, so a target under
     * ~26 ms puts the dips beneath the gaps. Setting it to 12 ms did exactly
     * that: la_starve_fallback=400 then 288, about 1.2 s of missing Move audio.
     * This is the check that stops someone reclaiming latency back into a
     * dropout. */
    check(TGT >= (uint32_t)(26 * 2 * 44),
          "target above the measured starvation floor (~26 ms)");
    check(TGT <= (uint32_t)(60 * 2 * 44),
          "…but not so deep that Move's tracks lag audibly (<=60 ms)");

    if (fails) { printf("FAIL: %d check(s)\n", fails); return 1; }
    printf("PASS: link audio backlog trim\n");
    return 0;
}
