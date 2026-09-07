/* Unit tests for param_slow.h — attribution for a param serve that ate the frame.
 *
 * Header-only and pure, so it runs natively on the dev machine. The producer's
 * real caller is the SPI callback, which is exactly the code path that cannot
 * be exercised here — so the ring, the threshold and the overwrite discipline
 * are pinned as arithmetic rather than observed on hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "param_slow.h"

static void test_threshold_is_honoured(void) {
    param_slow_t p;
    param_slow_init(&p, 1000);
    param_slow_entry_t e;

    /* A normal serve is ~10us. Two and a half orders of magnitude of clearance
     * is the whole reason this line can be trusted when it does fire. */
    assert(param_slow_record(&p, "synth:cutoff", 0, 1, 10) == 0);
    assert(param_slow_record(&p, "synth:cutoff", 0, 1, 999) == 0);
    assert(param_slow_take(&p, &e) == 0);      /* nothing recorded at all */

    /* Exactly at the threshold records: the constant names the smallest
     * duration we want to hear about, not the largest we tolerate. */
    assert(param_slow_record(&p, "synth:state", 1, 1, 1000) == 1);
    assert(param_slow_take(&p, &e) == 1);
    assert(e.us == 1000);
    printf("  threshold: ok\n");
}

static void test_the_key_is_what_gets_recorded(void) {
    param_slow_t p;
    param_slow_init(&p, 1000);
    param_slow_entry_t e;

    /* The entire point. `param=7/20051` is a number with no name attached;
     * twice now that has cost a full diagnosis session. */
    assert(param_slow_record(&p, "synth:state", 2, 1, 20051) == 1);
    assert(param_slow_take(&p, &e) == 1);
    assert(strcmp(e.key, "synth:state") == 0);
    assert(e.slot == 2);
    assert(e.is_set == 1);
    assert(e.us == 20051);
    printf("  key recorded: ok\n");
}

static void test_a_long_key_is_truncated_not_overflowed(void) {
    param_slow_t p;
    param_slow_init(&p, 1000);
    param_slow_entry_t e;
    char longkey[PARAM_SLOW_KEY_LEN * 2];
    memset(longkey, 'k', sizeof(longkey) - 1);
    longkey[sizeof(longkey) - 1] = '\0';

    assert(param_slow_record(&p, longkey, 0, 1, 5000) == 1);
    assert(param_slow_take(&p, &e) == 1);
    assert(strlen(e.key) == PARAM_SLOW_KEY_LEN - 1);

    /*
     * THE BOUNDARY, not just "something long".
     *
     * A key far over the limit clamps identically whether the guard is `>=` or
     * `>`, so a 127-char key cannot tell the two apart — and `>` is a one-byte
     * overrun of key[] at exactly PARAM_SLOW_KEY_LEN. Found by mutation: the
     * long-key case above passed the mutant. Both neighbours are pinned so the
     * clamp cannot drift in either direction.
     */
    char exact[PARAM_SLOW_KEY_LEN + 1];
    memset(exact, 'k', PARAM_SLOW_KEY_LEN);
    exact[PARAM_SLOW_KEY_LEN] = '\0';
    assert(strlen(exact) == PARAM_SLOW_KEY_LEN);
    assert(param_slow_record(&p, exact, 0, 1, 5000) == 1);
    assert(param_slow_take(&p, &e) == 1);
    assert(strlen(e.key) == PARAM_SLOW_KEY_LEN - 1);   /* truncated by one */

    char fits[PARAM_SLOW_KEY_LEN];
    memset(fits, 'k', PARAM_SLOW_KEY_LEN - 1);
    fits[PARAM_SLOW_KEY_LEN - 1] = '\0';
    assert(param_slow_record(&p, fits, 0, 1, 5000) == 1);
    assert(param_slow_take(&p, &e) == 1);
    assert(strcmp(e.key, fits) == 0);                  /* NOT truncated */
    printf("  long key truncated (incl. the exact boundary): ok\n");
}

static void test_a_missing_key_still_reports(void) {
    param_slow_t p;
    param_slow_init(&p, 1000);
    param_slow_entry_t e;

    /* A blown frame we cannot name is still a blown frame. Dropping it would
     * make the rate read lower than it is, which is worse than "?". */
    assert(param_slow_record(&p, NULL, 0, 0, 9000) == 1);
    assert(param_slow_take(&p, &e) == 1);
    assert(strcmp(e.key, "?") == 0);

    assert(param_slow_record(&p, "", 0, 0, 9000) == 1);
    assert(param_slow_take(&p, &e) == 1);
    assert(strcmp(e.key, "?") == 0);
    printf("  unnamed serve still reports: ok\n");
}

