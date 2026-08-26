#!/usr/bin/env bash
# Source pin: every master_fx:* key the SHIM answers must be DELEGATED to it.
#
# Two files name the same set of keys and neither refers to the other:
#
#   src/schwung_shim.c        shim_handle_param_special() — the handler,
#                             a ladder of strcmp(fx_key, "...")
#   src/host/shadow_chain_mgmt.c  the gate that decides whether to CALL it,
#                             a second ladder of strcmp(param_key, "...")
#
# Adding a key to the handler alone does nothing at all. The gate never
# delegates, the read falls through to slot 0's module, which does not know the
# key, and the caller gets an unserved "" — indistinguishable at the UI from a
# module that has no such parameter. That is not hypothetical: it is exactly how
# `master_fx:midi_channel` shipped dead in its first cut, showing "--" in
# Global Settings with no error anywhere in the log.
#
# Direction matters. shim-keys MUST be a subset of gate-keys; the reverse is
# fine, because the gate also lists things the shim answers by other means
# (the `jack:` prefix, suspend_overtake).
set -u

ROOT="$(dirname "$0")/../.."
SHIM="$ROOT/src/schwung_shim.c"
MGMT="$ROOT/src/host/shadow_chain_mgmt.c"

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

for f in "$SHIM" "$MGMT"; do
    [ -f "$f" ] || { echo "FAIL: cannot find $f" >&2; exit 1; }
done

# --- keys the shim's special handler answers -------------------------------
# Bounded to the function body so an unrelated fx_key comparison elsewhere in
# the shim is not mistaken for a delegated special.
shim_keys=$(awk '
    /^static int shim_handle_param_special\(/ { f = 1; next }
    f && /^}/ { exit }
    f { print }
' "$SHIM" | command grep -o 'strcmp(fx_key, "[^"]*"' \
  | sed 's/.*strcmp(fx_key, "//; s/"$//' | sort -u)

[ -n "$shim_keys" ] || fail "found no strcmp(fx_key, ...) keys in shim_handle_param_special — has it been renamed?"

# --- keys the delegation gate will hand over -------------------------------
gate_keys=$(awk '
    /if \(!has_slot_prefix && host\.handle_param_special\)/ { f = 1; next }
    f && /host\.handle_param_special\(req_type, req_id\)/ { exit }
    f { print }
' "$MGMT" | command grep -o 'strcmp(param_key, "[^"]*"' \
  | sed 's/.*strcmp(param_key, "//; s/"$//' | sort -u)

[ -n "$gate_keys" ] || fail "found no delegation whitelist in shadow_chain_mgmt.c — has the gate moved?"

# --- every handled key must be delegated -----------------------------------
if [ -n "$shim_keys" ] && [ -n "$gate_keys" ]; then
    missing=$(comm -23 <(printf '%s\n' "$shim_keys") <(printf '%s\n' "$gate_keys"))
    if [ -n "$missing" ]; then
        for k in $missing; do
            fail "master_fx:$k is answered by shim_handle_param_special but NOT delegated to it in shadow_chain_mgmt.c — the read falls through to slot 0 and returns unserved \"\""
        done
    fi
fi

# The key this test was written for, asserted by name: the generic subset check
# above passes vacuously if BOTH ladders lose it.
printf '%s\n' "$shim_keys" | command grep -qx "midi_channel" \
    || fail "the shim no longer answers master_fx:midi_channel"
printf '%s\n' "$gate_keys" | command grep -qx "midi_channel" \
    || fail "master_fx:midi_channel is no longer delegated — the MFX listen channel reads as unserved"

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed" >&2
    exit 1
fi
echo "PASS: all $(printf '%s\n' "$shim_keys" | wc -l | tr -d ' ') shim-answered master_fx keys are delegated"
