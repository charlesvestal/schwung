/*
 * mod_route_key.h -- the "mod<N>:" / legacy "lfo<N>:" parser, run natively.
 *
 * Its three siblings (bus_route.h, send_fx_key.h, master_fx_key.h) are tested
 * the same way and for the same reason: the caller lives in a translation unit
 * that needs the whole chain, so without this the parser only ever runs on the
 * device. master_fx_key.h exists because that is how an else-branch assigning
 * SLOT 0 shipped.
 *
 * The cap is passed in, so the test can also prove the parser does not carry a
 * copy of it -- a route count of 3 must reject "mod4:".
 */
#include <stdio.h>
#include <string.h>
#include "mod_route_key.h"

#define ROUTES 8

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", (m)); fails++; } \
                         else printf("  ok  %s\n", (m)); } while (0)

int main(void) {
    const char *rest = NULL;

    /* ---- both spellings reach the same storage ------------------------ */
    CHECK(mod_route_parse_key("mod1:depth", ROUTES, &rest) == 0
          && strcmp(rest, "depth") == 0, "mod1: is route 0, rest is the subkey");
    CHECK(mod_route_parse_key("lfo1:depth", ROUTES, &rest) == 0,
          "lfo1: is THE SAME route 0 -- the alias shares storage, never copies");
    CHECK(mod_route_parse_key("mod2:depth", ROUTES, &rest) == 1, "mod2: is route 1");
    CHECK(mod_route_parse_key("lfo2:depth", ROUTES, &rest) == 1,
          "lfo2: is the same route 1");
    CHECK(mod_route_parse_key("mod8:src", ROUTES, &rest) == 7
          && strcmp(rest, "src") == 0, "mod8: is route 7, the last one");
    CHECK(mod_route_parse_key("mod3:target_param", ROUTES, &rest) == 2
          && strcmp(rest, "target_param") == 0, "a subkey containing _ survives");

    /* ---- rejection, never a clamp and never route 0 ------------------- */
    CHECK(mod_route_parse_key("mod9:depth", ROUTES, &rest) == -1,
          "mod9: is REJECTED, not clamped onto route 7");
    CHECK(mod_route_parse_key("lfo3:depth", ROUTES, &rest) == -1,
          "lfo3: is rejected -- the legacy name froze at two");
    CHECK(mod_route_parse_key("mod0:depth", ROUTES, &rest) == -1, "mod0: is rejected");
    CHECK(mod_route_parse_key("mod01:depth", ROUTES, &rest) == -1,
          "a leading zero is rejected, as in bus_route.h");
    CHECK(mod_route_parse_key("mod1depth", ROUTES, &rest) == -1,
          "a missing colon is rejected");
    CHECK(mod_route_parse_key("mod:depth", ROUTES, &rest) == -1,
          "a missing index is rejected");
    CHECK(mod_route_parse_key("modulate:x", ROUTES, &rest) == -1,
          "a longer word beginning with mod is rejected");
    CHECK(mod_route_parse_key("lfo_config", ROUTES, &rest) == -1,
          "lfo_config, a real sibling key, is not mistaken for a route");
    CHECK(mod_route_parse_key("fx1:cutoff", ROUTES, &rest) == -1, "an fx key is not ours");
    CHECK(mod_route_parse_key("bus1:fx2:mix", ROUTES, &rest) == -1,
          "a bus key is not ours");
    CHECK(mod_route_parse_key("", ROUTES, &rest) == -1, "an empty key is rejected");
    CHECK(mod_route_parse_key(NULL, ROUTES, &rest) == -1, "NULL is rejected");
    CHECK(mod_route_parse_key("mod99999999999:x", ROUTES, &rest) == -1,
          "an absurd index cannot overflow into a plausible one");

    /* ---- the cap really is a parameter -------------------------------- */
    CHECK(mod_route_parse_key("mod4:depth", 3, &rest) == -1,
          "a route_count of 3 rejects mod4: -- the parser holds no copy of the cap");
    CHECK(mod_route_parse_key("mod3:depth", 3, &rest) == 2,
          "a route_count of 3 still accepts mod3:");
    CHECK(mod_route_parse_key("lfo2:depth", 1, &rest) == -1,
          "the legacy cap is clamped BY route_count too, never above it");
    CHECK(mod_route_parse_key("mod1:depth", 0, &rest) == -1,
          "a route_count of 0 accepts nothing");

    /* ---- a rejection leaves the caller's out-param alone --------------- */
    const char *sentinel = "untouched";
    rest = sentinel;
    (void)mod_route_parse_key("mod9:depth", ROUTES, &rest);
    CHECK(rest == sentinel, "a rejection leaves *out_rest untouched");
    rest = sentinel;
    (void)mod_route_parse_key("fx1:cutoff", ROUTES, &rest);
    CHECK(rest == sentinel, "a non-match leaves *out_rest untouched");

    /* ---- a NULL out_rest is allowed, for the does-this-match probe ----- */
    CHECK(mod_route_parse_key("mod5:depth", ROUTES, NULL) == 4,
          "out_rest may be NULL when the caller only wants the index");

    printf(fails ? "FAIL\n" : "ALL PASS\n");
    return fails ? 1 : 0;
}
