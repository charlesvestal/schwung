#!/usr/bin/env bash
# A REFUSED P-LOCK MUST NOT SPEND THE STEP PRESS.
#
# `shadow_lanes_plock_from_write` converts a knob write made while a step is
# held into a p-lock. Two consumers share that one gesture: the knob (whose
# live write the caller suppresses only when the lock LANDS) and the step
# button (whose press Move must still see as a note toggle if nothing landed).
#
# The spend -- shim_step_note_plock_key + shim_step_mark_used -- used to run
# BEFORE the `lanes:plocked` answer was read, so a refused lock still ate the
# press: the knob correctly kept working and the step silently did not toggle
# its note. With the kill switch off EVERY p-lock is refused, so a disarmed
# build ate every held-step press it saw.
#
# This is a CALL-ORDERING fact: the unit tests drive the chain directly and
# cannot see it, and the defect leaves both statements present and
# correct-looking. Same instrument as test_midi_in_compact_call_site.sh.
#
# COMMENTS ARE STRIPPED FIRST, and that is not tidiness. The first version of
# this pin matched `lanes:plocked` where the function NAMES it in a comment,
# four lines above the call -- so the mutation that puts the old order back
# passed. A grep pin that reads prose is measuring the prose.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys

src = "src/host/shadow_chain_mgmt.c"
text = open(src).read()

start = text.find("static int shadow_lanes_plock_from_write")
if start < 0:
    sys.exit("FAIL: shadow_lanes_plock_from_write not found in " + src)
end = text.find("\n}\n", start)
if end < 0:
    sys.exit("FAIL: could not find the end of shadow_lanes_plock_from_write")
body = text[start:end]

# Blank out comments and string literals, keeping line structure so the
# reported line numbers still mean something. A key named in prose, or in
# some other call's format string, must not satisfy this pin.
def blank(m):
    return re.sub(r"\S", " ", m.group(0))
code = re.sub(r"/\*.*?\*/", blank, body, flags=re.S)
code = re.sub(r"//[^\n]*", blank, code)

lines = code.split("\n")
def find(pat, want_call=True):
    for i, l in enumerate(lines):
        if re.search(pat, l):
            return i
    return None

# The READ is the get_param whose key argument is the string; the key sits on
# the continuation line, so look for the string in the (unblanked) body but
# require the call itself to be present in code.
if "get_param" not in code:
    sys.exit("FAIL: the conversion no longer calls get_param -- it cannot tell a "
             "landed lock from a refused one, so it must not suppress the live write either")

raw = body.split("\n")
landed = None
for i, l in enumerate(raw):
    if '"lanes:plocked"' in l and re.sub(r"\S", " ", code.split("\n")[i]) != code.split("\n")[i]:
        landed = i
        break
if landed is None:
    sys.exit("FAIL: the conversion no longer reads \"lanes:plocked\" as code -- "
             "if it only mentions it in a comment, nothing checks whether the lock landed")

note  = find(r"\bshim_step_note_plock_key\b")
spend = find(r"\bshim_step_mark_used\b")
if note is None:
    sys.exit("FAIL: shim_step_note_plock_key is gone from the conversion")
if spend is None:
    sys.exit("FAIL: shim_step_mark_used is gone -- a landed p-lock will also toggle the step's note")

if note < landed or spend < landed:
    sys.exit(
        "FAIL: the step press is spent (line %d/%d of the function) BEFORE the\n"
        "      \"lanes:plocked\" answer is read (line %d). A refused p-lock then eats\n"
        "      the press: the knob keeps working and the step does not toggle its\n"
        "      note. With the kill switch off every p-lock is refused, so this makes\n"
        "      a disarmed build eat every held-step press it sees."
        % (note + 1, spend + 1, landed + 1))

# And something must return between the answer and the spend, or the spend is
# unconditional again with the statements merely reordered.
between = "\n".join(lines[landed + 1:min(note, spend)])
if not re.search(r"\breturn\s+0\b", between):
    sys.exit("FAIL: nothing returns 0 between reading \"lanes:plocked\" and spending "
             "the press -- the spend is unconditional again")

print("PASS: a p-lock spends the step press only after the chain confirms it landed")
PY
