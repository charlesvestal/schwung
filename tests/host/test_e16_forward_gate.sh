#!/usr/bin/env bash
# Source pin: the external control surface (cable 2) reaches JS, and every one
# of the three sites that lets it through is GATED on external_surface.
#
# The shim cannot be compiled on a dev Mac, so this is a source pin rather than
# a unit. What it defends:
#
# 1. `external_surface` exists on shadow_control_t. Nothing else in the shim can
#    ask "is a control surface plugged in", so a rename that misses one side
#    silently turns the feature off rather than failing a build.
#
# 2. The cable-2 note-on diversion into shadow_queue_input_led keeps its gate.
#    That diversion is right for an M8-style LED protocol -- it COALESCES per
#    note and never publishes the event as input -- and fatal for a surface,
#    where every encoder BUTTON press is a note-on. Ungated, the buttons are
#    eaten with nothing logged, which is the failure this file exists for.
#
# 3. The non-overtake path still refuses cable 2 unless the flag is set. Without
#    that half, an external keyboard's notes would start arriving at the shadow
#    UI for every user, none of whom asked for a control surface.
#
# The gate condition is read from the STATEMENT, with comment lines stripped:
# the prose above these branches names external_surface, so a matcher that
# looked at raw context would pass on the comment alone and defend nothing.
set -u

SHIM="$(dirname "$0")/../../src/schwung_shim.c"
HDR="$(dirname "$0")/../../src/host/shadow_constants.h"
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

[ -f "$SHIM" ] || { echo "FAIL: cannot find $SHIM" >&2; exit 1; }
[ -f "$HDR" ] || { echo "FAIL: cannot find $HDR" >&2; exit 1; }

# Drop whole-line comments; a `/* ... */` opened mid-line stays, which is fine
# because the code before it is what we are reading.
strip_comments() { grep -vE '^[[:space:]]*(/\*|\*|//)'; }

# --- 1. The flag exists, as a field ---------------------------------------
if ! strip_comments < "$HDR" | grep -qE '^[[:space:]]*volatile uint8_t external_surface;'; then
    fail "shadow_control_t has no external_surface field — nothing can ask whether a surface is attached"
fi

# --- 2. The note-on diversion is gated ------------------------------------
# Find the statement that hands a cable-2 note-on to shadow_queue_input_led and
# read the `if` that guards it: everything from the nearest preceding
# non-comment `if (` down to the call. Joining the whole statement, rather than
# taking that one line, is what lets the condition wrap -- which it does, and a
# single-line matcher reported the gate missing while it was right there.
call_line=$(grep -nE '^[^*/]*shadow_queue_input_led\(' "$SHIM" | head -1 | cut -d: -f1)
if [ -z "$call_line" ]; then
    fail "no shadow_queue_input_led call site found — has the M8 LED path moved?"
else
    guard=$(head -n "$call_line" "$SHIM" | strip_comments \
        | awk '/^[[:space:]]*if \(/ { n = 0 } { buf[n++] = $0 }
               END { for (i = 0; i < n; i++) printf "%s ", buf[i] }')
    case "$guard" in
        *external_surface*) ;;
        *) fail "the cable-2 note-on diversion at line $call_line is not gated on external_surface (guard: ${guard:-none}) — every encoder button press is coalesced away" ;;
    esac
    case "$guard" in
        *"cable == 0x02"*) ;;
        *) fail "the guard above line $call_line no longer names cable 0x02 — the wrong branch was measured" ;;
    esac
fi

# --- 3. The non-overtake cable filter admits cable 2 under the flag -------
# Pinned by its own exact form, not by "the flag appears somewhere". This check
# once counted `cable == 0x02 && shadow_control->external_surface` anywhere in
# the file, and check 4's site carries that same phrase -- so deleting the
# filter's widening still left a match and the mutation passed. A pin that can
# be satisfied by a DIFFERENT site defends neither.
if ! strip_comments < "$SHIM" \
    | grep -qE 'cable != 0x00 &&[[:space:]]*$'; then
    fail "the post-ioctl cable filter no longer continues its condition — cable 2 is admitted unconditionally or not at all"
