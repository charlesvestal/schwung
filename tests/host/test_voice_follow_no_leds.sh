#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# MOVE OWNS THE PADS.
#
# The voice-follow path reads a param and navigates. It must never light the
# rack, however natural that looks while writing it: the pads belong to Move's
# firmware while the shadow UI is up, and a second writer produces exactly the
# stuck-LED class of bug that input_filter's setLED cache already made
# permanent once (it recorded the colour it believed it had sent and suppressed
# the next identical repaint, so a dropped packet was never retried -- a
# dropped LED was permanent, not a flicker).
#
# Decided explicitly: "we shouldn't take over the LEDs, move handles the pads
# LEDs". Pinned here so the next person to reach for rack lighting finds a red
# test and this paragraph, rather than a comment they can talk themselves past.
#
# Two things this test is careful about, both of which have burned this repo:
#
#   - It asserts on COMMENT-STRIPPED source. An assertion that trips on its own
#     documentation proves nothing, and the function it guards carries a
#     multi-line block comment that talks about LEDs out loud. A line-wise
#     `s:/\*.*\*/::` does NOT remove a multi-line block comment, so the strip is
#     done with perl over the whole file first.
#   - It extracts the function body by BRACE DEPTH from the signature line, not
#     by a guessed closing-indent pattern, and it fails loudly on an empty or
#     truncated extraction. A range that matches nothing passes against
#     everything, which is the one way a pin like this ships worthless.

PC=src/shared/param_pages/page_controller.mjs
VOICES=src/shared/param_pages/voices.mjs

# setLED / sendMidi are the JS UI helpers; move_midi_inject_to_move,
# move_midi_internal_send and the host_midi_* bindings are the host functions
# underneath them. Any of these on this path means a second writer to the pads.
FORBIDDEN="setLED|sendMidi|move_midi|midi_internal_send|host_midi|move_midi_inject_to_move"

fail() {
    echo "FAIL: $1"
    echo
    echo "  Move's firmware owns the pad LEDs while the shadow UI is up."
    echo "  The voice-follow path is a READ plus a NAVIGATION -- it must not"
    echo "  emit a single LED or MIDI byte. A second writer to the pads is how"
    echo "  this repo shipped a permanently stuck LED once already: the setLED"
    echo "  cache suppressed the repaint that would have healed a dropped"
    echo "  packet, so the drop was permanent rather than a flicker."
    echo
    echo "  If you are adding rack lighting, that is a design change and has to"
    echo "  be decided -- do not delete this assertion to make room for it."
    exit 1
}

[ -f "$PC" ] || fail "page_controller.mjs is missing"
[ -f "$VOICES" ] || fail "voices.mjs is missing"

strip_comments() { perl -0pe 's{/\*.*?\*/}{}gs' "$1" | sed 's://.*::'; }

# --- voices.mjs is pure: no output of any kind, anywhere in the file --------
VOICES_SRC=$(strip_comments "$VOICES")
[ -n "$VOICES_SRC" ] || fail "voices.mjs stripped to nothing -- the comment strip is broken"

if printf '%s\n' "$VOICES_SRC" | grep -Eq "$FORBIDDEN"; then
    fail "voices.mjs emits MIDI or LED writes -- it must stay a pure reader"
fi

# --- syncVoiceFromModule's body, by brace depth ----------------------------
#
# Comments are stripped BEFORE the extraction, so a brace inside a comment
# cannot end the body early -- and so the body being searched carries none of
# the prose that describes this very rule.
# STOP PRINTING, DO NOT `exit`.
#
# `exit` closes the pipe while strip_comments is still writing, so its `sed`
# takes SIGPIPE -- and under `set -o pipefail` that is the pipeline's status.
# BSD sed dies quietly so this passed on a Mac for as long as anyone looked;
# GNU sed reports "couldn't write N items to stdout: Broken pipe" and the test
# fails on CI with exit 4, naming a test that is not the one that changed.
# Draining the rest of the input costs microseconds and cannot SIGPIPE anyone.
BODY=$(strip_comments "$PC" | awk '
    /function syncVoiceFromModule\(/ { on = 1 }
    on && !done {
        print
        d += gsub(/\{/, "{")
        d -= gsub(/\}/, "}")
        if (d <= 0) done = 1
    }')

[ -n "$BODY" ] || fail "syncVoiceFromModule not found in page_controller.mjs -- the extraction matched nothing, so this test was asserting on an empty string"

# A body of one or two lines means the brace walk stopped dead on the signature
# line; that is an empty extraction wearing a disguise, and it would pass.
LINES=$(printf '%s\n' "$BODY" | wc -l | tr -d ' ')
[ "$LINES" -ge 10 ] || fail "syncVoiceFromModule extracted only $LINES line(s) -- the brace walk did not capture the body"

# And the last captured line must be the function's closing brace, or the walk
# ran off the end of the file and swept in every function after it.
LAST=$(printf '%s\n' "$BODY" | tail -1 | tr -d ' \t')
[ "$LAST" = "}" ] || fail "syncVoiceFromModule extraction did not end on a closing brace (got '$LAST') -- the brace walk is broken"

if printf '%s\n' "$BODY" | grep -Eq "$FORBIDDEN"; then
    fail "the voice-follow path writes LEDs or MIDI -- Move owns the pads"
fi

echo "PASS: voice follow writes no LEDs"
