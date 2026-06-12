#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# 2026-06 cleanup step 10: chain_host.c (6,450 lines post-sweep) split into
# functional units. Pure static-function relocation — no behavior change, no
# exported-symbol change. This pins the structure so it doesn't silently
# re-accrete into one file.

dsp="src/modules/chain/dsp"

# 1. The split files exist and are non-trivial.
declare -A expect_fn=(
  ["chain_json.c"]='int json_get_string\('
  ["chain_params.c"]='int parse_chain_params\('
  ["chain_mod.c"]='void chain_mod_recompute_effective\('
  ["chain_midi.c"]='void v2_on_midi\('
  ["chain_patch.c"]='int v2_parse_patch_file\('
)
for f in chain_json.c chain_params.c chain_mod.c chain_midi.c chain_patch.c chain_internal.h; do
  if [ ! -f "$dsp/$f" ]; then
    echo "FAIL: $dsp/$f missing" >&2
    exit 1
  fi
done

# 2. Representative cluster functions live in their new homes.
for f in "${!expect_fn[@]}"; do
  if ! rg -q "${expect_fn[$f]}" "$dsp/$f"; then
    echo "FAIL: $dsp/$f does not define expected cluster fn (${expect_fn[$f]})" >&2
    exit 1
  fi
done

# 3. chain_host.c keeps only lifecycle/params-entry/render/entry (< 2900 lines).
lines=$(wc -l < "$dsp/chain_host.c")
if [ "$lines" -ge 2900 ]; then
  echo "FAIL: chain_host.c is $lines lines — split regressed (expected < 2900)" >&2
  exit 1
fi

# 4. build.sh compiles every split unit into the chain DSP.
for f in chain_host.c chain_json.c chain_params.c chain_mod.c chain_midi.c chain_patch.c; do
  if ! rg -q "$dsp/$f" scripts/build.sh; then
    echo "FAIL: scripts/build.sh does not compile $dsp/$f" >&2
    exit 1
  fi
done

# 5. Exported-symbol invariant: dsp.so must export exactly the pre-split set
#    (5 chain entry points + 6 unified_log fns). Cross-TU internals must be
#    hidden-visibility so dlopen'd sub-plugins can't collide with them.
so="build/modules/chain/dsp.so"
if [ -f "$so" ] && command -v nm >/dev/null 2>&1; then
  got=$(nm -D --defined-only "$so" 2>/dev/null | awk '{print $NF}' | sort)
  want=$(printf '%s\n' \
    chain_fx_requires_continuous chain_process_fx chain_set_external_fx_mode \
    chain_set_inject_audio move_plugin_init_v2 \
    unified_log unified_log_crash unified_log_enabled unified_log_init \
    unified_log_shutdown unified_log_v | sort)
  if [ "$got" != "$want" ]; then
    echo "FAIL: dsp.so exported symbols changed:" >&2
    diff <(echo "$want") <(echo "$got") >&2 || true
    exit 1
  fi
else
  echo "note: $so absent or nm unavailable — symbol check skipped (build to enable)"
fi

echo "PASS: chain_host split into functional units, symbol surface unchanged"
