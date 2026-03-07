#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q 'static int parse_chain_params_array_json\(' "$file"; then
  echo "FAIL: missing runtime chain_params array parser" >&2
  exit 1
fi
if ! rg -q 'static int chain_mod_refresh_target_param_cache\(' "$file"; then
  echo "FAIL: missing runtime target param cache refresh helper" >&2
  exit 1
fi
if ! rg -q 'chain_mod_refresh_target_param_cache\(inst, target\)' "$file"; then
  echo "FAIL: find_param_by_key does not retry through runtime cache refresh" >&2
  exit 1
fi
if ! rg -q 'static void chain_mod_clear_target_entry\(' "$file"; then
  echo "FAIL: missing targeted modulation clear helper for stale mappings" >&2
  exit 1
fi
if ! rg -Fq 'if (!pinfo) {' "$file" || ! rg -q 'chain_mod_clear_target_entry\(inst, stale, 0\);' "$file"; then
  echo "FAIL: missing stale-target cleanup when modulation metadata lookup fails" >&2
  exit 1
fi
if ! rg -q 'strcmp\(subkey, "plugin_id"\) == 0' "$file"; then
  echo "FAIL: missing plugin_id cache invalidation hook for dynamic FX params" >&2
  exit 1
fi

echo "PASS: dynamic param lookup and stale-target modulation cleanup hooks are present"
