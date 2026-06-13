/*
 * Regression test for json_get_section_bounds null-valued-key handling.
 *
 * Bug (carried fix from PRs #115/#117): a null-valued key — e.g. an empty FX
 * slot "fx2":null — made the old code scan to the NEXT '{', grabbing a LATER
 * slot's object and corrupting saved presets (filled/empty/filled). The fix
 * anchors on the key's colon and only treats it as a section when the value is
 * actually an object. Also fixes a latent Master-preset bug.
 */
#include <stdio.h>
#include <string.h>

int json_get_section_bounds(const char *json, const char *section_key,
                            const char **out_start, const char **out_end);

int main(void) {
    /* fx2 is null, sandwiched between two real objects. */
    const char *json =
        "{ \"fx1\": { \"module\": \"a\" }, "
        "\"fx2\": null, "
        "\"fx3\": { \"module\": \"b\" } }";
    const char *s = NULL, *e = NULL;

    /* A null-valued key must report "no object section" (non-zero), NOT a
     * later slot's object. */
    if (json_get_section_bounds(json, "fx2", &s, &e) == 0) {
        fprintf(stderr,
            "FAIL: null-valued fx2 matched an object (grabbed a later slot)\n");
        return 1;
    }

    /* Real objects still resolve. */
    if (json_get_section_bounds(json, "fx1", &s, &e) != 0) {
        fprintf(stderr, "FAIL: fx1 object not found\n");
        return 1;
    }
    if (json_get_section_bounds(json, "fx3", &s, &e) != 0) {
        fprintf(stderr, "FAIL: fx3 object not found\n");
        return 1;
    }
    /* fx3's bounds must be its own small object (contains "b"), proving we
     * resolved fx3 and not some merged/earlier span. */
    if (!s || !e || e <= s || !strstr(s, "\"b\"") || (size_t)(e - s) > 40) {
        fprintf(stderr, "FAIL: fx3 bounds resolved incorrectly\n");
        return 1;
    }

    printf("PASS: null-valued FX slot is not matched to a later object\n");
    return 0;
}
