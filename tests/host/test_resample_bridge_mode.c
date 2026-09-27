/* The resample_bridge_mode parser.
 *
 * Run with no arguments it self-checks the table below. Run with one argument
 * it prints the parsed mode for that string and exits — which is how
 * test_resample_bridge_mode.sh drives this binary and shadow_ui.js's
 * parseResampleBridgeMode over the same inputs and requires them to agree.
 *
 * The agreement is the point. Two readers of one config key disagreed about
 * the legacy value `1`, and the C one runs first (shim init), so a device
 * carrying a Feb-2026 config came up in the retired additive mode until
 * shadow_ui loaded its config and overwrote it. Seconds in practice, because
 * nobody runs Schwung without the shadow UI — but a retired code path was live
 * for that window, and nothing anywhere said the two parsers had to match.
 */
#include <stdio.h>
#include <string.h>
#include "resample_bridge_mode.h"

static int failures = 0;

static void expect(const char *in, int want, const char *what)
{
    int got = (int)resample_bridge_mode_from_text_pure(in);
    if (got != want) {
        printf("FAIL: %s: parse(\"%s\") = %d, want %d\n",
               what, in ? in : "(null)", got, want);
        failures++;
    }
}

int main(int argc, char **argv)
{
    /* Driven by the shell test: print and exit, so both languages can be run
     * over one table without either restating it. */
    if (argc == 2) {
        printf("%d\n", (int)resample_bridge_mode_from_text_pure(argv[1]));
        return 0;
    }

    expect("0",         0, "numeric off");
    expect("off",       0, "word off");
    expect("Off",       0, "case");
    expect("2",         2, "numeric overwrite");
    expect("overwrite", 2, "word overwrite");
    expect("replace",   2, "the Feb 2026 spelling");
    expect("REPLACE",   2, "case");
    expect(" 2 ",       2, "padded, as a JSON token can arrive");

    /* THE LEGACY VALUE. shadow_ui.js migrates it to 2 and always has; this
     * parser returned the retired mode 1 instead, and runs first. */
    expect("1",   2, "legacy numeric 1 migrates to overwrite");
    expect("mix", 2, "legacy word mix migrates to overwrite");
    expect("Mix", 2, "legacy word, case");

    /* Mode 1 must never be produced. It is retired, and its value is left as a
     * hole in the enum because it is on disk in old configs — renumbering
     * would silently reinterpret them. */
    const char *every[] = {"0","1","2","3","off","mix","overwrite","replace",
                           "","native","schwung mix",NULL};
    for (int i = 0; every[i]; i++) {
        int got = (int)resample_bridge_mode_from_text_pure(every[i]);
        if (got == 1) {
            printf("FAIL: parse(\"%s\") produced the retired mode 1\n", every[i]);
            failures++;
        }
    }

    expect("",        0, "empty");
    expect(NULL,      0, "null");
    expect("garbage", 0, "unknown falls back to off, never to a live mode");

    /* "Native" is the UI's LABEL for mode 0, but it is not a wire value and
     * must not be parsed as one — the config stores numbers. It falling
     * through to off is correct, and stated so a future reader does not
     * "fix" it into a match. */
    expect("native", 0, "UI label is not a wire value");

    if (failures == 0) {
        printf("test_resample_bridge_mode: PASS\n");
        return 0;
    }
    printf("\n%d failure(s)\n", failures);
    return 1;
}
