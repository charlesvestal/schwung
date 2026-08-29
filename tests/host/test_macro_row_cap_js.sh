#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The macro row cap has two independent copies, same failure mode as
# MASTER_FX_SLOTS (test_master_fx_slots_js.sh): src/modules/chain/dsp/
# chain_internal.h owns the real array (knob_mapping_t.macro_targets), and
# src/shadow/shadow_ui.js holds a MIRROR because the Macro Editor only ever
# addresses rows by "macro_N_row_R:" key -- it never reads the cap off the
# wire. Raise MAX_MACRO_TARGETS on the C side without raising
# MACRO_MAX_TARGETS here and the DSP fans a knob out to more targets than the
# editor can ever show or edit, with no error anywhere.

HDR="src/modules/chain/dsp/chain_internal.h"
JS="src/shadow/shadow_ui.js"

hdr_cap=$(awk '/^#define MAX_MACRO_TARGETS /{print $3}' "$HDR")
js_cap=$(awk '/^const MACRO_MAX_TARGETS = /{gsub(/[^0-9]/, "", $4); print $4}' "$JS")

[ -n "$hdr_cap" ] || { echo "FAIL: could not read MAX_MACRO_TARGETS from $HDR" >&2; exit 1; }
[ -n "$js_cap" ] || { echo "FAIL: could not read MACRO_MAX_TARGETS from $JS" >&2; exit 1; }

[ "$hdr_cap" = "$js_cap" ] || {
    echo "FAIL: cap drift: $HDR says MAX_MACRO_TARGETS=$hdr_cap but $JS says MACRO_MAX_TARGETS=$js_cap" >&2
    exit 1
}

# The editor's own item count must be derived from the cap (one row per
# target -- target and amount share a row, and there is no separate Name
# row), not a restated literal that a cap raise would silently leave behind.
command grep -q 'return MACRO_MAX_TARGETS;' "$JS" \
    || { echo "FAIL: macroEditorItemCount() no longer derives its length from MACRO_MAX_TARGETS" >&2; exit 1; }

echo "PASS: macro row cap agrees between chain_internal.h and shadow_ui.js ($hdr_cap)"
