#include <stdio.h>
#include <string.h>

#include "master_fx_saved_state.h"

static int failures;

static void expect_state(const char *label, const char *json, const char *want)
{
    char out[512] = "unchanged";
    int got = master_fx_saved_state_copy(json, out, sizeof(out));
    if (got != (int)strlen(want) || strcmp(out, want) != 0) {
        fprintf(stderr, "FAIL %s: got %d [%s], want %zu [%s]\n",
                label, got, out, strlen(want), want);
        failures++;
    }
}

static void expect_rejected(const char *label, const char *json)
{
    char out[16];
    if (master_fx_saved_state_copy(json, out, sizeof(out)) >= 0) {
        fprintf(stderr, "FAIL %s: malformed/absent state was accepted as [%s]\n", label, out);
        failures++;
    }
}

int main(void)
{
    const char *palette =
        "2,0.2500,0.5000,0.1000,5,0.7500,0.2000,0.0000,"
        "0,0.0000,0.0000,0.0000,11,1.0000,0.9000,0.3000,"
        "0.4200,1.2500,7,3,0.1500,1,96,4";
    char palette_json[768];
    snprintf(palette_json, sizeof(palette_json),
             "{\"module_id\":\"palette\",\"state\":\"%s\"}", palette);
    expect_state("PALETTE opaque CSV", palette_json, palette);

    expect_state("structured object",
                 "{\"state\":{\"name\":\"brace } in string\",\"nested\":{\"x\":1}}}",
                 "{\"name\":\"brace } in string\",\"nested\":{\"x\":1}}");
    expect_state("escaped opaque string",
                 "{\"state\":\"line1\\nline2\\t\\\"quoted\\\"\\\\tail\"}",
                 "line1\nline2\t\"quoted\"\\tail");

    expect_rejected("no state", "{\"module_id\":\"palette\"}");
    expect_rejected("unterminated string", "{\"state\":\"oops}");
    expect_rejected("unsupported state type", "{\"state\":42}");
    {
        char tiny[4];
        if (master_fx_saved_state_copy(palette_json, tiny, sizeof(tiny)) >= 0) {
            fprintf(stderr, "FAIL overflow: truncated state was accepted\n");
            failures++;
        }
    }

    if (failures) return 1;
    puts("PASS: Master FX boot state accepts structured and opaque values");
    return 0;
}