fi
if ! strip_comments < "$SHIM" \
    | grep -qE '^[[:space:]]*!\(cable == 0x02 && shadow_control->external_surface\)\) continue;'; then
    fail "the cable filter does not admit cable 2 under external_surface — the surface's encoders never reach the shadow UI"
fi

# --- 4. The claim lives in the UNCONDITIONAL walk, not the gated block ----
# A surface we own is ours whether or not our screen is up. The first version
# sat inside `if (shadow_display_mode ...)`, so stepping onto a Move track
# stopped the whole block and nothing swallowed: the E16's Shift (note 16,
# channel 1) played on slot 1 and its encoders sent CC 1, the mod wheel.
# Measured on hardware 2026-09-11, twice -- the second time AFTER a swallow had
# been added in the gated block, which is what identified the gate itself as
# the fault.
#
# Pinned as a NEGATIVE plus a POSITIVE, because either alone is satisfiable by
# the bug: the claim must not appear inside the display-gated block, and it
# must appear before it.
gate_line=$(grep -n "if (shadow_display_mode && shadow_control && hardware_mmap_addr)" "$SHIM" \
            | head -1 | cut -d: -f1)
claim_line=$(grep -n "e16_claims_msg(1, st, e_d1)" "$SHIM" | head -1 | cut -d: -f1)

if [ -z "$claim_line" ]; then
    fail "no claim site found -- the surface either takes the whole cable or none of it"
elif [ -z "$gate_line" ]; then
    fail "the shadow_display_mode block is gone; this pin needs rewriting against whatever replaced it"
elif [ "$claim_line" -gt "$gate_line" ]; then
    fail "the claim sits inside the shadow_display_mode block (line $claim_line > $gate_line) — every E16 control reaches Move the moment the Schwung screen is not up"
fi

# ...and it must SWALLOW, adjacently. The file has eighteen other swallow sites,
# so a bare grep would pass with this one missing.
if [ -n "$claim_line" ]; then
    body=$(sed -n "${claim_line},$((claim_line + 12))p" "$SHIM")
    case "$body" in
        *"midi_in_swallow(sh_midi, hw_midi, j)"*) ;;
        *) fail "a claimed surface message is not swallowed from Move mailbox — every encoder press also plays a note" ;;
    esac
fi

# --- 5. The claim is NARROW ------------------------------------------------
# Consuming every cable-2 event while the setting is on silences the CC Map for
# any other device sharing the port for as long as the surface is switched on.
if ! strip_comments < "$SHIM" | grep -qE 'e16_claims_msg\(1, st, e_d1\)\) \{'; then
    fail "the publish site claims the whole cable, not just the surface own messages"
fi

# --- 6. Inbound SysEx reaches JS, and is NOT swallowed ---------------------
# With the claim narrowed to channel-voice messages, the E16 ACK reached the
# mailbox and was never handed to JS, so the lifecycle sought forever and
# withheld every frame: the device entered remote mode and stayed blank.
#
# It must ride in the same unconditional walk for the same reason the claim
# does -- presence cannot depend on which screen the Move is showing -- and it
# must NOT swallow, since a chain slot declaring capabilities.wants_sysex is
# still entitled to it.
if ! strip_comments < "$SHIM" | grep -qE 'if \(cin >= 0x04 && cin <= 0x07\) \{'; then
    fail "inbound SysEx is not published to JS -- the ACK never arrives and the surface withholds every frame"
fi
sysex_line=$(grep -n "if (cin >= 0x04 && cin <= 0x07) {" "$SHIM" | head -1 | cut -d: -f1)
if [ -n "$sysex_line" ] && [ -n "$gate_line" ] && [ "$sysex_line" -gt "$gate_line" ]; then
    fail "the SysEx publish is inside the display-gated block — the surface loses presence whenever the Schwung screen is not up"
fi
if [ -n "$sysex_line" ]; then
    sbody=$(sed -n "${sysex_line},$((sysex_line + 6))p" "$SHIM")
    case "$sbody" in
        *"midi_in_swallow"*) fail "the SysEx publish swallows the cable -- a wants_sysex slot would stop receiving" ;;
    esac
fi

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed" >&2
    exit 1
fi
echo "PASS: cable 2 reaches JS only behind external_surface, and its note-ons are not diverted to the LED queue"
