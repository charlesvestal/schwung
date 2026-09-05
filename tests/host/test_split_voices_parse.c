#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "split_voices_parse.h"

static void test_flat_order_is_the_buffer_index(void) {
    char ids[8][32];
    int n = split_voices_parse(
        "[{\"id\":\"kick\",\"label\":\"Kick\"},"
        "{\"id\":\"snare\",\"label\":\"Snare\"},"
        "{\"id\":\"chh\",\"label\":\"Closed Hat\"}]",
        ids, 8, 32);
    assert(n == 3);
    assert(strcmp(ids[0], "kick") == 0);
    assert(strcmp(ids[1], "snare") == 0);
    assert(strcmp(ids[2], "chh") == 0);
    printf("  flat order: ok\n");
}

static void test_absent_and_failed_are_not_the_same(void) {
    char ids[8][32];
    /* "" — the channel served us, the key produced nothing. */
    assert(split_voices_parse("", ids, 8, 32) == 0);
    /* "[]" — the module says it has no splittable voices. */
    assert(split_voices_parse("[]", ids, 8, 32) == 0);
    /* NULL — the read did not complete. A distinct return, so the caller can
     * retry instead of concluding the module cannot split. */
    assert(split_voices_parse(NULL, ids, 8, 32) == SPLIT_VOICES_READ_FAILED);
    printf("  tri-state: ok\n");
}

static void test_overlong_id_is_rejected_not_truncated(void) {
    char ids[8][8];   /* deliberately tiny */
    int n = split_voices_parse("[{\"id\":\"kick\"},{\"id\":\"a_very_long_voice_id\"}]",
                               ids, 8, 8);
    assert(n == 1);                       /* the long one is dropped */
    assert(strcmp(ids[0], "kick") == 0);
    printf("  overlong rejected: ok\n");
}

static void test_bounded(void) {
    char big[4096] = "[";
    for (int i = 0; i < 40; i++) {
        char one[64];
        snprintf(one, sizeof(one), "%s{\"id\":\"v%d\"}", i ? "," : "", i);
        strcat(big, one);
    }
    strcat(big, "]");
    char ids[8][32];
    int n = split_voices_parse(big, ids, 8, 32);
    assert(n == 8);                       /* capped at max_ids, no overrun */
    printf("  bounded: ok\n");
}

int main(void) {
    printf("test_split_voices_parse:\n");
    test_flat_order_is_the_buffer_index();
    test_absent_and_failed_are_not_the_same();
    test_overlong_id_is_rejected_not_truncated();
    test_bounded();
    printf("PASS\n");
    return 0;
}
