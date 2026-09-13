#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A STEP IS TWO GESTURES, AND THE RELEASE SAYS WHICH.
#
# The grid withholds every bare step press so that locking a value on a step
# does not also toggle a note in the clip. Swallowing it outright fixed the
# stray note and took Move's own step editing away for as long as the grid was
# on screen -- while Schwung was up you could not put a note on a step at all.
#
# Elektron splits the same button the same way and this follows it: a TAP
# toggles the trig, a HOLD enters parameter-lock without toggling. So the press
# is DEFERRED, and step_note_withhold() is where the release decides. This runs
# the real function, lifted, because the decision is three lines and all three
# are easy to get backwards.

fail() { echo "FAIL: $1"; exit 1; }
src=src/schwung_shim.c
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

awk '/^static void step_note_withhold/,/^}/' "$src" > "$tmp/fn.inc"
[ -s "$tmp/fn.inc" ] || fail "could not lift step_note_withhold out of $src"
# The real threshold, never a copy of it.
grep -E '^#define STEP_TAP_MS ' "$src" > "$tmp/tap.inc" || fail "STEP_TAP_MS is not defined in $src"

cat > "$tmp/t.c" <<'EOF'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "tap.inc"

static uint8_t  step_swallow_latch[16];
static uint64_t step_press_ms[16];
static uint8_t  step_press_vel[16];
static uint8_t  step_tap_replay[16];
/* The real one is volatile and lives beside the SPI callback's state; here it
 * only has to exist, so that the mask maintenance inside the lifted function
 * compiles and can be asserted on. */
static volatile uint32_t shadow_steps_held_mask;
static uint64_t g_now = 1000;            /* never 0: 0 means "no press seen" */
static uint64_t now_mono_ms(void) { return g_now; }

#include "fn.inc"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                       printf("\n"); fails++; } } while (0)
static void reset(void) {
    memset(step_swallow_latch, 0, sizeof(step_swallow_latch));
    memset(step_press_ms, 0, sizeof(step_press_ms));
    memset(step_press_vel, 0, sizeof(step_press_vel));
    memset(step_tap_replay, 0, sizeof(step_tap_replay));
    shadow_steps_held_mask = 0;
    g_now = 1000;
}

int main(void) {
    /* A TAP: Move gets the note it would have got. */
    reset();
    step_note_withhold(20, 100);
    CHECK(shadow_steps_held_mask == (1u << 4),
          "the withhold must mark the step HELD itself -- the swallow is what stops "
          "midi_monitor from ever seeing the press, and without this the gesture eats itself");
    CHECK(step_swallow_latch[4] == 1, "a press must latch, or its release reaches Move as an orphan button-up");
    CHECK(step_press_vel[4] == 100, "the press's own velocity must be kept -- a replay at 127 writes a different note than the finger did");
    g_now += STEP_TAP_MS - 1;
    step_note_withhold(20, 0);
    CHECK(shadow_steps_held_mask == 0, "the release must clear the mask, or the step stays held forever");
    CHECK(step_tap_replay[4] == 1, "a release inside STEP_TAP_MS is a TAP and must be replayed to Move");
    CHECK(step_swallow_latch[4] == 0, "the release retires the latch");

    /* A HOLD: Move is told nothing, which is what makes a lock trig possible. */
    reset();
    step_note_withhold(20, 100);
    g_now += STEP_TAP_MS;
    step_note_withhold(20, 0);
    CHECK(step_tap_replay[4] == 0, "a release AT the threshold is a hold -- it must not toggle a note");
    reset();
    step_note_withhold(31, 64);
    g_now += STEP_TAP_MS * 10;
    step_note_withhold(31, 0);
    CHECK(step_tap_replay[15] == 0, "a long hold must never replay: automation on a step with no note is the point");

    /* A release with no press behind it invents nothing. That is not
     * hypothetical -- the latch outlives the grid, so a build that changes
     * under a held finger, or a lost press, lands exactly here. */
    reset();
    step_swallow_latch[4] = 1;              /* latched, but no press time */
    step_note_withhold(20, 0);
    CHECK(step_tap_replay[4] == 0, "a release with no recorded press must not put a note on a step nobody touched");

    /* Steps are independent: one finger's hold must not silence another's tap. */
    reset();
    step_note_withhold(16, 100);            /* step 0 goes down */
    g_now += STEP_TAP_MS * 4;               /* ...and is a hold */
    step_note_withhold(21, 100);            /* step 5 taps inside it */
    g_now += 10;
    step_note_withhold(21, 0);
    CHECK(step_tap_replay[5] == 1, "a tap on one step must be a tap while another is held");
    step_note_withhold(16, 0);
    CHECK(step_tap_replay[0] == 0, "and the held one is still a hold");

    /* Out of range is not a step. */
    reset();
    step_note_withhold(15, 100);
    step_note_withhold(32, 100);
    for (int i = 0; i < 16; i++)
        CHECK(step_swallow_latch[i] == 0, "notes outside 16..31 are not steps");

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: step tap vs hold (threshold %d ms)\n", STEP_TAP_MS);
    return 0;
}
EOF
cc -O1 -Wall -Wextra -I "$tmp" -o "$tmp/t" "$tmp/t.c" || fail "compile"
"$tmp/t"

# The replay must be EMITTED after compaction, never un-swallowed: by the time
# the release arrives the press's slot is long gone, and nothing may move a
# slot while the index-paired swallows above are still running.
line_compact=$(grep -n 'shadow_midi_in_compact(global_mmap_addr' "$src" | cut -d: -f1)
line_replay=$(grep -n 'step_tap_replay\[i\] = on ? 2 : 0;' "$src" | cut -d: -f1)
[ -n "$line_compact" ] && [ -n "$line_replay" ] || fail "could not find the compaction and the tap replay"
[ "$line_replay" -gt "$line_compact" ] \
  || fail "the tap replay must be emitted AFTER shadow_midi_in_compact(), where the free slots are a contiguous tail"

# Both swallow sites must take the same decision, or a tap that ends after the
# grid is dismissed toggles nothing.
n=$(grep -c 'step_note_withhold(d1, d2)' "$src" || true)
[ "$n" = "2" ] || fail "both the gated swallow and the unconditional drain must call step_note_withhold, found $n"

# AND THE WITHHOLD MUST KEEP THE HELD-STEP MASK ITSELF.
#
# `shadow_steps_held_mask` is otherwise maintained by midi_monitor(), which
# reads the HARDWARE mailbox -- and midi_in_swallow zeroes that mailbox along
# with Move's copy. So a withheld step is invisible to the tracker: held_step
# reads NONE, no `<key>:held` resolves, and no write becomes a p-lock. The
# gesture eats itself, and it did: measured on hardware with the step down and
# the shim reporting 255.
fnbody=$(awk '/^static void step_note_withhold/,/^}/' "$src")
echo "$fnbody" | grep -q 'shadow_steps_held_mask |= (1u << i)' \
  || fail "the withhold must SET the held-step mask -- the swallow is what stops midi_monitor from seeing the press"
echo "$fnbody" | grep -q 'shadow_steps_held_mask &= ~(1u << i)' \
  || fail "and clear it on the release, or the step stays held forever"
