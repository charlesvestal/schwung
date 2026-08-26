/* Unit test: SPI frame tally — kernel transfer time + IRQ backlog.
 *
 * The thing being measured is a COMPARISON between two counters that come from
 * different places and wrap differently, so most of these tests are about the
 * ways the comparison can lie:
 *
 *   - the /proc counter is printed from an int and goes negative past 2^31, so
 *     the delta must stay a 32-bit subtraction of 32-bit operands; widening
 *     either side turns the wrap into a 4-billion-IRQ window;
 *   - the driver leaves spi_tx_time at zero until the first transfer, so
 *     folding that in pins the mean low;
 *   - `irqs < frames` is the backlog DRAINING, not a negative backlog, and an
 *     unsigned subtraction there underflows to ~2^32.
 *
 * Each of those produces a plausible-looking number rather than an error,
 * which is why they are pinned individually.
 */
#include <stdio.h>
#include <string.h>

#include "spi_tally.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

/* Advance the accumulator by `n` frames each costing `ns`. */
static void run_frames(spi_tally_t *t, int n, uint32_t ns)
{
    for (int i = 0; i < n; i++) spi_tally_record(t, ns);
}

static void test_a_zero_transfer_time_is_not_a_sample(void)
{
    spi_tally_t t;
    spi_tally_state_t s;
    spi_tally_reset(&t, &s);

    /* ablspi does not touch sysinfo->spi_tx_time until a transfer completes,
     * so the field reads 0 on the very first frames. Counting those as
     * zero-nanosecond transfers drags the mean toward zero and makes the
     * headroom look like a full period. */
    spi_tally_record(&t, 0);
    spi_tally_record(&t, 0);
    CHECK(t.frames == 0, "zero tx_ns must not count as a frame");
    CHECK(t.tx_ns_sum == 0, "zero tx_ns must not enter the sum");

    spi_tally_record(&t, 300000);
    CHECK(t.frames == 1, "a real sample counts");
    CHECK(t.tx_ns_max == 300000, "max tracks the first real sample");
}

static void test_max_is_the_worst_since_arming(void)
{
    spi_tally_t t;
    spi_tally_state_t s;
    spi_tally_reset(&t, &s);

    spi_tally_record(&t, 300000);
    spi_tally_record(&t, 1900000);
    spi_tally_record(&t, 310000);

    CHECK(t.tx_ns_max == 1900000, "max keeps the outlier, not the latest");
    CHECK(t.tx_ns_last == 310000, "last is the latest");

    /* The peak is deliberately NOT per-window: a spike that happens once an
     * hour is the whole finding, and a per-window max hands it to whichever
     * one-second bucket it landed in and then throws it away. */
    spi_tally_sample_t out;
    spi_tally_fold(&s, &t, 3, &out);          /* baseline */
    run_frames(&t, 10, 300000);
    spi_tally_fold(&s, &t, 13, &out);
    CHECK(out.max_ns == 1900000, "the peak survives a window boundary");
}

static void test_the_first_fold_is_a_baseline(void)
{
    spi_tally_t t;
    spi_tally_state_t s;
    spi_tally_sample_t out;
    spi_tally_reset(&t, &s);

    run_frames(&t, 44, 300000);
    spi_tally_fold(&s, &t, 1000, &out);

    CHECK(out.first == 1, "the first fold reports itself as a baseline");
    CHECK(out.frames == 0, "no frame delta is claimed from one sample");
    CHECK(out.late == 0, "no backlog is claimed from one sample");

    /* Arming mid-session means the counters already hold hours of history.
     * Reporting that history as one window's work would read as a colossal
     * overrun on the very first line. */
    spi_tally_fold(&s, &t, 1000, &out);
    CHECK(out.first == 0, "the second fold is a real window");
    CHECK(out.frames == 0, "…and it measures from the baseline, not from zero");
}

