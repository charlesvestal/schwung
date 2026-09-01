#!/usr/bin/env bash
# Source pin: "Keep Schwung" (Global Settings -> Display).
#
# On, a plain Track tap while the shadow UI is up switches to that slot instead
# of handing the screen back to Move. Three things have to hold together, and
# each of them is silent when it breaks:
#
#   1. The tap-dismiss in the long-press release branch must be guarded, or the
#      jump happens and the dismiss undoes it a moment later — which looks like
#      the setting doing nothing at all.
#   2. The jump must fire on the PRESS, OUTSIDE the LONG_PRESS_ACTIVE() block.
#      That whole block is gated on the shadow-UI trigger mode, so with the
#      trigger set to Shift+Vol a tap would otherwise reach no branch and the
#      setting would work for some users and not others.
#   3. The setting must survive a deploy. install.sh REWRITES features.json
#      from a fixed key list, so a key it does not carry over is reset to its
#      default on every install.
#   4. It must be declared as a BOOL. A two-option enum has the same click
#      behaviour and the same meta, and still draws as the ENUM SQUARE -- the
#      "there is a list behind this" widget -- because detectSwitch picks the
#      switch pill on BOOL_OPTION words, not on the option count. That is what
#      shipped in round one and was reported as "why is the setting a menu,
#      unlike display mirroring?".
#
# The struct byte itself is checked by the size assertion in shadow_constants.h
# (any compile catches a change), so what is pinned here is that schwung-manager
# reads it at the offset the C struct puts it at. Those are two hand-written
# numbers in two languages with nothing but this test between them, and past the
# end of a stale short segment a wrong one is a SIGBUS, not a wrong answer.
set -u

ROOT="$(dirname "$0")/../.."
SHIM="$ROOT/src/schwung_shim.c"
HDR="$ROOT/src/host/shadow_constants.h"
INSTALL="$ROOT/scripts/install.sh"
GO="$ROOT/schwung-manager/shmconfig.go"

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

for f in "$SHIM" "$HDR" "$INSTALL" "$GO"; do
    [ -f "$f" ] || { echo "FAIL: cannot find $f" >&2; exit 1; }
done

# --- 1. the byte, and where it sits -----------------------------------------
grep -q 'volatile uint8_t stay_in_shadow;' "$HDR" ||
    fail "shadow_control_t has no stay_in_shadow byte"

# The manager reads the same byte by raw offset. COMPUTE the offset from the C
# struct rather than transcribing it: a number kept in step by hand in two
# languages is how they drift.
# NOT mktemp -t: BSD takes a bare prefix, GNU demands X's and errors out on one
# ("too few X's in template"), which CI caught as a probe that could not build.
probe="${TMPDIR:-/tmp}/stay_off_$$"
cat > "$probe.c" <<'CEOF'
#include <stdio.h>
#include <stddef.h>
#include "src/host/shadow_constants.h"
int main(void) { printf("%zu\n", offsetof(shadow_control_t, stay_in_shadow)); return 0; }
CEOF
c_off=$( (cd "$ROOT" && cc -I. -o "$probe" "$probe.c" 2>/dev/null) && "$probe" )
rm -f "$probe" "$probe.c"
if [ -z "$c_off" ]; then
    fail "could not compile the offsetof probe — does shadow_constants.h still declare stay_in_shadow?"
else
    go_off=$(grep -oE 'offStayInShadow +=[[:space:]]*[0-9]+' "$GO" | grep -oE '[0-9]+$')
    [ "$go_off" = "$c_off" ] ||
        fail "schwung-manager reads stay_in_shadow at offset ${go_off:-none}, the C struct puts it at $c_off"
fi

# --- 2. the shim honours it --------------------------------------------------
grep -q '#define STAY_IN_SHADOW()' "$SHIM" ||
    fail "the shim has no STAY_IN_SHADOW() accessor"
grep -q 'shadow_control->stay_in_shadow != 0 : stay_in_shadow_setting' "$SHIM" ||
    fail "STAY_IN_SHADOW() must read the LIVE SHM byte, so the toggle takes effect without a restart"
grep -q '"\\"stay_in_shadow\\""' "$SHIM" ||
    fail "load_feature_config never parses stay_in_shadow out of features.json"
grep -q 'shadow_control->stay_in_shadow = stay_in_shadow_setting' "$SHIM" ||
    fail "the boot-time setting is never published to shared memory"

