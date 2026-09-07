#!/usr/bin/env bash
#
# THE modN: / legacy lfoN: KEY LADDER.
#
# Pinned rather than trusted because this is the exact shape master_fx_key.h
# exists to fix: a hand-written strncmp ladder that restates its own cap. Its
# preamble is the war story -- a cap raise broke seven sibling sites silently,
# and one else-branch ASSIGNED SLOT 0, so an out-of-range key was not dropped
# but routed into a different running module under a garbage param name.
#
# The properties below are the ones such a ladder loses first:
#   - an out-of-range index landing on route 0
#   - a legacy alias that COPIES rather than resolving to the same storage
#   - the two route counts (slot vs Master FX) fusing into one
#   - a stale emit source_id, which silently orphans a modulation source
set -euo pipefail

cd "$(dirname "$0")/../.."
HOST="src/modules/chain/dsp/chain_host.c"
INT="src/modules/chain/dsp/chain_internal.h"
# The ladders live in chain_mod_routes.c, not chain_host.c: the cluster was
# split out when the source types pushed chain_host.c past its 2900-line pin.
ROUTES="src/modules/chain/dsp/chain_mod_routes.c"
[ -f "$HOST" ] && [ -f "$INT" ] && [ -f "$ROUTES" ] || { echo "FAIL: missing sources"; exit 1; }

fail=0
say_fail() { echo "FAIL: $1"; fail=1; }
say_ok()   { echo "  ok  $1"; }

# ---- the cap is named once, and it is 8 ---------------------------------
n=$(/usr/bin/grep -c '^#define MOD_ROUTE_COUNT' "$INT" || true)
if [ "$n" = "1" ]; then say_ok "MOD_ROUTE_COUNT is defined exactly once"
else say_fail "MOD_ROUTE_COUNT defined $n times, expected 1"; fi

if /usr/bin/grep -q '^#define MOD_ROUTE_COUNT 8' "$INT"; then
  say_ok "MOD_ROUTE_COUNT is 8"
else say_fail "MOD_ROUTE_COUNT is not 8"; fi

# ---- the chain has stopped reading the Master FX-shaped count ------------
# LFO_COUNT still exists in lfo_common.h and is still Master FX's business.
# The chain reading it is how the two silently re-fuse.
# -w so MASTER_FX_LFO_COUNT, which the header names only to say it is a
# DIFFERENT number, is not mistaken for a use of LFO_COUNT itself.
if /usr/bin/grep -rqw 'LFO_COUNT' src/modules/chain/dsp/; then
  echo "    offending lines:"
  /usr/bin/grep -rnw 'LFO_COUNT' src/modules/chain/dsp/ | /usr/bin/sed 's/^/      /'
  say_fail "src/modules/chain/dsp still reads LFO_COUNT; it must use MOD_ROUTE_COUNT"
else
  say_ok "the chain no longer reads LFO_COUNT"
fi

# ---- Master FX keeps its own count, and must not grow silently ----------
if /usr/bin/grep -q 'MASTER_FX_LFO_COUNT' src/host/shadow_chain_mgmt.h; then
  say_ok "Master FX still names its own count"
else say_fail "MASTER_FX_LFO_COUNT is gone"; fi

if /usr/bin/grep -q 'MOD_ROUTE_COUNT' src/host/shadow_chain_mgmt.c; then
  say_fail "shadow_chain_mgmt.c reads MOD_ROUTE_COUNT; Master FX must stay at 2"
else
  say_ok "Master FX does not read the slot route count"
fi

# ---- the ladder parses its index rather than enumerating it -------------
# "lfo1:"/"lfo2:" may be literal -- the legacy name is frozen at two by the
# on-disk format and can never grow. "modN:" must not be.
if /usr/bin/grep -qE 'strncmp\(key, "mod[0-9]:"' "$ROUTES"; then
  say_fail "the modN: ladder enumerates indices; parse them instead"
else
  say_ok "the modN: ladder does not enumerate its indices"
fi

if /usr/bin/grep -q 'chain_mod_route_index' "$ROUTES"; then
  say_ok "index resolution goes through one named helper"
else
  say_fail "no chain_mod_route_index helper; the ladder is open-coded"
fi

# ---- the wrapper DELEGATES; there is no second copy of the parse --------
# The parsing itself moved to src/host/mod_route_key.h, beside bus_route.h and
# send_fx_key.h, so it can be run natively -- tests/host/test_mod_route_key.c
# covers the rejection contract (out-of-range rejected not clamped, leading
# zeros refused, the legacy spelling capped at two, *out_rest left alone) as
# BEHAVIOUR rather than as source shape. What is left to pin here is that the
# in-tree helper stayed a one-line binding of the cap: a re-inlined parser would
# pass every behavioural test above while quietly carrying its own copy of
# MOD_ROUTE_COUNT, which is the exact drift master_fx_key.h exists to prevent.
body=$(/usr/bin/sed -n '/^int chain_mod_route_index(/,/^}$/p' "$ROUTES")
if [ -z "$body" ]; then
  say_fail "could not lift chain_mod_route_index()"
elif printf '%s' "$body" | /usr/bin/grep -q 'mod_route_parse_key(key, MOD_ROUTE_COUNT, out_rest)'; then
  say_ok "chain_mod_route_index delegates to the shared header, binding the cap once"
else
  say_fail "chain_mod_route_index no longer delegates to mod_route_key.h"
fi

lines=$(printf '%s' "$body" | /usr/bin/wc -l | /usr/bin/tr -d ' ')
if [ "$lines" -le 4 ]; then
  say_ok "the wrapper is still a binding, not a re-inlined parser ($lines lines)"
else
  say_fail "chain_mod_route_index is $lines lines -- the parse has been re-inlined"
fi

# ---- every route emits under a modN source_id ---------------------------
# chain_mod_clear_source matches on this string. If routes 1 and 2 kept
# emitting as "lfo1"/"lfo2" while the clear path said "mod1"/"mod2", a patch
# load would clear a source that does not exist and leave the real one running
# -- a stuck modulation with no UI showing it.
if /usr/bin/grep -rq 'source_id), "lfo%d"' src/modules/chain/dsp/; then
  say_fail "an emit source_id is still lfo%d; all eight routes must emit as modN"
else
  say_ok "every route emits under a modN source_id"
fi

# ---- the legacy alias resolves to the same storage, never a copy --------
if /usr/bin/grep -qE 'legacy_lfo|lfo_alias_copy|memcpy\(.*mod_routes.*lfos' "$HOST"; then
  say_fail "the legacy alias copies; it must resolve to the same route"
else
  say_ok "the legacy alias does not copy"
fi

# ---- lfo_config stays a TWO-entry legacy document ------------------------
# It is what shadow_ui.js writes into patch.lfos, and chain_patch.c's legacy
# reader only looks for lfo1/lfo2. Emitting lfo3..lfo8 here would write six
# routes that nothing ever reads back -- silent loss on every save.
cfg=$(/usr/bin/sed -n '/strcmp(key, "lfo_config")/,/^    }$/p' "$HOST")
if [ -z "$cfg" ]; then
  say_fail "could not lift the lfo_config getter"
elif printf '%s' "$cfg" | /usr/bin/grep -q 'MOD_ROUTE_COUNT'; then
  say_fail "lfo_config emits MOD_ROUTE_COUNT entries; the legacy document holds two"
else
  say_ok "lfo_config stays a two-entry legacy document"
fi

[ "$fail" = "0" ] && echo "ALL PASS" || { echo "FAIL"; exit 1; }
