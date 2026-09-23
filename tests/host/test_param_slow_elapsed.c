/*
 * param-slow reported 4294967.295 ms for calls that took microseconds.
 *
 * The producer computed elapsed time as
 *     (b.tv_sec - a.tv_sec) * 1000000 + (b.tv_nsec - a.tv_nsec) / 1000
 * in UNSIGNED arithmetic. `tv_nsec` borrows -- whenever the nanosecond field
 * wrapped, the second term is negative, becomes ~1.8e19 unsigned, and the
 * clamped result is exactly 0xFFFFFFFF microseconds.
 *
 * The line that prints is "the module is doing blocking work in its entry
 * point", naming a key. It is ALWAYS ON and it is what this codebase reaches
 * for to attribute a stall, so a false accusation costs the session it exists
 * to save. Observed twice in one log on an idle device, against `synth:name`
 * and `lfo2:enabled`.
 *
 * The borrow case is the FIRST test here because it is the one that shipped.
 */
#include <stdio.h>
#include <string.h>
#include "param_slow.h"

static int fails;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static struct timespec ts(long sec, long nsec) {
    struct timespec t; t.tv_sec = sec; t.tv_nsec = nsec; return t;
}

int main(void) {
    /* THE BORROW. 999.9 ms into a second, ending 0.1 ms into the next: a real
     * elapsed time of 200 us. The old form made this 0xFFFFFFFF. */
    struct timespec a = ts(100, 999900000L);
    struct timespec b = ts(101, 100000L);
    uint32_t us = param_slow_elapsed_us(&a, &b);
    CHECK(us == 200, "a borrow across a second reported %u us, expected 200 "
                     "(the unsigned form reported 4294967295)", us);

    /* The ordinary case, no borrow. */
    a = ts(100, 1000000L); b = ts(100, 3500000L);
    us = param_slow_elapsed_us(&a, &b);
    CHECK(us == 2500, "a plain 2.5 ms span reported %u us", us);

    /* Whole seconds, which the seconds term alone must carry. */
    a = ts(100, 500000000L); b = ts(103, 500000000L);
    us = param_slow_elapsed_us(&a, &b);
    CHECK(us == 3000000, "a 3 s span reported %u us", us);

    /* A CLOCK THAT WENT BACKWARDS IS NOT A MEASUREMENT. Reporting a number
     * here is what the old code did, and somebody acts on it. */
    a = ts(100, 500000000L); b = ts(99, 500000000L);
    CHECK(param_slow_elapsed_us(&a, &b) == 0,
          "a backwards clock reported %u us instead of 0",
          param_slow_elapsed_us(&a, &b));

    /* Equal reads are zero, not a wrap. */
    a = ts(100, 12345L);
    CHECK(param_slow_elapsed_us(&a, &a) == 0, "an identical pair was not 0");

    /* Saturation stays inside the field rather than wrapping it. */
    a = ts(0, 0); b = ts(9000, 0);
    CHECK(param_slow_elapsed_us(&a, &b) == 0xFFFFFFFFu,
          "a span past the field did not clamp (%u)",
          param_slow_elapsed_us(&a, &b));

    /* And the threshold still gates: the producer drops anything under it, so
     * a sub-threshold span must not be recorded at all. */
    param_slow_t p; memset(&p, 0, sizeof(p));
    p.threshold_us = 1000;
    CHECK(param_slow_record(&p, "synth:name", 0, 0, 999) == 0,
          "a span under the threshold was recorded");
    CHECK(param_slow_record(&p, "synth:name", 0, 0, 1000) == 1,
          "a span at the threshold was dropped");

    /* The formatted line must carry the key -- the whole reason this exists. */
    param_slow_entry_t e;
    CHECK(param_slow_take(&p, &e) == 1, "nothing to take");
    char buf[256];
    param_slow_format(&e, buf, sizeof(buf));
    CHECK(strstr(buf, "synth:name") != NULL,
          "the line does not name the key: %s", buf);
    CHECK(strstr(buf, "1.000 ms") != NULL,
          "1000 us did not format as 1.000 ms: %s", buf);

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: param_slow elapsed time survives the nanosecond borrow\n");
    return 0;
}
