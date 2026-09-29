/* lane_trace.h — the ring the lane diagnostic records through.
 *
 * What matters here is that a FULL ring keeps the OLDEST samples. The take is
 * recorded first and the quiet loops after it come later, so a ring that
 * overwrites from the front throws away the only part worth having and still
 * produces a plausible-looking file -- the failure this test exists to stop.
 */
#include "lane_trace.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                       printf("\n"); fails++; } } while (0)

int main(void) {
    static lane_trace_t t;
    lane_trace_entry_t e;

    CHECK(lane_trace_pop(&t, &e) == 0, "an empty ring answered a sample");

    lane_trace_push(&t, 7, 2, "ph=1.0 drv=1");
    CHECK(lane_trace_pop(&t, &e) == 1, "a pushed sample did not come back");
    CHECK(e.frame == 7 && e.slot == 2 && strcmp(e.line, "ph=1.0 drv=1") == 0,
          "the sample came back as frame=%u slot=%u \"%s\"", e.frame, e.slot, e.line);
    CHECK(lane_trace_pop(&t, &e) == 0, "the ring answered twice for one push");

    /* FIFO across a wrap of the index, which is where an off-by-one lives. */
    memset(&t, 0, sizeof(t));
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < LANE_TRACE_ENTRIES - 1; i++) {
            char b[32]; snprintf(b, sizeof(b), "%d", i);
            lane_trace_push(&t, (uint32_t)i, 0, b);
        }
        for (int i = 0; i < LANE_TRACE_ENTRIES - 1; i++) {
            if (!lane_trace_pop(&t, &e)) { CHECK(0, "ran dry at %d in round %d", i, round); break; }
            CHECK(e.frame == (uint32_t)i, "round %d wanted %d got %u", round, i, e.frame);
        }
    }
    CHECK(t.dropped == 0, "a ring that never filled reported %u drops", t.dropped);

    /* FULL KEEPS THE OLDEST. Push one more than it holds, then read it out:
     * entry 0 must still be there and the LAST push must be the one missing. */
    memset(&t, 0, sizeof(t));
    for (int i = 0; i < LANE_TRACE_ENTRIES + 50; i++) {
        char b[32]; snprintf(b, sizeof(b), "%d", i);
        lane_trace_push(&t, (uint32_t)i, 0, b);
    }
    CHECK(t.dropped == 51, "a ring overrun by 51 reported %u drops", t.dropped);
    CHECK(lane_trace_pop(&t, &e) == 1 && e.frame == 0,
          "the OLDEST sample was overwritten -- the take is the part that is "
          "recorded first, so a full ring must drop the NEW end (got %u)",
          e.frame);
    uint32_t last = 0, n = 1;
    while (lane_trace_pop(&t, &e)) { last = e.frame; n++; }
    CHECK(n == LANE_TRACE_ENTRIES - 1, "a full ring held %u of %d",
          n, LANE_TRACE_ENTRIES - 1);
    CHECK(last == LANE_TRACE_ENTRIES - 2,
          "the newest kept sample was %u, not the one before the overrun", last);

    /* A line longer than the entry is TRUNCATED AND TERMINATED, never run off
     * the end -- the producer is on the SPI callback. */
    memset(&t, 0, sizeof(t));
    char big[LANE_TRACE_LINE_MAX * 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    lane_trace_push(&t, 1, 0, big);
    CHECK(lane_trace_pop(&t, &e) == 1, "an over-long line was refused entirely");
    CHECK(strlen(e.line) == LANE_TRACE_LINE_MAX - 1,
          "an over-long line stored %zu chars", strlen(e.line));

    /* THE RATE MUST NOT DEPEND ON THE CALLER'S CADENCE.
     *
     * The old assertions here checked `frame % 17` in isolation and passed
     * while the instrument ran 16x slower than its own header claimed. The
     * only caller is shadow_lanes_publish_driving(), which the shim runs every
     * 16th frame; with a 17-frame period the two gates are coprime, so the
     * sampler fired every 272 frames -- 1.26 Hz against a documented 20 Hz,
     * 0.79 s between samples, and the question the trace exists to answer
     * (one beat of silence, or one loop?) is finer than that.
     *
     * So drive it the way the shim does -- frames 16 apart, never 1 apart --
     * and demand the call count, not the frame numbers, set the rate. */
    {
        const int calls = 1000;
        int got = 0;
        for (int i = 0; i < calls; i++)
            if (lane_trace_should_sample((uint32_t)i * 16u)) got++;
        const int want = calls / LANE_TRACE_EVERY_CALLS;
        CHECK(got >= want - 1 && got <= want + 1,
              "a 16-frame caller got %d samples in %d calls, wanted ~%d "
              "(rate is following the frame number, not the call)",
              got, calls, want);
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: lane_trace\n");
    return 0;
}
