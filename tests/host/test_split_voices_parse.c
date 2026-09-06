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
    /* The rejected entry is a HOLE at its own index, not a compaction: if it
     * shrank n and shifted "kick" to stay at [0], the *next* real voice would
     * silently inherit index 1 -- exactly the wrong-bus-wrong-drum bug this
     * table exists to prevent. */
    assert(n == 2);
    assert(strcmp(ids[0], "kick") == 0);
    assert(ids[1][0] == '\0');
    printf("  overlong rejected: ok\n");
}

static void test_index_integrity_not_compacted(void) {
    /* The motivating case from review: one rejected id must not re-point
     * every voice behind it. "kick" is voice 1 in the module's list and must
     * stay voice 1 in the table -- never shift to 0. */
    char ids[8][8];   /* id_len 8: "a_very_long_voice_id" does not fit */
    int n = split_voices_parse(
        "[{\"id\":\"a_very_long_voice_id\"},{\"id\":\"kick\"},{\"id\":\"snare\"}]",
        ids, 8, 8);
    assert(n == 3);
    assert(ids[0][0] == '\0');
    assert(strcmp(ids[1], "kick") == 0);
    assert(strcmp(ids[2], "snare") == 0);
    printf("  index integrity: ok\n");
}

static void test_empty_id_is_a_hole(void) {
    /* Fixture FIX 1 needs: an empty id is a hole at its own index too. */
    char ids[8][32];
    int n = split_voices_parse(
        "[{\"id\":\"kick\"},{\"id\":\"\"},{\"id\":\"snare\"}]",
        ids, 8, 32);
    assert(n == 3);
    assert(strcmp(ids[0], "kick") == 0);
    assert(ids[1][0] == '\0');
    assert(strcmp(ids[2], "snare") == 0);
    printf("  empty id is a hole: ok\n");
}

static void test_unterminated_string(void) {
    char ids[8][32];
    int n = split_voices_parse("[{\"id\":\"kick", ids, 8, 32);
    assert(n == 0);
    printf("  unterminated string: ok\n");
}

static void test_non_string_id_value(void) {
    char ids[8][32];
    int n = split_voices_parse("[{\"id\":123},{\"id\":\"snare\"}]", ids, 8, 32);
    assert(n == 1);
    assert(strcmp(ids[0], "snare") == 0);
    printf("  non-string id value: ok\n");
}

static void test_nested_id_is_a_documented_limitation(void) {
    /* The scan is flat: it cannot tell a nested "id" from a sibling one, so
     * this yields THREE voices, not two. Documented in the header, not a
     * bug fix -- this pins the current (surprising) behaviour. */
    char ids[8][32];
    int n = split_voices_parse(
        "[{\"id\":\"kick\",\"meta\":{\"id\":\"inner\"}},{\"id\":\"snare\"}]",
        ids, 8, 32);
    assert(n == 3);
    assert(strcmp(ids[0], "kick") == 0);
    assert(strcmp(ids[1], "inner") == 0);
    assert(strcmp(ids[2], "snare") == 0);
    printf("  nested id (documented limitation): ok\n");
}

static void test_whitespace_before_colon(void) {
    /* A module pretty-printing its JSON is a realistic producer. */
    char ids[8][32];
    int n = split_voices_parse("[{\"id\"\t:\n\"kick\"}]", ids, 8, 32);
    assert(n == 1);
    assert(strcmp(ids[0], "kick") == 0);
    printf("  whitespace before colon: ok\n");
}

static void test_advance_past_closing_quote(void) {
    /* Guards `p = end + 1` (must skip PAST the closing quote, not land on
     * it). Constructed so the value's closing quote is immediately
     * followed by a bare `id":...` with no quote of its own before "id" --
     * if the scan resumed AT the closing quote instead of after it, that
     * quote would double as the opening quote of a spurious "id" token and
     * a phantom second voice ("snare") would appear. */
    char ids[8][32];
    int n = split_voices_parse("[{\"id\":\"kick\"id\":\"snare\"}]", ids, 8, 32);
    assert(n == 1);
    assert(strcmp(ids[0], "kick") == 0);
    printf("  advance past closing quote: ok\n");
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
    test_index_integrity_not_compacted();
    test_empty_id_is_a_hole();
    test_unterminated_string();
    test_non_string_id_value();
    test_nested_id_is_a_documented_limitation();
    test_whitespace_before_colon();
    test_advance_past_closing_quote();
    test_bounded();
    printf("PASS\n");
    return 0;
}
