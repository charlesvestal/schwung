#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

host="src/modules/chain/dsp/chain_host.c"
mod="src/modules/chain/dsp/chain_mod.c"
internal="src/modules/chain/dsp/chain_internal.h"
common="src/host/lock_common.h"

# --- the non-destructive contract -------------------------------------------
# A lock must never write a base value. The only write it may make is through
# the modulation bus, which restores the base on clear.
if rg -q 'lock_tick' "$host" && rg -n 'lock_tick' -A40 "$host" | rg -q 'chain_mod_set_param_string'; then
  echo "FAIL: lock_tick must not write parameters directly" >&2
  exit 1
fi

if ! rg -q 'chain_mod_emit_absolute\(inst, source_id, lane->target, lane->param,' "$host"; then
  echo "FAIL: lock playback does not publish through the modulation bus" >&2
  exit 1
fi

# --- absolute, not relative --------------------------------------------------
# A p-lock states its value. Expressed as an offset it would re-base whenever
# the user turned the knob, so the absolute path must exist and be used.
if ! rg -q 'int absolute;' "$internal"; then
  echo "FAIL: mod_source_contribution_t is missing the absolute flag" >&2
  exit 1
fi

if ! rg -q 'chain_mod_active_absolute' "$mod"; then
  echo "FAIL: chain_mod has no absolute-source resolution" >&2
  exit 1
fi

# Relative contributions must be excluded from the sum when absolute, or an
# absolute source would be added to itself.
if ! rg -q 'if \(entry->sources\[i\]\.absolute\) continue;' "$mod"; then
  echo "FAIL: absolute sources are not excluded from the relative sum" >&2
  exit 1
fi

# An absolute source replaces the ORIGIN, so relative sources (an LFO) still
# apply on top of a locked value.
if ! rg -q 'const float origin = abs_src \? abs_src->contribution : entry->base_value;' "$mod"; then
  echo "FAIL: effective value does not take its origin from the absolute source" >&2
  exit 1
fi

# --- locks must advance on silent slots -------------------------------------
# render_block is skipped on a silent slot, so a lock that only ticked there
# would land a step late whenever the sequence came back from silence.
if ! rg -n 'mod:tick' -A12 "$host" | rg -q 'lock_tick\(inst\);'; then
  echo "FAIL: lock_tick is not driven from the mod:tick path" >&2
  exit 1
fi

# --- pattern length is explicit, never inferred ------------------------------
# Nothing reports Move's clip length. Guessing it puts every lock on the wrong
# step silently, so both the length and the step rate are settable.
for k in pattern_len rate_div; do
  if ! rg -q "strcmp\(subkey, \"$k\"\) == 0" "$host"; then
    echo "FAIL: lock:$k is not settable" >&2
    exit 1
  fi
done

if rg -q 'floor\(beat \* 4\)|% 16' "$common"; then
  echo "FAIL: lock_common.h hardcodes a 16-step 1/16 pattern" >&2
  exit 1
fi

# --- per-slot state is cleared with the slot ---------------------------------
# Locks name targets ("synth", "fx1") exactly as the LFOs do, so they must be
# cleared when everything they could point at is unloaded.
if ! rg -n 'memset\(inst->lfos, 0, sizeof\(inst->lfos\)\);' -A12 "$host" | rg -q 'lock_clear_all\(inst\);'; then
  echo "FAIL: locks are not cleared when the slot's modules are unloaded" >&2
  exit 1
fi

# --- lane exhaustion is reported, not silent ---------------------------------
if ! rg -n 'lock_alloc_lane\(st, target, param\)' -A12 "$host" | rg -q 'v2_chain_log'; then
  echo "FAIL: a dropped lock (lanes exhausted) is not logged" >&2
  exit 1
fi

# --- the editor can read what it needs ---------------------------------------
if ! rg -q 'strncmp\(subkey, "at:", 3\) == 0' "$host"; then
  echo "FAIL: get_param cannot report the locks on a held step" >&2
  exit 1
fi
if ! rg -q 'strcmp\(subkey, "step"\) == 0' "$host"; then
  echo "FAIL: get_param cannot report the playhead step" >&2
  exit 1
fi

echo "PASS: chain parameter locks are absolute, non-destructive and explicitly timed"
