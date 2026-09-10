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

# --- 4. The non-overtake publish site is gated too -------------------------
# The filter above is not the only thing standing between a stray cable and
# this branch: it re-tests the flag itself so a later widening of that filter
# for some other reason cannot make this `continue` swallow the cable whole.
if ! strip_comments < "$SHIM" \
    | grep -qE '^[[:space:]]*if \(!overtake_mode && cable == 0x02 && shadow_control->external_surface &&$'; then
    fail "the non-overtake cable-2 publish is missing or ungated — the surface either never reaches JS, or swallows a cable it was not given"
fi
# ...and it must be NARROW. The first version consumed every cable-2 event
# while the setting was on, which silences the CC Map for any other device
# sharing the cable for as long as the surface is switched on. Pinned
# separately from the flag because dropping either one is silent: without
# the flag the cable is swallowed unconditionally, without the claim it is
# swallowed whenever the surface is on.
if ! strip_comments < "$SHIM" \
    | grep -qE '^[[:space:]]*e16_claims_msg\(1, status, d1\)\) \{'; then
    fail "the publish site claims the whole cable, not just the surface own messages"
fi


# --- 6. Inbound SysEx reaches JS, and is NOT swallowed ---------------------
# Measured on hardware 2026-09-10: with the claim narrowed to channel-voice
# messages, the E16's ACK reached the mailbox and was never handed to JS, so
# the lifecycle sought forever and withheld every frame. The device entered
# remote mode and stayed blank -- eleven ENTERs out, not one framebuffer.
#
# The absence of `continue` is pinned too. A chain slot declaring
# capabilities.wants_sysex must still receive this; the surface is one consumer
# of inbound SysEx, not its owner.
if ! strip_comments < "$SHIM" \
    | grep -qE '^[[:space:]]*cin >= 0x04 && cin <= 0x07\) \{'; then
    fail "inbound SysEx is not published to JS -- the ACK never arrives and the surface withholds every frame"
fi
if strip_comments < "$SHIM" \
    | grep -A2 -E '^[[:space:]]*cin >= 0x04 && cin <= 0x07\) \{' \
    | grep -qE '^[[:space:]]*continue;'; then
    fail "the SysEx publish swallows the cable -- a wants_sysex slot would stop receiving"
fi


# --- 7. SysEx survives the CIN gate for a configured surface ---------------
# The non-overtake path drops CINs 0x04-0x07 before any cable test. That is the
# gate docs/SYSEX.md names as the reason a chain slot is write-only for SysEx.
# The surface's ACK is SysEx, so widening only the CABLE condition is not
# enough: measured on hardware 2026-09-10, the ACK died here, `present` never
# flipped, and the device sat in remote mode with every frame withheld.
if ! strip_comments < "$SHIM" \
    | grep -qE 'cin >= 0x04 && cin <= 0x07 && cable == 0x02 &&'; then
    fail "SysEx does not survive the CIN gate -- the ACK never reaches JS and the surface withholds every frame"
fi

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed" >&2
    exit 1
fi
echo "PASS: cable 2 reaches JS only behind external_surface, and its note-ons are not diverted to the LED queue"