# The tap-dismiss branch — the one that logs "Track tap: dismissing shadow UI".
dismiss_guard=$(awk '/Track tap: dismissing shadow UI/ { found = 1 }
                     { buf[NR] = $0 }
                     END {
                       for (i = 1; i <= NR; i++)
                         if (buf[i] ~ /Track tap: dismissing shadow UI/) {
                           for (j = i - 12; j < i; j++) print buf[j];
                           exit
                         }
                     }' "$SHIM")
grep -q '!STAY_IN_SHADOW()' <<<"$dismiss_guard" ||
    fail "the Track-tap dismiss is not guarded by !STAY_IN_SHADOW() — the slot switch would be undone by the dismiss that follows it"

# The jump must be raised outside the long-press block. Both live inside the
# same Track-CC handler, so compare line numbers: the jump has to come BEFORE
# the "Long-press detection for Track buttons" comment that opens that block.
jump_line=$(grep -n 'STAY_IN_SHADOW() && shadow_display_mode' "$SHIM" | head -1 | cut -d: -f1)
lp_line=$(grep -n 'Long-press detection for Track buttons' "$SHIM" | head -1 | cut -d: -f1)
if [ -z "$jump_line" ] || [ -z "$lp_line" ]; then
    fail "cannot locate the stay-in-Schwung jump or the long-press block — has the Track handler moved?"
elif [ "$jump_line" -gt "$lp_line" ]; then
    fail "the stay-in-Schwung jump (line $jump_line) sits inside the long-press block (opens at $lp_line), which is gated on the shadow-UI trigger mode — a tap would do nothing in Shift+Vol mode"
fi

# It raises the same hand-off Shift+Vol+Track uses, rather than inventing one.
jump_body=$(awk -v n="${jump_line:-0}" 'NR >= n && NR <= n + 6' "$SHIM")
grep -q 'SHADOW_UI_FLAG_JUMP_TO_SLOT' <<<"$jump_body" ||
    fail "the stay-in-Schwung branch does not raise SHADOW_UI_FLAG_JUMP_TO_SLOT"
grep -q 'ui_slot' <<<"$jump_body" ||
    fail "the stay-in-Schwung branch does not tell the UI WHICH slot to open"

# Shift+Track is the escape hatch and must not be swallowed by the new branch.
grep -q '!shadow_shift_held' <<<"$jump_body" ||
    fail "the stay-in-Schwung branch fires with Shift held, closing the Shift+Track way out"

# --- 3. it draws as a SWITCH, not an enum square -----------------------------
(cd "$ROOT" && node -e '
import("./src/shadow/shadow_ui_global_grid.mjs").then(async (G) => {
  const V = await import("./src/shared/param_pages/viz.mjs");
  const M = await import("./src/shared/param_pages/param_meta.mjs");
  const c = G.buildGlobalSettingsContract();
  const idx = M.buildMetaIndex({ hierarchy: c.hierarchy, chainParams: c.chainParams });
  const meta = idx.getOrGuess("stay_in_shadow");
  if (!V.isBooleanMeta(meta)) {
    console.error("FAIL: stay_in_shadow is not a boolean to viz.detectSwitch -- options "
      + JSON.stringify(meta.options) + " are not BOOL_OPTION words, so the cell draws as "
      + "the enum square (a menu) rather than the switch beside it");
    process.exit(1);
  }
  console.error("ok: stay_in_shadow draws as a switch");
}).catch((e) => { console.error("FAIL: " + (e && e.stack || e)); process.exit(1); });
') || fails=$((fails + 1))

# --- 4. it survives a deploy -------------------------------------------------
grep -q 'get_existing_feature "stay_in_shadow"' "$INSTALL" ||
    fail "install.sh does not preserve stay_in_shadow — it rewrites features.json, so the setting resets on every deploy"
grep -q 'stay_in_shadow\\": \$existing_stay_in_shadow' "$INSTALL" ||
    fail "install.sh reads the existing stay_in_shadow but never writes it back"

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed" >&2
    exit 1
fi
echo "PASS: Keep Schwung — a bool so it draws as a SWITCH, live SHM byte at the C struct's offset, jump raised on the press outside the long-press gate, dismiss guarded, Shift+Track still exits, preserved across installs"