static void test_a_clean_window_has_no_backlog(void)
{
    spi_tally_t t;
    spi_tally_state_t s;
    spi_tally_sample_t out;
    spi_tally_reset(&t, &s);

    spi_tally_fold(&s, &t, 500, &out);        /* baseline */
    run_frames(&t, 44, 300000);
    spi_tally_fold(&s, &t, 544, &out);

    CHECK(out.frames == 44, "frame delta");
    CHECK(out.irqs == 44, "irq delta");
    CHECK(out.late == 0, "serviced every IRQ — nothing late");
    CHECK(out.backlog == 0, "backlog stays clean");
    CHECK(out.avg_ns == 300000, "mean is over this window's frames");
    CHECK(out.headroom_ns == SPI_TALLY_PERIOD_NS - 300000, "headroom");
}

static void test_an_overrun_queues_rather_than_drops(void)
{
    spi_tally_t t;
    spi_tally_state_t s;
    spi_tally_sample_t out;
    spi_tally_reset(&t, &s);

    spi_tally_fold(&s, &t, 0, &out);          /* baseline */

    /* 44 IRQs fired, we only got through 41. ablspi's irq_arrived is a
     * counting semaphore, so those 3 are queued and will be replayed
     * back-to-back — the burst that reads as somebody else's producer
     * misbehaving. */
    run_frames(&t, 41, 300000);
    spi_tally_fold(&s, &t, 44, &out);
    CHECK(out.late == 3, "three IRQs arrived while busy");
    CHECK(out.backlog == 3, "backlog accumulates");
    CHECK(out.backlog_peak == 3, "peak tracks the worst window");

    run_frames(&t, 43, 300000);
    spi_tally_fold(&s, &t, 88, &out);
    CHECK(out.late == 1, "one more");
    CHECK(out.backlog == 4, "backlog is cumulative");
    CHECK(out.backlog_peak == 3, "peak keeps the WORSE earlier window");
}

static void test_draining_the_queue_is_not_a_negative_backlog(void)
{
    spi_tally_t t;
    spi_tally_state_t s;
    spi_tally_sample_t out;
    spi_tally_reset(&t, &s);

    spi_tally_fold(&s, &t, 0, &out);          /* baseline */
    run_frames(&t, 41, 300000);
    spi_tally_fold(&s, &t, 44, &out);         /* 3 queued */
    CHECK(out.backlog == 3, "precondition: 3 queued");

    /* Now we work off the backlog: 47 frames against 44 IRQs. `irqs - frames`
     * is negative here, and the counters are unsigned — done naively that is
     * ~2^32 late IRQs and a backlog that never recovers. */
    run_frames(&t, 47, 300000);
    spi_tally_fold(&s, &t, 88, &out);
    CHECK(out.late == 0, "draining the queue is not lateness");
    CHECK(out.backlog == 3, "backlog does not underflow");
    CHECK(out.frames == 47, "frames still counted correctly");
}

static void test_the_proc_counter_wrapping_is_not_a_spike(void)
{
    spi_tally_t t;
    spi_tally_state_t s;
    spi_tally_sample_t out;

    /* The irq_count file under /proc/ableton is printed from an `int` that only
     * ever increments. At Move's 344.5 Hz block rate it crosses 2^31 after ~72
     * days and prints negative from then on, then wraps 2^32 -> 0. Both
     * transitions must read as an ordinary window. */
    const uint32_t sign_flip = 0x7FFFFFFFu;   /* INT_MAX, next print is negative */
    spi_tally_reset(&t, &s);
    spi_tally_fold(&s, &t, sign_flip - 22u, &out);   /* baseline */
    run_frames(&t, 44, 300000);
    spi_tally_fold(&s, &t, sign_flip + 22u, &out);
    CHECK(out.irqs == 44, "crossing INT_MAX is a 44-irq window, not a spike");
    CHECK(out.late == 0, "…and produces no phantom backlog");

    const uint32_t wrap = 0xFFFFFFFFu;
    spi_tally_reset(&t, &s);
    spi_tally_fold(&s, &t, wrap - 22u, &out);        /* baseline */
    run_frames(&t, 44, 300000);
    spi_tally_fold(&s, &t, (uint32_t)(wrap + 22u), &out);
    CHECK(out.irqs == 44, "wrapping 2^32 is a 44-irq window, not a spike");
    CHECK(out.late == 0, "…and produces no phantom backlog");
}

