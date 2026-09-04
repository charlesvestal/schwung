#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A THROWING WIDGET GETS ONE STRIKE, AND THE PAGE STAYS CORRECT.
#
# The fallback is not "draw nothing" -- it is the built-in widget the detector
# would have chosen, so a user whose module ships a broken widget sees a working
# page rather than a hole. The author sees the throw in debug.log.
#
# This is the posture page_controller already takes on an unresolved contract:
# keep something correct on screen, never let a failure become a picture.
#
# Catching every frame instead would flood the log and burn the frame budget
# forever; not catching at all would take the shadow UI down for a user who
# merely installed a module from the catalog.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the one-strike tests" >&2
  exit 1
fi

node --input-type=module -e '
import { drawVizGroup } from "./src/shared/param_pages/viz_draw.mjs";
import { resolveViz, VIZ_ENVELOPE, VIZ_FADER } from "./src/shared/param_pages/viz.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { registerWidget, clearWidgets, isWidgetAvailable, setWidgetLogger }
  from "./src/shared/param_pages/widget_registry.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const recorder = () => {
  const calls = [];
  return { calls,
    fillRect(x, y, w, h, c) { calls.push([x, y, w, h, c]); },
    print() {}, textWidth(t) { return String(t).length * 4; } };
};

const idx = (chainParams, keys) => buildMetaIndex({
  hierarchy: { modes: null, levels: { root: {
    label: "T", knobs: keys.filter(Boolean),
    params: chainParams.map((p) => ({ key: p.key })) } } },
  chainParams,
});

const RECT = { x: 8, y: 9, w: 17, h: 15 };
const single = (kind) => ({ kind, group: null, roles: { value: "a" }, keys: ["a"],
                            slotStart: 0, slotSpan: 1 });

/* The widget gets a frame-local ctx, sized to the rect, with no reads. */
clearWidgets();
let seen = null, payload = null;
registerWidget("custom:probe", { draw: (c, p) => { seen = c; payload = p; c.fillRect(0, 0, 2, 2, 1); } });
let p = recorder();
drawVizGroup(p, RECT, single("custom:probe"), { a: "0.5" }, idx([], []));
ok(seen && seen.width === 17 && seen.height === 15,
   "the widget ctx is sized to the frame, not the screen");
ok(seen && typeof seen.getParam === "undefined", "the widget ctx exposes no getParam");
ok(JSON.stringify(p.calls[0]) === JSON.stringify([8, 9, 2, 2, 1]),
   "the widget draws translated into its frame");
ok(payload && payload.values && payload.values.a === "0.5",
   "values are HANDED to the widget rather than read by it");
ok(payload && payload.group && payload.group.kind === "custom:probe",
   "the widget receives its own group");

/* A widget cannot draw outside its frame. */
clearWidgets();
registerWidget("custom:greedy", { draw: (c) => { c.fillRect(-50, -50, 400, 400, 1); } });
p = recorder();
drawVizGroup(p, RECT, single("custom:greedy"), {}, idx([], []));
ok(p.calls.length > 0, "the greedy widget did draw something");
ok(p.calls.every(([x, y, w, h]) => x >= 8 && y >= 9 && x + w <= 25 && y + h <= 24),
   "a greedy widget cannot draw outside its frame");

/* ONE STRIKE. */
clearWidgets();
setWidgetLogger(() => {});
let calls = 0;
registerWidget("custom:bad", { draw: () => { calls++; throw new Error("boom"); } });
const g = single("custom:bad");
drawVizGroup(recorder(), RECT, g, {}, idx([], []));
drawVizGroup(recorder(), RECT, g, {}, idx([], []));
drawVizGroup(recorder(), RECT, g, {}, idx([], []));
ok(calls === 1, "a throwing widget is invoked exactly once across three draws");
ok(!isWidgetAvailable("custom:bad"), "a throwing widget is no longer available");

/* The throw is REPORTED, not swallowed. */
clearWidgets();
let logged = "";
setWidgetLogger((m) => { logged += m; });
registerWidget("custom:noisy", { draw: () => { throw new Error("kaboom"); } });
drawVizGroup(recorder(), RECT, single("custom:noisy"), {}, idx([], []));
ok(/custom:noisy/.test(logged) && /kaboom/.test(logged),
   "the disable names the widget and the error");

/* And the page falls back to the built-in the detector would have chosen. */
const cp = [{ key: "attack",  name: "Attack",  type: "float", min: 0, max: 1, viz: { kind: "custom:bad2" } },
            { key: "decay",   name: "Decay",   type: "float", min: 0, max: 1 },
            { key: "sustain", name: "Sustain", type: "float", min: 0, max: 1 },
            { key: "release", name: "Release", type: "float", min: 0, max: 1 }];
const keys = ["attack", "decay", "sustain", "release", null, null, null, null];
clearWidgets();
setWidgetLogger(() => {});
registerWidget("custom:bad2", { draw: () => { throw new Error("boom"); } });

/* Before the throw: the custom widget owns the cell, so NO envelope. */
let r = resolveViz({ keys, metaIndex: idx(cp, keys) });
ok(r.groups.some((x) => x.kind === "custom:bad2"),
   "before the throw the custom widget claims its key");

/* Throw once, then re-resolve. */
drawVizGroup(recorder(), RECT, r.groups.find((x) => x.kind === "custom:bad2"), {}, idx(cp, keys));
r = resolveViz({ keys, metaIndex: idx(cp, keys) });
ok(!r.groups.some((x) => x.kind === "custom:bad2"),
   "after the throw the custom widget no longer claims its key");
const env = r.groups.find((x) => x.kind === VIZ_ENVELOPE);
ok(env && env.slotSpan === 4,
   "after the disable the detector built-in draws over ALL FOUR of those keys");

/* A built-in kind is untouched by any of this. */
clearWidgets();
p = recorder();
drawVizGroup(p, RECT, { kind: VIZ_FADER, group: null, roles: { value: "a" }, keys: ["a"],
                        slotStart: 0, slotSpan: 1 },
             { a: "0.5" },
             idx([{ key: "a", name: "A", type: "float", min: 0, max: 1 }], ["a"]));
ok(p.calls.length > 0, "a built-in viz kind still draws through the same entry point");

process.exit(fail ? 1 : 0);
'
