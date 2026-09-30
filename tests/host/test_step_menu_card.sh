#!/usr/bin/env bash
# The step menu card (src/shared/step_menu_card.mjs) and its wiring.
#
#  - The JS condition list must be the C one (src/host/step_chance.h): the
#    card names the index the chain stores, and a drifted ladder shows "50%"
#    over a step that plays at 41%.
#  - Drawn, a % bar is SOLID with height = percent, an A:B bar is HOLLOW, an
#    empty step is one baseline mark, and the blit rect stays on the screen.
#  - shadow_ui.js draws it over Move's screen only, and the autosave/restore
#    carry chance_<i>.txt on every path that carries lanes_<i>.json.
set -e
cd "$(dirname "$0")/../.."
fail() { echo "FAIL: $1"; exit 1; }

c_ladder=$(sed -n '/sc_percent_ladder\[SC_N_PERCENT\] = {/,/};/p' src/host/step_chance.h | tr -d '\n' | sed 's/.*{//; s/}.*//' | tr -dc '0-9,')
js_ladder=$(grep -o 'PERCENT_LADDER = \[[^]]*\]' src/shared/step_menu_card.mjs | tr -dc '0-9,')
[ -n "$c_ladder" ] || fail "cannot read the C ladder"
[ "$c_ladder" = "$js_ladder" ] || fail "JS percent ladder ($js_ladder) differs from step_chance.h ($c_ladder)"
grep -q '#define SC_MAX_B 8' src/host/step_chance.h || fail "SC_MAX_B changed; update MAX_B in step_menu_card.mjs"
grep -q 'const MAX_B = 8;' src/shared/step_menu_card.mjs || fail "MAX_B drifted from step_chance.h"

node --input-type=module -e "
import { createFramebuffer, drawContext } from '$PWD/tools/param-pages/harness.mjs';
import { drawStepMenuCard, condName, condCount } from '$PWD/src/shared/step_menu_card.mjs';
const fail = (m) => { console.log('FAIL: ' + m); process.exit(1); };
if (condCount() !== 57) fail('condCount ' + condCount());
if (condName(0) !== '100%' || condName(11) !== '50%' || condName(22) !== '1:2' || condName(56) !== '8:8') fail('names');
const page = [0, 255, 11, 22, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254];
const fb = createFramebuffer(); const c = drawContext(fb);
const g = drawStepMenuCard(c, { field: 0, step: 2, cond: 11, vel: 100, lenC: 100, page });
const b = g.blit;
if (b.x < 0 || b.y < 0 || b.x + b.w > 128 || b.y + b.h > 64) fail('blit off screen ' + JSON.stringify(b));
/* find the bar row: the column of pixels of bar 0 (100%) */
const px = (x, y) => fb.getPixel ? fb.getPixel(x, y) : fb.pixels[y * 128 + x];
let col0 = -1, top0 = -1, bot0 = -1;
/* inside the frame, below the band and the value line (3 + 9 + 1 + 10) */
const barTop = g.y + 23;
for (let x = g.x + 3; x < g.x + g.w - 3 && col0 < 0; x++) {
  let run = 0, start = -1;
  for (let y = barTop; y < g.y + g.h - 3; y++) { if (px(x, y)) { if (!run) start = y; run++; if (run >= 14) { col0 = x; top0 = start; bot0 = y; break; } } else run = 0; }
}
if (col0 < 0) fail('no 14px solid bar found for the 100% step');
const pitch = 7;
/* bar 2 (50%): 7 px tall, solid */
let h2 = 0; for (let y = top0; y <= bot0; y++) if (px(col0 + 2 * pitch + 2, y)) h2++;
if (h2 !== 7) fail('50% bar height ' + h2 + ' (want 7)');
/* bar 3 (1:2): hollow -- its centre column is lit only at top and bottom */
let mid = 0; for (let y = top0 + 1; y < bot0; y++) if (px(col0 + 3 * pitch + 2, y)) mid++;
if (mid !== 0) fail('A:B bar is not hollow');
let edge = 0; for (let y = top0; y <= bot0; y++) if (px(col0 + 3 * pitch, y)) edge++;
if (edge !== 14) fail('A:B bar outline height ' + edge);
/* bar 1 (empty): only the baseline */
let e1 = 0; for (let y = top0; y < bot0; y++) if (px(col0 + pitch + 2, y)) e1++;
if (e1 !== 0 || !px(col0 + pitch + 2, bot0)) fail('empty step is not a lone baseline mark');
/* bar 4 (off): nothing */
let o4 = 0; for (let y = top0; y <= bot0; y++) for (let x = 0; x < 6; x++) if (px(col0 + 4 * pitch + x, y)) o4++;
if (o4 !== 0) fail('an OFF cell drew something');
/* held-step marker under bar 2 */
if (!px(col0 + 2 * pitch + 2, bot0 + 2)) fail('no marker under the held step');
if (px(col0 + 1 * pitch + 2, bot0 + 2)) fail('marker under the wrong step');
/* Move-only: Chance says so, and draws no bars */
const fb2 = createFramebuffer(); const c2 = drawContext(fb2);
const g2 = drawStepMenuCard(c2, { field: 0, step: 2, cond: 11, vel: 100, lenC: 100, page, flags: 1, track: 1 });
const px2 = (x, y) => fb2.pixels[y * 128 + x];
let solid = 0;
for (let x = g2.x + 3; x < g2.x + g2.w - 3; x++) { let run = 0; for (let y = g2.y + 23; y < g2.y + g2.h - 3; y++) { run = px2(x, y) ? run + 1 : 0; if (run >= 14) solid++; } }
if (solid) fail('a Move-only card still draws bars');
const { fieldValue } = await import('$PWD/src/shared/step_menu_card.mjs');
if (fieldValue({ field: 0, flags: 1, cond: 11 }) !== 'Move only') fail('Move-only chance value');
if (fieldValue({ field: 1, flags: 1, cond: 11, lenC: 120 }) !== '1.2') fail('Length must still work on a Move-only track');
/* a chord is a range, as Move prints it */
if (fieldValue({ field: 1, cond: 0, lenC: 200, lenMaxC: 1600 }) !== '2.0-16.0') fail('length range: ' + fieldValue({ field: 1, cond: 0, lenC: 200, lenMaxC: 1600 }));
if (fieldValue({ field: 1, cond: 0, lenC: 120, lenMaxC: 120 }) !== '1.2') fail('single length');
if (fieldValue({ field: 2, cond: 0, vel: 90, velMax: 127 }) !== '90-127') fail('velocity range');
if (fieldValue({ field: 2, cond: 0, vel: 100 }) !== '100') fail('single velocity (no max field: an older shim)');
console.log('PASS: card draws the page');
" || exit 1

ui=src/shadow/shadow_ui.js
grep -q "if (stepMenu && shadowDisplayHidden())" "$ui" || fail "the card is not gated on Move's screen"
[ "$(grep -c 'persistSlotChance(i);' "$ui")" -ge 1 ] || fail "the autosave never writes chance_<i>.txt"
[ "$(grep -c 'restoreSlotLanes(i); } catch' "$ui")" = "$(grep -c 'restoreSlotChance(i); } catch' "$ui")" ] \
  || fail "a restore path restores lanes but not chance"
echo "PASS: step menu card + wiring"
