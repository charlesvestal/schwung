#include <stdio.h>
#include "editor_bar_announce.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

int main(void)
{
    printf("editor bar announcement parsing\n");

    /* What Move actually sends -- captured 2026-09-12 over D-Bus. */
    CHECK(editor_bar_parse("Bar 1") == 1, "Bar 1");
    CHECK(editor_bar_parse("Bar 4") == 4, "Bar 4");
    CHECK(editor_bar_parse("Bar 12") == 12, "Bar 12");
    CHECK(editor_bar_parse("bar 7") == 7, "lowercase");
    CHECK(editor_bar_parse("Bar\n3") == 3, "Move uses a newline in some strings");
    CHECK(editor_bar_parse("  Bar 5  ") == 5, "surrounding whitespace");

    /* This is a SHARED channel. Move announces track names, parameter values
     * and Schwung's own TTS through it; a loose match costs a wrong page,
     * which is indistinguishable from a phase error. */
    CHECK(editor_bar_parse("Bar Fight") == 0, "a clip named Bar Fight");
    CHECK(editor_bar_parse("Bar 4 of 8") == 0, "a different message entirely");
    CHECK(editor_bar_parse("Barcelona 3") == 0, "prefix must be the whole word");
    CHECK(editor_bar_parse("Set Bar 4") == 0, "no leading words");
    CHECK(editor_bar_parse("Bar") == 0, "no number");
    CHECK(editor_bar_parse("Bar 0") == 0, "bars are 1-based; 0 is not a page");
    CHECK(editor_bar_parse("Bar -2") == 0, "negative");
    CHECK(editor_bar_parse("Bar 99999") == 0, "absurd");
    CHECK(editor_bar_parse("") == 0, "empty");
    CHECK(editor_bar_parse(0) == 0, "null");

    /* A truncated announcement is a plausible string naming the WRONG page.
     * "Bar 12" cut short reads as "Bar 1", so the caller must never see a
     * partial parse -- but note the cut string is itself valid, which is why
     * the defence has to be at the transport, not here. This pins the
     * behaviour so the limitation is written down rather than assumed away. */
    CHECK(editor_bar_parse("Bar 1") == 1,
          "a truncated 'Bar 12' is indistinguishable from 'Bar 1' -- "
          "this parser CANNOT detect it");

    if (failures) { printf("\n%d CHECK(s) failed\n", failures); return 1; }
    printf("\nall editor_bar checks passed\n");
    return 0;
}
