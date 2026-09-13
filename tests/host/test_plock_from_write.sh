#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A COMPONENT WRITE MADE WHILE A STEP IS HELD IS A P-LOCK, and the decision
# lives in the shim rather than in the UI.
#
# Its UI half used to be `onValueWritten` in the host's param-pages io, armed
# only while VIEWS.PARAM_PAGES was up -- so a module drawing its own screen
# from ui_chain.js could not p-lock at all. 9W9 is one, and RECORDING worked
# there the whole time (lane_on_set_param intercepts every component write,
# whatever UI made it), which is what made it look like a module bug.
#
# What this pins is the part that is easy to get subtly wrong: WHICH keys are
# a component's parameters. `lanes:`, `slot:` and `buses:` all have a colon
# and are not parameters of anything -- treating one as a target would file a
# breakpoint under a lane named `lanes`.

fail() { echo "FAIL: $1"; exit 1; }
src=src/host/shadow_chain_mgmt.c
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# Lift the predicate rather than restating it, so a change to the real one is
# what this runs.
awk '/^static int shadow_component_param_split/,/^}/' "$src" > "$tmp/split.inc"
[ -s "$tmp/split.inc" ] || fail "could not lift shadow_component_param_split out of $src"

cat > "$tmp/t.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "split.inc"
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                       printf("\n"); fails++; } } while (0)
int main(void) {
    CHECK(shadow_component_param_split("synth:cutoff") == 5, "synth: must split at 5");
    CHECK(shadow_component_param_split("fx3:mix") == 3, "fx3: must split at 3");
    CHECK(shadow_component_param_split("fx12:mix") == 4, "a two-digit fx index must split");
    CHECK(shadow_component_param_split("midi_fx1:rate") == 8, "midi_fx1: must split at 8");

    /* The ones that share the shape and are NOT parameters of a component. */
    CHECK(shadow_component_param_split("lanes:plock") == 0,
          "lanes: is not a component -- filing under it would create a lane "
          "whose target is the word lanes");
    CHECK(shadow_component_param_split("lanes:state") == 0, "lanes:state is not a param");
    CHECK(shadow_component_param_split("slot:volume") == 0, "slot: is not a component");
    CHECK(shadow_component_param_split("buses:config") == 0, "buses: is not a component");
    CHECK(shadow_component_param_split("master_fx:fx1:mix") == 0,
          "master_fx has no lanes -- it is not a chain slot component");

    /* Shapes that must not be mistaken for an indexed component. */
    CHECK(shadow_component_param_split("fx:mix") == 0, "fx with no index is not a component");
    CHECK(shadow_component_param_split("fxa:mix") == 0, "fx must be followed by digits");
    CHECK(shadow_component_param_split("synthetic:x") == 0,
          "a longer word starting with synth is not synth");
    CHECK(shadow_component_param_split("nocolon") == 0, "no colon, no split");
    CHECK(shadow_component_param_split(":leading") == 0, "a leading colon names no target");
    CHECK(shadow_component_param_split(NULL) == 0, "NULL must not split");

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: plock-from-write key split\n");
    return 0;
}
EOF
cc -O1 -Wall -Wextra -Wno-unused-parameter -I "$tmp" -o "$tmp/t" "$tmp/t.c" || fail "compile"
"$tmp/t"

# ...and BOTH param paths must do it, or the gesture works from one UI and not
# another -- which is the bug this replaced, in a new place.
n=$(grep -c 'shadow_lanes_plock_from_write(slot,' "$src" || true)
[ "$n" = "2" ] || fail "expected the write-time p-lock on BOTH param paths (direct + SHM), found $n"

# The guards live in the shim, together, so neither call site can forget one.
#
# Matched WITHOUT a storage class. The first version of this pinned
# `static int shim_plock_held_step`, and removing that `static` is precisely
# the fix for the crash loop below -- so the pin broke on the change it should
# have been indifferent to.
body=$(awk '/^(static )?int shim_plock_held_step\(void\)/,/^}/' src/schwung_shim.c)
[ -n "$body" ] || fail "could not find shim_plock_held_step in src/schwung_shim.c"
echo "$body" | grep -q 'shadow_display_mode' \
  || fail "shim_plock_held_step lost its display guard -- without it a held step on MOVE's own screen would p-lock"
echo "$body" | grep -q 'm & (m - 1)' \
  || fail "shim_plock_held_step must refuse when MORE THAN ONE step is held -- two held steps name no single phase"

# AND IT MUST NOT BE static. shadow_chain_mgmt.c calls it; a shared library
# links clean with the symbol undefined, so `static` here builds green and
# fails at LOAD -- which for an LD_PRELOAD shim is MoveOriginal not starting.
# Measured: a crash loop, ~7 s a cycle, nothing in dmesg.
grep -q '^static int shim_plock_held_step' src/schwung_shim.c \
  && fail "shim_plock_held_step must NOT be static -- shadow_chain_mgmt.c calls it, and the shim would fail to LOAD"
grep -qE '^int shim_plock_held_step' src/schwung_shim.c \
  || fail "shim_plock_held_step must have external linkage"

# The build must be able to SAY so, rather than leaving it to the device.
grep -q -- '-Wl,--no-undefined' scripts/build.sh \
  || fail "the shim link must use -Wl,--no-undefined, or this class of error reaches hardware as a crash loop"
