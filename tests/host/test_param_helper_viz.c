/*
 * Tests for param_helper.h's viz emission — the `viz` field of
 * docs/MODULES.md, "Parameter visualisations".
 *
 * A module declares a graphic by filling in viz_group/viz_role/viz_kind beside
 * the param itself. What is being pinned here is mostly that the JSON comes out
 * the shape the host's resolver expects (src/shared/param_pages/viz.mjs reads
 * `group`/`role`/`kind`, and treats `viz: false` as a suppression), because
 * nothing else in the build would notice if it drifted: a malformed viz field
 * does not fail to compile, it just silently stops drawing a picture.
 */
#include <stdio.h>
#include <string.h>
#include "param_helper.h"

static int failures = 0;

static void expect_str(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s\n  got:  %s\n  want: %s\n", what, got, want);
        failures++;
    }
}

static void expect_int(const char *what, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

enum { IDX_A, IDX_B, IDX_C, IDX_D };

int main(void) {
    char buf[512];

    /* A group member: group + role, kind left for the host to derive. */
    {
        param_def_t d = {"attack", "Attack", PARAM_TYPE_FLOAT, IDX_A, 0.0f, 1.0f,
                         "amp", "attack", NULL};
        int n = param_helper_viz_json(&d, buf, sizeof(buf));
        expect_str("group+role", buf, ",\"viz\":{\"group\":\"amp\",\"role\":\"attack\"}");
        expect_int("group+role length", n, (int)strlen(buf));
    }

    /* A single-param graphic: kind alone, no group. */
    {
        param_def_t d = {"volume", "Volume", PARAM_TYPE_FLOAT, IDX_B, 0.0f, 1.0f,
                         NULL, NULL, "fader"};
        param_helper_viz_json(&d, buf, sizeof(buf));
        expect_str("kind only", buf, ",\"viz\":{\"kind\":\"fader\"}");
    }

    /* Group with an explicit kind — both appear, group first. */
    {
        param_def_t d = {"lfo_rate", "Rate", PARAM_TYPE_FLOAT, IDX_C, 0.0f, 1.0f,
                         "osc1_lfo", "rate", "lfo"};
        param_helper_viz_json(&d, buf, sizeof(buf));
        expect_str("group+role+kind", buf,
                   ",\"viz\":{\"group\":\"osc1_lfo\",\"role\":\"rate\",\"kind\":\"lfo\"}");
    }

    /* PARAM_VIZ_NONE is `viz: false` — stop a detector guessing, never an
     * object with kind "false". */
    {
        param_def_t d = {"low_xo", "Low XO", PARAM_TYPE_INT, IDX_D, 0.0f, 400.0f,
                         NULL, NULL, PARAM_VIZ_NONE};
        param_helper_viz_json(&d, buf, sizeof(buf));
        expect_str("viz false", buf, ",\"viz\":false");
    }

    /* Undeclared: writes nothing at all, so the param falls through to the
     * host's detectors and then to a plain dial. Most params are this. */
    {
        param_def_t d = {"timbre", "Timbre", PARAM_TYPE_FLOAT, IDX_A, 0.0f, 1.0f,
                         NULL, NULL, NULL};
        buf[0] = 'x'; buf[1] = '\0';
        int n = param_helper_viz_json(&d, buf, sizeof(buf));
        expect_int("undeclared writes nothing", n, 0);
        expect_str("undeclared leaves buffer alone", buf, "x");
    }

    /* Too small a buffer reports -1 rather than emitting half an object. */
    {
        param_def_t d = {"attack", "Attack", PARAM_TYPE_FLOAT, IDX_A, 0.0f, 1.0f,
                         "amp", "attack", NULL};
        char tiny[12];
        expect_int("tiny buffer", param_helper_viz_json(&d, tiny, sizeof(tiny)), -1);
    }

    /* The generator threads viz through, and a param that declares none adds
     * no field. */
    {
        static const param_def_t defs[] = {
            {"attack", "Attack", PARAM_TYPE_FLOAT, IDX_A, 0.0f, 1.0f, "amp", "attack", NULL},
            {"timbre", "Timbre", PARAM_TYPE_FLOAT, IDX_B, 0.0f, 1.0f, NULL, NULL, NULL},
        };
        int n = param_helper_chain_params_json(defs, PARAM_DEF_COUNT(defs), buf, sizeof(buf));
        expect_str("chain_params", buf,
            "[{\"key\":\"attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0,\"max\":1,"
            "\"viz\":{\"group\":\"amp\",\"role\":\"attack\"}},"
            "{\"key\":\"timbre\",\"name\":\"Timbre\",\"type\":\"float\",\"min\":0,\"max\":1}]");
        expect_int("chain_params length", n, (int)strlen(buf));
    }

    /*
     * A buffer too small for every param must fail, not return a short list.
     * Silently dropping params reaches the host as a module that simply has
     * fewer of them — the exact failure the viz migration guide warns about.
     */
    {
        static const param_def_t defs[] = {
            {"attack",  "Attack",  PARAM_TYPE_FLOAT, IDX_A, 0.0f, 1.0f, "amp", "attack",  NULL},
            {"release", "Release", PARAM_TYPE_FLOAT, IDX_B, 0.0f, 1.0f, "amp", "release", NULL},
        };
        char small[64];
        expect_int("truncation is an error",
                   param_helper_chain_params_json(defs, PARAM_DEF_COUNT(defs), small, sizeof(small)), -1);
    }

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS test_param_helper_viz\n");
    return 0;
}
