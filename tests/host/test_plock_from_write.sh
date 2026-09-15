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

# A LANDED P-LOCK REPLACES THE LIVE WRITE, it does not accompany it. Elektron's
# rule: holding a trig and turning edits that step and leaves the track value
# alone. Applying both is one gesture changing two things, and the one you did
# not ask for is the one that plays on every other step.
n=$(grep -c 'if (!shadow_lanes_plock_from_write(slot,' "$src" || true)
[ "$n" = "2" ] || fail "both param paths must SUPPRESS the live write on a landed p-lock, found $n"
grep -q 'lanes:plocked", landed' "$src" \
  || fail "from_write must report whether the p-lock LANDED -- suppressing the write on a refusal is a dead knob"

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

# A RECORDING PASS IS NEVER CONVERTED INTO P-LOCKS.
#
# This is the reason the first attempt was reverted rather than debugged: a
# p-lock writes a rectangle at one phase into the same lane a sweep is being
# recorded into, so a stale held step made ordinary recording WORSE. The two
# gestures are mutually exclusive by construction now, and the condition is
# the record branch's own -- asked of the chain, never restated here.
body=$(awk '/^static int shadow_lanes_plock_from_write/,/^}/' "$src")
[ -n "$body" ] || fail "could not find shadow_lanes_plock_from_write in $src"
echo "$body" | grep -q 'lanes:recording' \
  || fail "the write-time p-lock must refuse while a recording pass is running, or a held step punches stepped points through a take"
echo "$body" | grep -q "rn < 0" \
  || fail "a FAILED get_param is not a 'no' -- converting on an unserved read is the tri-state mistake (CLAUDE.md)"

# ...and the predicate must live ONCE. A host-side copy of "armed and the
# phase is known" is free to drift from the branch it is meant to mirror.
lanes=src/modules/chain/dsp/chain_lanes.c
grep -q 'static int lane_is_recording' "$lanes" \
  || fail "lane_is_recording must exist in $lanes -- it is the one statement of the recording condition"
grep -q 'if (lane_is_recording(inst)) {' "$lanes" \
  || fail "lane_on_set_param's record branch must TAKE the predicate, not restate it"
n=$(grep -c 'inst->lane_armed && inst->clip_phase_valid' "$lanes" || true)
[ "$n" = "1" ] || fail "the recording condition must be written exactly once (inside lane_is_recording), found $n"
grep -q '"recording"' "$lanes" \
  || fail "the chain must SERVE lanes:recording -- the host has no other way to ask"

echo "PASS: plock-from-write guards"

# ...AND IT MUST SAY SO. The gesture is silent by nature -- a p-lock changes
# nothing audible until the loop reaches that step -- so eight correct p-locks
# on hardware were reported as the feature not working. The mark is the fix,
# and what it is pinned on is that it can never claim more than happened.
cbody=$(awk '/^static void shadow_lanes_plock_confirm/,/^}/' "$src")
[ -n "$cbody" ] || fail "could not find shadow_lanes_plock_confirm in $src"
echo "$cbody" | grep -q 'lanes:plocked' \
  || fail "the mark must be confirmed by the CHAIN's own answer -- forwarding a p-lock is not the same as landing one (unknown param, full store)"
echo "$cbody" | grep -q 'plock_seq++' || fail "shadow_lanes_plock_confirm must bump plock_seq"

# All THREE paths that write `lanes:plock` confirm, or the gesture reports
# itself on some screens and not others -- this feature's recurring shape.
n=$(grep -c 'shadow_lanes_plock_confirm(slot)' "$src" || true)
[ "$n" = "3" ] || fail "expected the confirm at all THREE plock write sites (write-time, SHM, direct/web), found $n"

# APPENDED, never inserted: sizeof(shadow_control_t) is a contract between two
# binaries and schwung-manager reads `stay_in_shadow` out of the same struct at
# a raw offset.
#
# EACH FIELD'S POSITION IS PINNED, and getting here took three tries worth
# recording, because the two weaker forms both PASSED a mutation that inserted
# a field:
#
#   "the last field is one of these three"  - a name list that has to be edited
#       every time a field is appended. The same shape as install.sh's
#       features.json key list, which this project has already been burned by:
#       a check you must update to keep it passing gets updated, and stops
#       meaning anything.
#   "they appear in this relative order"    - putting a new field BETWEEN two of
#       them leaves their order intact. Mutated and it passed.
#
# Only the absolute position moves when something is inserted, and it moves for
# every field behind the insertion. Appending to the tail never shifts one, so
# these numbers do not need touching in the normal case -- if one fails, the
# layout moved and that is the finding, not a number to bump.
fields=$(awk '/^typedef struct shadow_control_t/,/^} shadow_control_t;/' src/host/shadow_constants.h \
         | grep -E '^\s+volatile ' | sed -E 's/.*[ *]([a-z_0-9]+)(\[[0-9]*\])?;.*/\1/')
idx_of() { echo "$fields" | grep -nxF "$1" | cut -d: -f1; }
check_at() {
  local got; got=$(idx_of "$1")
  [ -n "$got" ] || fail "$1 has left shadow_control_t -- the lanes fields are a published layout"
  [ "$got" = "$2" ] \
    || fail "$1 is volatile field $got, not $2 -- a field was INSERTED ahead of it, which shifts every offset behind it"
}
check_at plock_seq          69
check_at lanes_driving_mask 70
check_at held_step          71

# The first read of the counter must arm NOTHING, or every entry to the UI
# flashes a mark for the previous session's last p-lock.
mark=$(awk '/^function drawPlockMark/,/^}/' src/shadow/shadow_ui.js)
[ -n "$mark" ] || fail "could not find drawPlockMark in src/shadow/shadow_ui.js"
echo "$mark" | grep -q 'plockMarkSeq === null' \
  || fail "drawPlockMark must take a baseline on its first sample rather than treating it as a change"
grep -q 'drawPlockMark();' src/shadow/shadow_ui.js \
  || fail "drawPlockMark must be CALLED -- from the overlay block after the view switch, so it lands over a module's own frame too"

echo "PASS: plock landed-mark"

# THE PLAYBACK HALF. A landed p-lock is invisible until the loop reaches it,
# and when it does, a module drawing its own screen still shows nothing --
# automation running and nothing running look identical. The knob grid has the
# per-key form (the mod dot, off `<key>:modulated`); this is slot altitude.
grep -q 'strcmp(sub, "driving")' "$lanes" \
  || fail "the chain must serve lanes:driving -- the UI has no other way to know a lane is speaking"
pub=$(awk '/^void shadow_lanes_publish_driving/,/^}/' "$src")
[ -n "$pub" ] || fail "could not find shadow_lanes_publish_driving in $src"
echo "$pub" | grep -q 'lanes_driving_mask = mask' \
  || fail "the publisher must write the whole mask each pass -- a lamp that only ever ORs bits in never goes out"
grep -q 'LANES_DRIVING_PUBLISH_FRAMES) == 0' src/schwung_shim.c \
  || fail "the shim must publish the lamp periodically, not per frame (four chain get_params on every SPI callback)"
echo "$mark" >/dev/null   # re-read: drawPlockMark changed above
mark=$(awk '/^function drawPlockMark/,/^}/' src/shadow/shadow_ui.js)
echo "$mark" | grep -q 'lanesDrivingHere()' \
  || fail "the mark must stay lit while a lane is driving, not only flash when one lands -- the flash alone was the reported gap"

echo "PASS: automation lamp"
