#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# AN UNKNOWN CUSTOM WIDGET FALLS THROUGH TO THE DETECTOR.
#
# collectDeclared claims keys as it walks, so a custom kind that is not
# registered simply does not claim -- its keys stay in the detector pool and the
# built-in widget draws. That ONE behaviour covers four cases at once:
#
#   - an author typo in the kind name
#   - a widget whose canvas.js failed to load
#   - an OLDER HOST reading a NEWER module, which has never heard of the name
#   - a widget disabled after throwing (see test_widget_one_strike.sh)
#
# They share a code path, so the forward-compatibility behaviour cannot rot
# separately from the typo behaviour.
#
# The assertion that matters is the DRAWN RESULT -- that the detector group is
# actually present -- not merely that nothing threw. A test that only checked
# for the absence of a custom group would pass if resolution produced nothing
# at all, which is the failure mode being guarded against.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the custom viz tests" >&2
  exit 1
fi

node --input-type=module -e '
import { resolveViz, VIZ_ENVELOPE } from "./src/shared/param_pages/viz.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { isCustomKind, registerWidget, clearWidgets, isWidgetAvailable }
  from "./src/shared/param_pages/widget_registry.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const idx = (chainParams, keys) => buildMetaIndex({
  hierarchy: { modes: null, levels: { root: {
    label: "T", knobs: keys.filter(Boolean),
    params: chainParams.map((p) => ({ key: p.key })) } } },
  chainParams,
});

ok(isCustomKind("custom:mymeter"), "custom: prefix is recognised");
ok(!isCustomKind("envelope"), "a built-in kind is not a custom kind");
ok(!isCustomKind(null), "a null kind is not a custom kind");
ok(!isCustomKind(""), "an empty kind is not a custom kind");

/* Registration is refused for anything that is not drawable. */
clearWidgets();
ok(!registerWidget("mymeter", { draw: () => {} }),
   "a kind without the custom: prefix cannot be registered");
ok(!registerWidget("custom:x", {}), "a widget with no draw function is refused");
ok(registerWidget("custom:x", { draw: () => {} }), "a well-formed widget registers");

/* A registered single-param custom widget claims its key. */
clearWidgets();
registerWidget("custom:mymeter", { draw: () => {} });
let cp = [{ key: "drive", name: "Drive", type: "float", min: 0, max: 1,
            viz: { kind: "custom:mymeter" } }];
let keys = ["drive", null, null, null, null, null, null, null];
let r = resolveViz({ keys, metaIndex: idx(cp, keys) });
ok(r.groups.length === 1 && r.groups[0].kind === "custom:mymeter",
   "a registered custom kind produces its group");
ok(r.groups[0].source === "declared", "a custom group is a DECLARED group");

/* A registered grouped custom widget spans its adjacent run. */
clearWidgets();
registerWidget("custom:xy", { draw: () => {} });
cp = [{ key: "px", name: "X", type: "float", min: 0, max: 1,
        viz: { group: "pad", role: "x", kind: "custom:xy" } },
      { key: "py", name: "Y", type: "float", min: 0, max: 1,
        viz: { group: "pad", role: "y" } }];
keys = ["px", "py", null, null, null, null, null, null];
r = resolveViz({ keys, metaIndex: idx(cp, keys) });
ok(r.groups.length === 1 && r.groups[0].kind === "custom:xy" && r.groups[0].slotSpan === 2,
   "a grouped custom widget spans its adjacent run");

/* UNREGISTERED SINGLE: claims nothing, and the detector still fires. */
clearWidgets();
cp = [{ key: "attack",  name: "Attack",  type: "float", min: 0, max: 1, viz: { kind: "custom:nope" } },
      { key: "decay",   name: "Decay",   type: "float", min: 0, max: 1 },
      { key: "sustain", name: "Sustain", type: "float", min: 0, max: 1 },
      { key: "release", name: "Release", type: "float", min: 0, max: 1 }];
keys = ["attack", "decay", "sustain", "release", null, null, null, null];
r = resolveViz({ keys, metaIndex: idx(cp, keys) });
ok(!r.groups.some((g) => isCustomKind(g.kind)),
   "an unregistered custom kind produces no custom group");
ok(r.groups.some((g) => g.kind === VIZ_ENVELOPE),
   "the detector still draws its built-in widget over those keys");

/* UNREGISTERED GROUP: the kind may be declared on any member, so the group
 * path needs its own guard -- a single-only guard would let this through. */
clearWidgets();
cp = [{ key: "attack",  name: "Attack",  type: "float", min: 0, max: 1,
        viz: { group: "amp", role: "attack", kind: "custom:nope" } },
      { key: "decay",   name: "Decay",   type: "float", min: 0, max: 1,
        viz: { group: "amp", role: "decay" } },
      { key: "sustain", name: "Sustain", type: "float", min: 0, max: 1,
        viz: { group: "amp", role: "sustain" } },
      { key: "release", name: "Release", type: "float", min: 0, max: 1,
        viz: { group: "amp", role: "release" } }];
keys = ["attack", "decay", "sustain", "release", null, null, null, null];
r = resolveViz({ keys, metaIndex: idx(cp, keys) });
ok(!r.groups.some((g) => isCustomKind(g.kind)),
   "an unregistered custom kind declared on a GROUP produces no custom group");
ok(r.groups.some((g) => g.kind === VIZ_ENVELOPE),
   "the detector recovers a group whose custom kind was unavailable");

/* AND IT RECOVERS ALL FOUR CELLS.
 *
 * This is the assertion the one above cannot make on its own. A groups kind
 * may be declared on ANY member, so guarding in the shared walk rather than in
 * the group loop would drop only the member carrying the kind: the remaining
 * three roles would still form a group, inferKindFromRoles would name it
 * envelope, and the check above would go green over a THREE-cell envelope with
 * `attack` orphaned as a dial beside it. Pin the span, or the bug is invisible. */
const env = r.groups.find((g) => g.kind === VIZ_ENVELOPE);
ok(env && env.slotSpan === 4,
   "the recovered group spans all FOUR cells, not three with one orphaned");
ok(env && ["attack", "decay", "sustain", "release"].every((k) => env.keys.includes(k)),
   "the recovered group contains the key that carried the custom kind");

/* Registration and availability are distinct: the disabled set is what
 * one-strike will use, so isWidgetAvailable is the predicate viz consults. */
clearWidgets();
ok(!isWidgetAvailable("custom:never"), "an unregistered kind is not available");
registerWidget("custom:here", { draw: () => {} });
ok(isWidgetAvailable("custom:here"), "a registered kind is available");
clearWidgets();
ok(!isWidgetAvailable("custom:here"), "clearWidgets makes it unavailable again");

process.exit(fail ? 1 : 0);
'