static void test_headroom_floors_at_zero(void)
{
    spi_tally_t t;
    spi_tally_state_t s;
    spi_tally_sample_t out;
    spi_tally_reset(&t, &s);

    spi_tally_fold(&s, &t, 0, &out);          /* baseline */
    /* A transfer longer than the nominal period. Unsigned subtraction would
     * report several thousand microseconds of headroom at the exact moment
     * there is none. */
    run_frames(&t, 4, SPI_TALLY_PERIOD_NS + 500000u);
    spi_tally_fold(&s, &t, 4, &out);
    CHECK(out.headroom_ns == 0, "headroom cannot go below zero");
}

static void test_reset_drops_the_previous_session(void)
{
    spi_tally_t t;
    spi_tally_state_t s;
    spi_tally_sample_t out;
    spi_tally_reset(&t, &s);

    spi_tally_fold(&s, &t, 0, &out);
    run_frames(&t, 41, 1900000);
    spi_tally_fold(&s, &t, 44, &out);
    CHECK(out.backlog == 3 && out.max_ns == 1900000, "precondition");

    /* Disarming and re-arming must not inherit the old peak or backlog —
     * otherwise a fresh measurement opens by reporting an overrun that
     * happened before the user started looking. */
    spi_tally_reset(&t, &s);
    spi_tally_fold(&s, &t, 44, &out);
    CHECK(out.first == 1, "re-arming takes a fresh baseline");
    CHECK(out.backlog == 0, "backlog is dropped");
    CHECK(out.max_ns == 0, "peak is dropped");
}

static void test_format_never_overruns(void)
{
    spi_tally_sample_t out;
    memset(&out, 0, sizeof(out));
    out.frames = 18446744073709551615ull;
    out.irqs = 0xFFFFFFFFu;
    out.avg_ns = 0xFFFFFFFFu;
    out.max_ns = 0xFFFFFFFFu;
    out.headroom_ns = 0xFFFFFFFFu;
    out.late = 0xFFFFFFFFu;
    out.backlog = 18446744073709551615ull;
    out.backlog_peak = 18446744073709551615ull;

    char small[16];
    memset(small, 0x7F, sizeof(small));
    spi_tally_format(&out, small, (int)sizeof(small));
    CHECK(small[sizeof(small) - 1] == '\0', "format NUL-terminates within len");

    memset(small, 0x7F, sizeof(small));
    spi_tally_format_late(&out, small, (int)sizeof(small));
    CHECK(small[sizeof(small) - 1] == '\0', "format_late NUL-terminates within len");

    char big[256];
    out.first = 1;
    spi_tally_format(&out, big, (int)sizeof(big));
    CHECK(strstr(big, "baseline") != NULL, "the baseline line says so");
}

int main(void)
{
    test_a_zero_transfer_time_is_not_a_sample();
    test_max_is_the_worst_since_arming();
    test_the_first_fold_is_a_baseline();
    test_a_clean_window_has_no_backlog();
    test_an_overrun_queues_rather_than_drops();
    test_draining_the_queue_is_not_a_negative_backlog();
    test_the_proc_counter_wrapping_is_not_a_spike();
    test_headroom_floors_at_zero();
    test_reset_drops_the_previous_session();
    test_format_never_overruns();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS: spi tally — irq backlog survives the /proc counter's wrap, "
           "and draining is not a negative backlog\n");
    return 0;
}
