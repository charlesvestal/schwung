#!/usr/bin/env bash
# THE DEVICE HARNESS MUST KNOW MOVE HAS THREE PAD MODES.
#
# `clipkit.py` modelled two. Its `ui_mode()` said "1 = Session, 0 = not" and
# `ensure_note()` waited for 0 -- which is UI_UNKNOWN. NOTE is 2. So
# ensure_note() could never succeed: it tapped Menu, gave up, returned False,
# and every caller ignored the return and tapped steps anyway -- in SESSION
# view, where a step button does not add a note.
#
# The cost was a full afternoon: three "create a clip" runs created nothing,
# `saveSongIfDirty` was declared broken against a song that was never dirty,
# and a wrong-clip bug was investigated with an instrument that was measuring
# nothing. A harness that cannot reach the mode it claims is worse than no
# harness, because its silence reads as a result.
#
# Pinned here because the harness lives in the repo but runs on the device,
# so nothing else can catch a regression in it.
set -euo pipefail
cd "$(dirname "$0")/../.."

K=docs/plans/clipkit.py
fail() { echo "FAIL: $1"; exit 1; }
[ -f "$K" ] || fail "the device harness is missing ($K)"

# The labels must match the shim's, which is the authority.
H=src/host/move_ui_mode_label.h
for pair in "UI_UNKNOWN:MOVE_UI_MODE_UNKNOWN 0" "UI_SESSION:MOVE_UI_MODE_SESSION 1" \
            "UI_NOTE:MOVE_UI_MODE_NOTE 2" "UI_SET_OVERVIEW:MOVE_UI_MODE_SET_OVERVIEW 3"; do
  py="${pair%%:*}"; rest="${pair#*:}"; c="${rest%% *}"; want="${rest##* }"
  grep -q "define $c *$want" "$H" || fail "$H no longer defines $c as $want"
done
grep -q 'UI_UNKNOWN, UI_SESSION, UI_NOTE, UI_SET_OVERVIEW = 0, 1, 2, 3' "$K" \
  || fail "clipkit's mode constants no longer match move_ui_mode_label.h"

# ensure_note must not target UNKNOWN -- the original defect, exactly.
grep -q 'def ensure_note' "$K" || fail "ensure_note is gone"
grep -q '_ensure_ui(UI_NOTE' "$K" \
  || fail "ensure_note no longer targets UI_NOTE -- targeting UI_UNKNOWN is the bug that cost an afternoon"

# NOTE is reached by a TRACK press, not by Menu: Menu puts up SESSION, so
# cycling it to reach NOTE sits in SESSION forever.
grep -q 'target == UI_NOTE' "$K" \
  || fail "the NOTE path no longer differs from the SESSION path -- Menu cannot reach NOTE"

# A setup that does not land must STOP the run -- checked by PARSING, not by
# grepping for the word `raise`. The first version of this looked for the
# string, so short-circuiting the failure path with `return False` in front of
# the raise sailed through: the raise was still present and still unreachable.
python3 - "$K" <<'PYCHK' || fail "_ensure_ui can return instead of raising -- every caller ignored the old return value and tapped in the wrong mode"
import ast, sys
tree = ast.parse(open(sys.argv[1]).read())
fn = next((n for n in ast.walk(tree)
           if isinstance(n, ast.FunctionDef) and n.name == "_ensure_ui"), None)
if fn is None:
    sys.exit("_ensure_ui is gone")
if not any(isinstance(n, ast.Raise) for n in ast.walk(fn)):
    sys.exit("no raise in _ensure_ui")
for n in ast.walk(fn):
    if isinstance(n, ast.Return) and isinstance(n.value, ast.Constant) \
       and not n.value.value:
        sys.exit("_ensure_ui returns a falsy value instead of raising")
sys.exit(0)
PYCHK

# And pads must be refused in Set Overview, where they swap the loaded SET.
grep -q 'def assert_pads_safe' "$K" || fail "assert_pads_safe is gone"
grep -q 'UI_SET_OVERVIEW' "$K" || fail "the Set Overview guard no longer names the mode"

python3 -c "import ast,sys; ast.parse(open('$K').read())" \
  || fail "clipkit.py does not parse"

# AND IMPORTING THE HARNESS MUST NOT ARM THE FEATURE. It used to call
# arm_lanes(True) at module level, so reading any value through it switched
# lanes on -- and testing the kill switch through it re-armed the flag between
# the delete and the read, reporting "cannot disarm" for a switch that works.
python3 - "$K" <<'PYARM' || fail "clipkit arms lanes at import -- the instrument changes what it measures, and it hides whether the kill switch can be turned off"
import ast, sys
tree = ast.parse(open(sys.argv[1]).read())
for node in tree.body:                       # module level only
    if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
        fn = node.value.func
        if isinstance(fn, ast.Name) and fn.id == "arm_lanes":
            sys.exit("arm_lanes called at import")
sys.exit(0)
PYARM

echo "PASS: the device harness models three pad modes and refuses an unconfirmed one"
