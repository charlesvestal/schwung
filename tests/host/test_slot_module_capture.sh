#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

mgmt="src/host/shadow_chain_mgmt.c"
midi="src/host/shadow_midi.c"
types="src/host/shadow_chain_types.h"

# A chain slot captures the UNION of its patch's rules and its synth MODULE's.
#
# Capture used to be parsed from the patch file only, and only when a patch was
# loaded from the library by index. The autosave restore (load_file), a set
# switch and a module swap never opened a patch file, so a restored slot had no
# rules at all — reported from the device: 9W9's step sequencer, and the
# parameter locks built on the same captured steps, were dead after every
# reboot and came back only when the patch was loaded again from the browser.

if ! rg -q 'shadow_capture_rules_t module_capture;' "$types"; then
  echo "FAIL: a slot has no module-level capture rules" >&2
  exit 1
fi

# The focused-slot predicates must consult BOTH sets, or module rules decide
# nothing.
if ! rg -n 'capture_has_note\(&host_chain_slots\[slot\]\.capture, note\)' -A1 "$midi" \
     | rg -q 'capture_has_note\(&host_chain_slots\[slot\]\.module_capture, note\)'; then
  echo "FAIL: shadow_focused_captures_note ignores module rules" >&2
  exit 1
fi
if ! rg -n 'capture_has_cc\(&host_chain_slots\[slot\]\.capture, cc\)' -A1 "$midi" \
     | rg -q 'capture_has_cc\(&host_chain_slots\[slot\]\.module_capture, cc\)'; then
  echo "FAIL: shadow_focused_captures_cc ignores module rules" >&2
  exit 1
fi

# Re-derived on EVERY load path. Each of these is a way a slot comes to hold a
# synth, and each was a way to end up with no rules.
for anchor in 'strcmp\(key_copy, "load_file"\) == 0' 'strcmp\(key_copy, "synth:module"\) == 0' 'shadow_slot_load_capture\(slot, idx\);'; do
  if ! rg -n "$anchor" -A4 "$mgmt" | rg -q 'shadow_slot_load_module_capture\(slot\)'; then
    echo "FAIL: module capture is not refreshed after: $anchor" >&2
    exit 1
  fi
done
# The boot restore — the path that actually bit.
if ! rg -n 'shadow_slot_apply_boot_feedback_hold\(i\);' -A4 "$mgmt" | rg -q 'shadow_slot_load_module_capture\(i\)'; then
  echo "FAIL: module capture is not derived on the boot restore path" >&2
  exit 1
fi

# Unloading clears both sets — a rule outliving its module is the same bug as
# an LFO outliving its target.
n_patch=$(rg -c 'capture_clear\(&shadow_chain_slots\[slot\]\.capture\);' "$mgmt")
n_module=$(rg -c 'capture_clear\(&shadow_chain_slots\[slot\]\.module_capture\);' "$mgmt")
# One patch-rules clear is the re-parse inside shadow_slot_load_capture itself,
# which must NOT clear module rules; every unload site must clear both.
if [ "$n_module" -lt $((n_patch - 1)) ]; then
  echo "FAIL: an unload path clears patch rules but not module rules ($n_patch vs $n_module)" >&2
  exit 1
fi

# Bounded to the capabilities block, as the Master FX path is, so a stray
# "capture" elsewhere in a module.json cannot claim the surface.
if ! rg -n 'void shadow_slot_load_module_capture' -A60 "$mgmt" | rg -q 'strstr\(json, "\\"capabilities\\""\)'; then
  echo "FAIL: module capture is not bounded to capabilities" >&2
  exit 1
fi

# Cached by module id: this runs in the set_param intercept, inside the SPI
# callback, and must not read a file per write.
if ! rg -n 'void shadow_slot_load_module_capture' -A30 "$mgmt" | rg -q 'strcmp\(id, s->module_capture_id\) == 0\) return;'; then
  echo "FAIL: module capture re-reads module.json on every set_param" >&2
  exit 1
fi

echo "PASS: a slot captures its patch's rules and its synth module's, on every load path"
