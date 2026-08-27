#!/usr/bin/env bash
# Source pin: overtake_suppress_master_volume's three invariants.
#
# 1. THE TWO PLAIN-VOLUME-TOUCH PREDICATES MUST AGREE.
#    shadow_swap_display() decides whether the pixels in the mmap'd display
#    region are Move's or Schwung's. native_display_visible gates a scanner
#    that hunts MOVE's volume bar in those same pixels and writes
#    shadow_master_volume — mailbox gain — when it thinks it found one.
#    They are the same condition written twice, so a term added to one and
#    not the other points the scanner at Schwung's own OLED. That is exactly
#    what shipped in #291: the flag reached shadow_swap_display() only.
#
#    shadow_volume_knob_touched is tracked from the HARDWARE buffer, so it is
#    still 1 while the flag excludes Move from the touch — nothing else stops
#    the scan.
#
# 2. THE SUPPRESSION LIVES IN THE PRECEDENCE TAIL, NOT THE MODE BRANCHES.
#    The branches are last-writer-wins, and two later clauses clear `filter`
#    unconditionally: the capabilities.button_passthrough list, and the
#    move-native co-run cede (CORUN_GRP_MASTER is CC 79, CORUN_GRP_TOUCH
#    covers note 8). A guard placed in a branch is silently defeated by
#    either. So the flag must be enforced AFTER both.
#
# 3. IT IS CLEARED ON ANY OVERTAKE-MODE CHANGE, NOT JUST ON EXIT.
#    2 -> 1 carries it into the shadow menu, which honours it too, and the
#    menu's own volume knob goes dead until full exit.
set -u

SHIM="$(dirname "$0")/../../src/schwung_shim.c"
FLAG=overtake_suppress_master_volume
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

[ -f "$SHIM" ] || { echo "FAIL: cannot find $SHIM" >&2; exit 1; }

# --- 1. both plain-volume-touch predicates carry the flag ------------------
# Locate each predicate by its own anchor, then require the flag within the
# few lines that make up that condition. Anchoring on the anchor (rather than
# grepping the file for the flag) is what makes this fail when the term is
# dropped from one site while surviving at the other.
swap_line=$(grep -n 'shadow_volume_knob_touched && !shadow_shift_held' "$SHIM" | head -1 | cut -d: -f1)
[ -n "$swap_line" ] || fail "cannot find shadow_swap_display()'s plain-volume-touch test — has it moved?"
if [ -n "${swap_line:-}" ]; then
    sed -n "${swap_line},$((swap_line + 3))p" "$SHIM" | grep -q "$FLAG" \
        || fail "shadow_swap_display()'s volume-touch test does not check $FLAG"
fi

ndv_line=$(grep -n 'int native_display_visible' "$SHIM" | head -1 | cut -d: -f1)
[ -n "$ndv_line" ] || fail "cannot find native_display_visible — has it moved?"
if [ -n "${ndv_line:-}" ]; then
    sed -n "${ndv_line},$((ndv_line + 8))p" "$SHIM" | grep -q "$FLAG" \
        || fail "native_display_visible does not check $FLAG — the volume-bar scanner will read Schwung's own OLED and jump the mailbox gain"
fi

# --- 2. enforced after button_passthrough AND the co-run cede -------------
# The tail clause re-asserts filter = 1; the branch guards it replaced read
# !flag. Requiring the *last* flag mention in the loop to sit below both
# defeating clauses is the assertion that survives a refactor of either.
pass_line=$(grep -n 'overtake_passthrough_ccs\[d1\]' "$SHIM" | tail -1 | cut -d: -f1)
corun_line=$(grep -n 'corun_event_owner(shadow_control, type, d1) == CORUN_OWNER_PEER' "$SHIM" | tail -1 | cut -d: -f1)
tail_line=$(grep -n "shadow_control->$FLAG" "$SHIM" | tail -1 | cut -d: -f1)

[ -n "$pass_line" ]  || fail "cannot find the button_passthrough clause — has it moved?"
[ -n "$corun_line" ] || fail "cannot find the co-run cede clause — has it moved?"
[ -n "$tail_line" ]  || fail "the filter loop never reads $FLAG"

if [ -n "${pass_line:-}" ] && [ -n "${tail_line:-}" ] && [ "$tail_line" -lt "$pass_line" ]; then
    fail "$FLAG is enforced at line $tail_line, ABOVE the button_passthrough clause at $pass_line — a tool listing CC 79 in capabilities.button_passthrough silently gets no suppression"
fi
if [ -n "${corun_line:-}" ] && [ -n "${tail_line:-}" ] && [ "$tail_line" -lt "$corun_line" ]; then
    fail "$FLAG is enforced at line $tail_line, ABOVE the co-run cede at $corun_line — a tool ceding CORUN_GRP_MASTER/CORUN_GRP_TOUCH silently gets no suppression"
fi

# The tail must SET filter, not clear it: this flag takes the event away from
# Move. A clause that reads the flag and writes filter = 0 is the inverted fix.
if [ -n "${tail_line:-}" ]; then
    sed -n "${tail_line},$((tail_line + 6))p" "$SHIM" | grep -q 'filter = 1;' \
        || fail "the $FLAG clause does not set filter = 1 — suppression means Move must NOT see the event"
fi

# --- 3. cleared on any overtake-mode change ------------------------------
# Pinned as an absence as well as a presence: an exit-only clear is the bug.
grep -q 'prev_overtake_mode != overtake_mode' "$SHIM" \
    || fail "no 'prev_overtake_mode != overtake_mode' clear — the flag strands across a 2 -> 1 transition into the shadow menu"

clear_line=$(grep -n "prev_overtake_mode != overtake_mode" "$SHIM" | head -1 | cut -d: -f1)
if [ -n "${clear_line:-}" ]; then
    sed -n "${clear_line},$((clear_line + 3))p" "$SHIM" | grep -q "$FLAG = 0" \
        || fail "the mode-change block does not clear $FLAG"
fi

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed" >&2
    exit 1
fi
echo "PASS: $(basename "$0")"
