#!/usr/bin/env bash
set -euo pipefail

file="src/modules/chain/dsp/chain_host.c"

if ! rg -q 'typedef struct mod_source_contribution' "$file"; then
  echo "FAIL: missing per-source modulation contribution struct" >&2
  exit 1
fi
if ! rg -q 'sources\[MAX_MOD_SOURCES_PER_TARGET\]' "$file"; then
  echo "FAIL: modulation target state does not store per-source contributions" >&2
  exit 1
fi
if ! rg -q 'static float chain_mod_sum_contributions\(' "$file"; then
  echo "FAIL: missing contribution summing helper" >&2
  exit 1
fi
if ! rg -q 'chain_mod_find_or_alloc_source_contribution\(' "$file"; then
  echo "FAIL: missing source allocation path for per-source contributions" >&2
  exit 1
fi
if ! rg -q 'chain_mod_remove_source_contribution\(' "$file"; then
  echo "FAIL: missing per-source contribution removal helper" >&2
  exit 1
fi
if ! rg -q 'source_entry->contribution =' "$file"; then
  echo "FAIL: modulation emit path is not writing per-source contribution values" >&2
  exit 1
fi

echo "PASS: modulation runtime supports additive per-source contribution summing"
