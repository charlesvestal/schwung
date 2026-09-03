#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A GATED BUILD IS NOT A GATED MODULE.
#
# build.sh compiles the test fixtures only under SCHWUNG_BUILD_TEST_MODULES --
# but the generic "Copying module files" loop copies every module.json, .js and
# .mjs it finds under src/modules, regardless. So a fixture whose .so is gated
# still shipped its module.json, with NO .so beside it.
#
# That is not a cosmetic leak. widget-test and gesture-test both declare
# chainable audio_fx, so they appear in the FX picker, and a chain slot
# referencing a module that cannot load is restored on EVERY BOOT -- the shape
# CLAUDE.md records for the breakbeat boot loop. gesture-test had been shipping
# that way already; widget-test would have joined it.
#
# The fix is a scrub under the same gate, which build.sh already does for
# sysex-test. This pins that every fixture is BOTH gated at build AND scrubbed,
# because having only the first is what made the leak invisible.
#
# Source-level, deliberately: the alternative is two full Docker builds per run.
# The real artifact check happens in the release flow; this catches the edit
# that drops a fixture from the scrub list.

fail=0
ok() { if [ "$1" = "1" ]; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }

BUILD=scripts/build.sh

# Every module that is compiled only under the test-modules gate...
gated=$(awk '/SCHWUNG_BUILD_TEST_MODULES/,0' "$BUILD" |
        grep -oE 'build/modules/[a-z_]+/[a-z0-9-]+/' | sed 's:/$::' | sort -u)

ok "$([ -n "$gated" ] && echo 1 || echo 0)" "found gated fixture paths in build.sh"

# ...must also be scrubbed when the gate is OFF.
scrub_block=$(awk '/if \[ -z "\$\{SCHWUNG_BUILD_TEST_MODULES:-\}" \]/,/fi/' "$BUILD")

for m in widget-test gesture-test; do
  compiled=$(grep -c "build/modules/audio_fx/${m}/" "$BUILD" || true)
  ok "$([ "$compiled" -gt 0 ] && echo 1 || echo 0)" "$m is built by build.sh at all"

  in_gate=$(awk "/SCHWUNG_BUILD_TEST_MODULES/,0" "$BUILD" | grep -c "audio_fx/${m}/" || true)
  ok "$([ "$in_gate" -gt 0 ] && echo 1 || echo 0)" "$m is built only under the test-modules gate"

  scrubbed=$(printf '%s' "$scrub_block" | grep -c "build/modules/audio_fx/${m}" || true)
  ok "$([ "$scrubbed" -gt 0 ] && echo 1 || echo 0)" \
     "$m is SCRUBBED when the gate is off (a gated build is not a gated module)"
done

# The POC must be a real module, not a module.json with no DSP -- that is the
# combination that wedges a chain slot across reboots.
ok "$([ -f src/modules/audio_fx/widget-test/widget_test.c ] && echo 1 || echo 0)" \
   "widget-test ships a real DSP source rather than a bare module.json"

# And it must not reappear under tools/, where the chain host could never load
# it (build.sh: the FX path is audio_fx/<id>/<id>.so).
ok "$([ ! -d src/modules/tools/widget-test ] && echo 1 || echo 0)" \
   "widget-test does not linger under tools/, where a chain FX cannot load"

exit "$fail"
