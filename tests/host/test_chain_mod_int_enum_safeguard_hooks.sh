#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q '#define MOD_INT_ENUM_MIN_INTERVAL_MS 50' "$file"; then
  echo "FAIL: missing global int/enum modulation min-interval constant" >&2
  exit 1
fi
if ! rg -q 'static void chain_mod_apply_effective_value\(chain_instance_t \*inst, mod_target_state_t \*entry, int force_write\)' "$file"; then
  echo "FAIL: chain_mod_apply_effective_value force-write signature missing" >&2
  exit 1
fi
if ! rg -q 'if \(entry->type == KNOB_TYPE_INT \|\| entry->type == KNOB_TYPE_ENUM\)' "$file"; then
  echo "FAIL: missing generic int/enum modulation throttle guard" >&2
  exit 1
fi
if ! rg -q 'entry->last_applied_ms > 0 && \(now_ms - entry->last_applied_ms\) < min_interval_ms' "$file"; then
  echo "FAIL: missing time-based modulation throttle check" >&2
  exit 1
fi
if rg -q 'strcmp\(inst->current_synth_module, "minijv"\) == 0' "$file"; then
  echo "FAIL: modulation throttle must not be Mini-JV specific" >&2
  exit 1
fi
if ! rg -q 'if \(\(int\)entry->effective_value == \(int\)entry->last_applied_value\)' "$file"; then
  echo "FAIL: integer modulation dedupe guard missing" >&2
  exit 1
fi
if ! rg -q 'chain_mod_apply_effective_value\(inst, entry, 1\)' "$file"; then
  echo "FAIL: force-write restore path missing on modulation clear" >&2
  exit 1
fi

echo "PASS: chain modulation includes global int/enum flood safeguards"
