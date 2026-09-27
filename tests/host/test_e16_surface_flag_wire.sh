#!/usr/bin/env bash
# The setting and the shim's gate are in different processes, and nothing in
# either half fails when the wire between them is missing.
#
# Task 6 added `shadow_control_t.external_surface` and the shim reads it. Task 7
# added the Global Settings row and persisted it to shadow_config.json. Both
# passed their own tests while the flag was never written: the setting said E16,
# the shim saw 0, and no encoder reached JS. Each agent was scoped to its own
# files, so neither could see the seam.
#
# This pins all three parts of the wire, and the RESTATE specifically: /dev/shm
# does not survive a shim restart, so a memoised mirror latches at 0 and the
# surface dies silently. The binding is idempotent against the SHM to make a
# per-tick restate cost a byte compare -- the same rule pad_block carries.
set -euo pipefail
cd "$(dirname "$0")/../.."

fail=0
note() { echo "FAIL: $1"; fail=1; }

grep -q "shadow_control->external_surface = (uint8_t)val;" src/shadow/shadow_ui.c \
  || note "no C binding writes shadow_control->external_surface"

grep -q 'JS_SetPropertyStr(ctx, global_obj, "host_external_surface"' src/shadow/shadow_ui.c \
  || note "the binding is never registered, so JS cannot call it"

# Idempotence is what licenses the per-tick restate. Without the early return a
# restate would log on every frame from the shadow UI process.
grep -q "if (shadow_control->external_surface == (uint8_t)val) return JS_TRUE;" src/shadow/shadow_ui.c \
  || note "the binding is not idempotent against the SHM"

grep -q "^function reconcileExternalSurface() {" src/shadow/shadow_ui.js \
  || note "no reconciler restates the flag"

# It must be CALLED, not merely defined -- a defined-but-uncalled reconciler is
# exactly the shape this whole test exists to catch.
grep -q "^    reconcileExternalSurface();" src/shadow/shadow_ui.js \
  || note "reconcileExternalSurface is defined but never called from the tick"

if [ "$fail" -eq 0 ]; then
  echo "PASS: the surface setting reaches the shim, and is restated rather than mirrored"
else
  exit 1
fi
