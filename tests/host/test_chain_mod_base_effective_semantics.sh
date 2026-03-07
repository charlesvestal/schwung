#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q 'chain_mod_update_base_from_set_param' "$file"; then
  echo "FAIL: missing base update hook for set_param path" >&2
  exit 1
fi
if ! rg -q 'chain_mod_apply_effective_value' "$file"; then
  echo "FAIL: missing effective value apply helper" >&2
  exit 1
fi
if ! rg -Fq 'if (chain_mod_is_target_active' "$file"; then
  echo "FAIL: set/get flow missing mod-target active guard" >&2
  exit 1
fi
if ! rg -q 'chain_mod_get_base_for_subkey' "$file"; then
  echo "FAIL: missing base-value getter helper for UI edit flows" >&2
  exit 1
fi

echo "PASS: base/effective modulation semantics hooks are present"