static void test_ring_holds_several_offenders(void) {
    param_slow_t p;
    param_slow_init(&p, 1000);
    param_slow_entry_t e;

    /* A recall walks several components and only ONE is the culprit. Keeping a
     * single slot would report "the worst" — the wrong summary when the
     * question is which keys were slow. */
    assert(param_slow_record(&p, "0:synth:state", 0, 1, 20051) == 1);
    assert(param_slow_record(&p, "1:synth:state", 1, 1, 1200) == 1);

    assert(param_slow_take(&p, &e) == 1);
    assert(strcmp(e.key, "0:synth:state") == 0);   /* FIFO, not worst-first */
    assert(param_slow_take(&p, &e) == 1);
    assert(strcmp(e.key, "1:synth:state") == 0);
    assert(param_slow_take(&p, &e) == 0);
    printf("  ring keeps several: ok\n");
}

static void test_overflow_drops_oldest_and_counts_it(void) {
    param_slow_t p;
    param_slow_init(&p, 1000);
    param_slow_entry_t e;

    /* The producer is the SPI callback: it may not block waiting for a
     * consumer, so loss is by construction. What must NOT happen is silent
     * loss — `dropped` non-zero is itself the finding that something is slow
     * faster than 1 Hz. */
    char key[16];
    for (int i = 0; i < PARAM_SLOW_ENTRIES + 2; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        assert(param_slow_record(&p, key, 0, 1, 2000) == 1);
    }
    assert(param_slow_take_dropped(&p) == 2);
    assert(param_slow_take_dropped(&p) == 0);      /* cleared by the read */

    /* The SURVIVORS are the newest, so a burst names what is happening now
     * rather than what happened first. */
    assert(param_slow_take(&p, &e) == 1);
    assert(strcmp(e.key, "k2") == 0);
    printf("  overflow drops oldest, counts it: ok\n");
}

static void test_drain_is_complete(void) {
    param_slow_t p;
    param_slow_init(&p, 1000);
    param_slow_entry_t e;

    assert(param_slow_record(&p, "a", 0, 1, 2000) == 1);
    assert(param_slow_take(&p, &e) == 1);
    assert(param_slow_take(&p, &e) == 0);
    /* and the ring is reusable after draining, rather than wedged */
    assert(param_slow_record(&p, "b", 0, 1, 2000) == 1);
    assert(param_slow_take(&p, &e) == 1);
    assert(strcmp(e.key, "b") == 0);
    printf("  drain and reuse: ok\n");
}

static void test_format_names_the_key_and_the_thread(void) {
    param_slow_entry_t e = { "synth:state", 20051, 2, 1 };
    char buf[256];
    int n = param_slow_format(&e, buf, sizeof(buf));
    assert(n > 0);

    /* A log line nobody can grep for is the failure this header exists to
     * prevent, so the greppable parts are pinned, not just the length. */
    assert(strstr(buf, "param-slow:") != NULL);
    assert(strstr(buf, "synth:state") != NULL);
    assert(strstr(buf, "slot 2") != NULL);
    assert(strstr(buf, "set") != NULL);
    assert(strstr(buf, "20.051 ms") != NULL);   /* not 20051us, not 20ms */
    assert(strstr(buf, "SPI callback") != NULL);

    e.is_set = 0;
    assert(param_slow_format(&e, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "get") != NULL);
    printf("  format: ok\n");
}

static void test_format_is_bounded(void) {
    param_slow_entry_t e = { "synth:state", 20051, 2, 1 };
    char buf[16];
    int n = param_slow_format(&e, buf, sizeof(buf));
    /* snprintf's return is what it WOULD have written; the buffer must still
     * be terminated inside its own bounds. */
    assert(n > 0);
    assert(strlen(buf) < sizeof(buf));
    printf("  format bounded: ok\n");
}

int main(void) {
    printf("param_slow:\n");
    test_threshold_is_honoured();
    test_the_key_is_what_gets_recorded();
    test_a_long_key_is_truncated_not_overflowed();
    test_a_missing_key_still_reports();
    test_ring_holds_several_offenders();
    test_overflow_drops_oldest_and_counts_it();
    test_drain_is_complete();
    test_format_names_the_key_and_the_thread();
    test_format_is_bounded();
    printf("param_slow: all ok\n");
    return 0;
}
