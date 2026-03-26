#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'const configJson = shadow_get_param\(0, "master_fx:lfo" \+ li \+ ":config"\);' "$file"; then
  echo "FAIL: save flow does not read Master FX LFO config directly from DSP" >&2
  exit 1
fi

if ! rg -q 'stateFile\.lfos = masterFxLfoConfig;' "$file"; then
  echo "FAIL: save flow does not persist Master FX LFO config into per-set master_fx_0.json" >&2
  exit 1
fi

if ! rg -q 'JSON\.stringify\(\{ lfos: masterFxLfoConfig \}, null, 2\) \+ "\\n"' "$file"; then
  echo "FAIL: empty fx1 slot save flow does not retain per-set Master FX LFO config" >&2
  exit 1
fi

if ! rg -q 'if \(i === 0 && stateFile\.lfos && typeof shadow_set_param === "function"\)' "$file"; then
  echo "FAIL: startup load flow does not restore Master FX LFOs from per-set state file" >&2
  exit 1
fi

if ! rg -q 'if \(mfxi === 0 && mfxData\.lfos && typeof shadow_set_param === "function"\)' "$file"; then
  echo "FAIL: set-change flow does not restore Master FX LFOs from per-set state file" >&2
  exit 1
fi

if ! rg -q 'shadow_set_param\(0, pfx \+ "polarity", String\(lfoConfig\.polarity \|\| 0\)\);' "$file"; then
  echo "FAIL: per-set load flow does not restore Master FX LFO polarity" >&2
  exit 1
fi

if rg -q 'config\.master_fx_chain\["lfo" \+ li\]' "$file"; then
  echo "FAIL: Master FX LFOs are still persisted in shadow_config master_fx_chain" >&2
  exit 1
fi

echo "PASS: Master FX LFO persistence follows per-set master_fx_0.json flow"
