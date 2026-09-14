#!/usr/bin/env bash
# The WIRING of the sound-generator keep-alive — the opt-out from the shim's
# silence-skip.
#
# The shim parks a slot after ~1 s of silent OUTPUT and thereafter renders one
# frame in 172, waking only on non-silent probe output, on a fade ramp, or on
# MIDI. A generator whose output is the LINE INPUT has none of those levers:
# the host never inspects the jack and the module takes no MIDI. Parked, it
# drops up to ~0.5 s off the front of every phrase and swallows anything under
# DSP_SILENCE_LEVEL — heard as a noise gate that no module setting turns off,
# which is exactly how this was reported against the Line In module.
#
# Pinned here and not in a C unit because the parts that can go wrong are the
# JOINS: a capability nobody reads, an export nobody resolves, or a check that
# sits on the wrong side of the idle gate. Same shape, and same reasoning, as
# tests/host/test_idle_midi_tick_wake.sh.
set -euo pipefail

cd "$(dirname "$0")/../.."

HOST=src/modules/chain/dsp/chain_host.c
MGMT=src/host/shadow_chain_mgmt.c
MGMTH=src/host/shadow_chain_mgmt.h
SHIM=src/schwung_shim.c
LINEIN=src/modules/sound_generators/linein/module.json

fail() { echo "FAIL: $1"; exit 1; }

# 1. The synth loader reads the capability at all. The audio FX loader has read
#    it since loopers needed it; a generator that declares it and is still
#    parked is the failure this pins.
awk '
  /^int v2_load_synth/ {fn=1}
  fn && /"requires_continuous_processing"/ {found=1}
  fn && /^}/ {exit}
  END {exit found ? 0 : 1}
' "$HOST" || fail "v2_load_synth never reads capabilities.requires_continuous_processing"

# 2. A line-input consumer gets the keep-alive WITHOUT declaring it. This is
#    the part that makes the fix general: any module that pulls the jack in has
#    no wake signal the shim can see, whatever its module.json says.
awk '
  /inst->synth_consumes_line_input = 1;/ {branch=1}
  branch && /inst->synth_requires_continuous = 1;/ {found=1; exit}
  branch && /^                        \}/ {exit}
  END {exit found ? 0 : 1}
' "$HOST" || fail "a line-input consumer is not implicitly kept alive"

# 3. Reset on unload, so a keep-alive module's flag cannot outlive it and hold
#    the NEXT module in the slot permanently awake.
awk '
  /^void v2_unload_synth/ {fn=1}
  fn && /inst->synth_requires_continuous = 0;/ {found=1}
  fn && /^}/ {exit}
  END {exit found ? 0 : 1}
' "$HOST" || fail "the keep-alive flag survives a synth unload"

# 4. The export the shim resolves by dlsym, and the loader that resolves it.
grep -q 'int chain_synth_requires_continuous(void \*instance)' "$HOST" \
  || fail "chain DSP exports no synth keep-alive result"
grep -q 'dlsym(shadow_dsp_handle, "chain_synth_requires_continuous")' "$MGMT" \
  || fail "the shim loader does not resolve the synth keep-alive export"
grep -q 'extern int (\*shadow_chain_synth_requires_continuous)(void \*instance);' "$MGMTH" \
  || fail "the synth keep-alive pointer is not declared for the shim"

# 5. It must be NULL-CHECKED at the call site: the pointer is NULL against any
#    chain DSP built before the export, and calling through it is a crash on
#    the SPI callback rather than a missing feature.
grep -q 'shadow_chain_synth_requires_continuous &&' "$SHIM" \
  || fail "the shim calls the keep-alive export without null-checking it"

# 6. ORDER, in the shim: the keep-alive must be decided BEFORE the idle gate's
#    bail-out, or a slot already parked stays parked for up to a probe interval
#    after the module loads — the audible bug, just shorter.
awk '
  /int synth_keep_alive = / {ka=NR}
  ka && /goto slot_run_deferred_fx;/ {bail=NR; exit}
  END {exit (ka && bail && ka < bail) ? 0 : 1}
' "$SHIM" || fail "the keep-alive is decided after the idle gate, not before it"

# 7. And the silence accumulator must not re-park it. Clearing idle once at the
#    top is not enough: the counter downstream runs on every rendered block, so
#    a keep-alive slot with silent output would climb back to the threshold and
#    park on the very next second.
awk '
  /if \(synth_keep_alive\) \{/ {ka=1}
  ka && /\} else if \(is_silent\) \{/ {found=1; exit}
  END {exit found ? 0 : 1}
' "$SHIM" || fail "a keep-alive slot can still be parked by the silence counter"

# 8. The module that prompted this declares it, so the intent is readable at
#    the module rather than only inferred by the loader.
grep -q '"requires_continuous_processing": true' "$LINEIN" \
  || fail "the Line In module does not declare requires_continuous_processing"

echo "PASS: line-input generators opt out of the shim's silence-skip"
