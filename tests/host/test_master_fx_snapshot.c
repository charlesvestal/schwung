/*
 * master_fx_snapshot.h — the one-read `master_fx:modules` answer.
 *
 * What is worth pinning here is not "does it produce JSON". It is the three
 * properties the READER depends on and cannot check for itself:
 *
 *   1. The array is POSITIONAL. An unloaded position is an empty entry, never
 *      an omitted one — a compacted array would move position 3's module onto
 *      position 1 the moment anything ahead of it was empty, which is a wrong
 *      module loaded at boot rather than a missing one.
 *   2. It never truncates. A short array parses perfectly and reads as "those
 *      positions are empty", which is the erase this path exists to prevent, so
 *      an overflow must produce NO answer rather than a plausible one.
 *   3. A quote or backslash in a path cannot break the whole array. It is an
 *      absurd filename, but the blast radius is not one odd row: the reader
 *      treats unparseable exactly as it treats a failed read, so the entire
 *      chain would stop persisting.
 *
 * Built natively because its caller (shadow_chain_mgmt.c) is a shim translation
 * unit that cannot be compiled on the dev machine — which is how string
 * building like this ends up shipped untested.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "master_fx_snapshot.h"

static int failures = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  ok:   %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want) == 0) {
        printf("  ok:   %s\n", what);
    } else {
        printf("  FAIL: %s\n        got:  %s\n        want: %s\n", what, got, want);
        failures++;
    }
}

/* Build a whole array the way the call site does. Returns 1 on success. */
static int build(char *buf, size_t cap, size_t *len,
                 const char *const *ids, const char *const *paths, int n)
{
    int ok = master_fx_snapshot_begin(buf, cap, len);
    for (int i = 0; ok && i < n; i++) {
        ok = master_fx_snapshot_append(buf, cap, len, i, ids[i], paths[i]);
    }
    if (ok) ok = master_fx_snapshot_end(buf, cap, len);
    return ok;
}

int main(void)
{
    char buf[4096];
    size_t len = 0;

    printf("== empty chain ==\n");
    {
        const char *ids[]   = { "", "", "" };
        const char *paths[] = { "", "", "" };
        check(build(buf, sizeof(buf), &len, ids, paths, 3), "builds");
        check_str(buf,
                  "[{\"id\":\"\",\"path\":\"\"},"
                  "{\"id\":\"\",\"path\":\"\"},"
                  "{\"id\":\"\",\"path\":\"\"}]",
                  "every position present and empty");
        check(len == strlen(buf), "reported length matches the string");
    }

    printf("== a hole in the middle stays a hole ==\n");
    {
        /* This is property 1. If the empty position were omitted, the reader
         * would index "verb" at position 1 — the position the user cleared. */
        const char *ids[]   = { "clap", "", "verb" };
        const char *paths[] = { "/a/clap/dsp.so", "", "/a/verb/dsp.so" };
        check(build(buf, sizeof(buf), &len, ids, paths, 3), "builds");
        check_str(buf,
                  "[{\"id\":\"clap\",\"path\":\"/a/clap/dsp.so\"},"
                  "{\"id\":\"\",\"path\":\"\"},"
                  "{\"id\":\"verb\",\"path\":\"/a/verb/dsp.so\"}]",
                  "position 1 is an empty entry, not an omission");
    }

    printf("== quotes and backslashes cannot end the string early ==\n");
    {
        const char *ids[]   = { "od\"d" };
        const char *paths[] = { "/a/b\\c/dsp.so" };
        check(build(buf, sizeof(buf), &len, ids, paths, 1), "builds");
        check_str(buf,
                  "[{\"id\":\"od\\\"d\",\"path\":\"/a/b\\\\c/dsp.so\"}]",
                  "both are escaped");
    }

    printf("== control characters are dropped, not emitted raw ==\n");
    {
        const char *ids[]   = { "a\nb" };
        const char *paths[] = { "" };
        check(build(buf, sizeof(buf), &len, ids, paths, 1), "builds");
        check(strchr(buf, '\n') == NULL, "no raw newline in the answer");
        check_str(buf, "[{\"id\":\"ab\",\"path\":\"\"}]", "dropped in place");
    }

    printf("== overflow REFUSES, it does not truncate ==\n");
    {
        /* Property 2, and the reason the append is all-or-nothing. A buffer
         * one byte short of the second entry must not leave the first entry
         * behind with a valid-looking close bracket. */
        const char *ids[]   = { "aaaa", "bbbb" };
        const char *paths[] = { "/x/aaaa/dsp.so", "/x/bbbb/dsp.so" };
        size_t exact = 0;
        char big[4096];
        (void)build(big, sizeof(big), &exact, ids, paths, 2);

        int all_refused = 1;
        for (size_t cap = 1; cap < exact + 1; cap++) {
            char small[4096];
            size_t got = 0;
            memset(small, 0x7f, sizeof(small));
            int ok = build(small, cap, &got, ids, paths, 2);
            if (ok) {
                printf("  FAIL: cap %zu claimed success for a %zu-byte answer\n",
                       cap, exact);
                failures++;
                all_refused = 0;
                break;
            }
            /* Whatever it left behind must be NUL-terminated inside the cap and
             * must not be a complete array — a caller that ignored the return
             * and shipped the buffer would otherwise ship a SHORTER CHAIN. */
            if (memchr(small, '\0', cap) == NULL) {
                printf("  FAIL: cap %zu left an unterminated buffer\n", cap);
                failures++;
                all_refused = 0;
                break;
            }
            size_t l = strlen(small);
            if (l > 0 && small[l - 1] == ']') {
                printf("  FAIL: cap %zu left a parseable partial array: %s\n",
                       cap, small);
                failures++;
                all_refused = 0;
                break;
            }
        }
        check(all_refused, "every cap below the exact size refuses");

        size_t ok_len = 0;
        check(build(buf, exact + 1, &ok_len, ids, paths, 2),
              "the exact size + NUL succeeds");
        check(ok_len == exact, "and reports the same length");
    }

    printf("== begin() owns the buffer ==\n");
    {
        size_t stale = 999;
        strcpy(buf, "leftover");
        check(master_fx_snapshot_begin(buf, sizeof(buf), &stale), "begins");
        check(stale == 1 && strcmp(buf, "[") == 0,
              "a stale length from a previous answer is reset, not appended to");
    }

    if (failures) {
        printf("\n%d master_fx_snapshot check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall master_fx_snapshot tests passed\n");
    return 0;
}
