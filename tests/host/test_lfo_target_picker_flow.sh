#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The LFO target picker's NAVIGATION, driven through the real functions lifted
# out of shadow_ui.js.
#
# Two things the grouping module cannot say anything about, because they are
# about screens rather than lists:
#
#   WHERE BACK GOES. The group step is skipped for a component with no usable
#   hierarchy, so Back from the param list must go two different places
#   depending on a decision made one screen earlier. Getting that wrong strands
#   the user on a screen they were never shown, or drops them past the
#   component picker entirely.
#
#   WHERE THE CURSOR LANDS. The picker used to reset to 0 on every entry; the
#   point of the change is that it now lands on the routing the LFO already
#   has. A seed that silently falls back to 0 looks exactly like the old
#   behaviour, which is why every case here asserts the INDEX and not merely
#   that nothing threw.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the LFO target picker flow test" >&2
  exit 1
fi

node --input-type=module -e '
import fs from "node:fs";
import { locateLfoTargetParam, indexOfKey } from "./src/shared/lfo_target_groups.mjs";

let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };

const src = fs.readFileSync("./src/shadow/shadow_ui.js", "utf8");
function lift(name, deps) {
  const at = src.indexOf("function " + name + "(");
  const end = at >= 0 ? src.indexOf("\n}\n", at) : -1;
  if (end < 0) { console.log("FAIL: " + name + " is gone from shadow_ui.js"); process.exit(1); }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}

/* ------------------------------------------------------------------ world */

const VIEWS = {
  LFO_EDIT: "lfoedit",
  LFO_TARGET_COMPONENT: "lfotargetcomp",
  LFO_TARGET_GROUP: "lfotargetgroup",
  LFO_TARGET_PARAM: "lfotargetparam",
};

const COMPONENTS = [
  { key: "synth", label: "Synth: Forge" },
  { key: "fx1",   label: "FX 1: Reverb" },
  { key: "lfo2",  label: "LFO 2" },
  { key: "__clear__", label: "[Clear Target]" },
];

/* A component with levels worth grouping, and one without. */
const GROUPED = {
  grouped: true,
  flat: [],
  groups: [
    { key: "root",   label: "Main",   params: [{ key: "a", label: "A" }, { key: "b", label: "B" }] },
    { key: "filter", label: "Filter", params: [{ key: "cut", label: "Cutoff" }, { key: "res", label: "Res" }] },
    { key: "amp",    label: "Amp",    params: [{ key: "atk", label: "Attack" }] },
  ],
};
const FLAT = {
  grouped: false,
  flat: [{ key: "x", label: "X" }, { key: "y", label: "Y" }, { key: "z", label: "Z" }],
  groups: [],
};

/* One mutable bag standing in for the module-level picker state. */
function makeWorld({ target, targetParam, grouping }) {
  const w = {
    view: VIEWS.LFO_EDIT,
    lfoTargetComponents: [], selectedLfoTargetComp: 0,
    lfoTargetGroups: [], selectedLfoTargetGroup: 0,
    lfoTargetParams: [], selectedLfoTargetParam: 0,
    announced: [],
  };
  w.ctx = {
    getParam: (k) => (k === "target" ? target : k === "target_param" ? targetParam : ""),
    getTargetComponents: () => COMPONENTS.slice(),
    getTargetGroups: () => grouping,
    getTargetParams: () => grouping.flat,
    title: "LFO 1",
  };
  return w;
}

/* The three entry points, each closed over the same world. Lifting them keeps
   the test on the shipped control flow rather than a paraphrase of it. */
function bind(w) {
  const setView = (v) => { w.view = v; };
  const announce = (s) => { w.announced.push(s); };
  const deps = {
    lfoCtx: w.ctx, setView, announce, VIEWS, indexOfKey, locateLfoTargetParam,
    /* setters, because a lifted function assigns to module-level bindings */
  };
  /* new Function cannot write back to our object through a plain parameter, so
     the picker state is exposed as accessors on a proxy the lifted code sees
     as bare identifiers. Simplest faithful way: eval the lifted source inside a
     wrapper that declares the state as `var` and hands it back. */
  const body = ["enterLfoTargetPicker", "enterLfoTargetParamPicker", "enterLfoTargetGroupParams"]
    .map((n) => {
      const at = src.indexOf("function " + n + "(");
      const end = src.indexOf("\n}\n", at);
      if (at < 0 || end < 0) { console.log("FAIL: " + n + " is gone from shadow_ui.js"); process.exit(1); }
      return src.slice(at, end + 2);
    }).join("\n");
  const f = new Function(
    "lfoCtx", "setView", "announce", "VIEWS", "indexOfKey", "locateLfoTargetParam", "state",
    "var lfoTargetComponents = [], selectedLfoTargetComp = 0;\n" +
    "var lfoTargetGroups = [], selectedLfoTargetGroup = 0;\n" +
    "var lfoTargetParams = [], selectedLfoTargetParam = 0;\n" +
    body + "\n" +
    "return { enterLfoTargetPicker, enterLfoTargetParamPicker, enterLfoTargetGroupParams,\n" +
    "  read: () => ({ comp: selectedLfoTargetComp, group: selectedLfoTargetGroup,\n" +
    "    param: selectedLfoTargetParam, groups: lfoTargetGroups, params: lfoTargetParams,\n" +
    "    components: lfoTargetComponents }) };"
  );
  return f(w.ctx, setView, announce, VIEWS, indexOfKey, locateLfoTargetParam, w);
}

/* =================================================================== cases */

/* ---- 1. grouped: the cursor lands on the stored routing ---- */
{
  const w = makeWorld({ target: "fx1", targetParam: "res", grouping: GROUPED });
  const api = bind(w);
  api.enterLfoTargetPicker();
  let st = api.read();
  if (st.comp !== 1)
    fail("step 1 landed on component " + st.comp + " (" + COMPONENTS[st.comp].label +
         "), not the fx1 the LFO is routed at -- the picker is still resetting to 0");
  if (w.view !== VIEWS.LFO_TARGET_COMPONENT) fail("step 1 did not open the component picker");

  api.enterLfoTargetParamPicker("fx1");
  st = api.read();
  if (w.view !== VIEWS.LFO_TARGET_GROUP)
    fail("a grouped component did not open the group step, view is " + w.view);
  if (st.group !== 1)
    fail("landed on group " + st.group + ", not the Filter group that holds res");
  if (st.param !== 1)
    fail("landed on param " + st.param + ", not res at index 1 inside Filter");

  api.enterLfoTargetGroupParams(1);
  st = api.read();
  if (w.view !== VIEWS.LFO_TARGET_PARAM) fail("the group step did not open the param list");
  if (st.params.map((p) => p.key).join(",") !== "cut,res")
    fail("the param list is not the group’s params: " + st.params.map((p) => p.key));
  if (st.param !== 1) fail("the seeded param index was lost entering the group");
}

/* ---- 2. grouped, but entering a DIFFERENT group starts at the top ---- */
{
  const w = makeWorld({ target: "fx1", targetParam: "res", grouping: GROUPED });
  const api = bind(w);
  api.enterLfoTargetPicker();
  api.enterLfoTargetParamPicker("fx1");
  api.enterLfoTargetGroupParams(0);
  const st = api.read();
  if (st.param !== 0)
    fail("entering a group the routing is not in kept index " + st.param +
         "; an index from another list points at whatever happens to sit there");
}

/* ---- 3. routed at a DIFFERENT component: do not carry the index across ---- */
{
  const w = makeWorld({ target: "synth", targetParam: "res", grouping: GROUPED });
  const api = bind(w);
  api.enterLfoTargetPicker();
  api.enterLfoTargetParamPicker("fx1");   /* user walked to fx1, not the routed one */
  const st = api.read();
  if (st.group !== 0 || st.param !== 0)
    fail("a routing in ANOTHER component seeded this one: group " + st.group +
         " param " + st.param + ". The key means nothing here.");
}

/* ---- 4. ungrouped: the group step is skipped entirely ---- */
{
  const w = makeWorld({ target: "fx1", targetParam: "z", grouping: FLAT });
  const api = bind(w);
  api.enterLfoTargetPicker();
  api.enterLfoTargetParamPicker("fx1");
  const st = api.read();
  if (w.view !== VIEWS.LFO_TARGET_PARAM)
    fail("an ungrouped component opened " + w.view + " instead of the param list");
  if (st.param !== 2)
    fail("the ungrouped list landed on " + st.param + ", not z at index 2");
}

/* ---- 4b. a GROUPED component visited first must not leave its groups behind ---- */
/* The sequence that produces the bug, not a fresh state: open a grouped
   component, back out, open an ungrouped one. If the group list survives, Back
   from the ungrouped param list returns to the previous component’s sections --
   a screen this component never showed. Case 4 alone cannot see this, because a
   world that starts empty is empty either way. */
{
  const w = makeWorld({ target: "", targetParam: "", grouping: GROUPED });
  const api = bind(w);
  api.enterLfoTargetPicker();
  api.enterLfoTargetParamPicker("synth");        /* grouped: populates the groups */
  if (api.read().groups.length === 0) fail("premise: the grouped component populated nothing");

  /* Back to the component list is a VIEW CHANGE, not a re-entry -- step 1 is
     not run again -- so the clear that matters is the one in step 2. */
  w.ctx.getTargetGroups = () => FLAT;            /* now an ungrouped component */
  api.enterLfoTargetParamPicker("fx1");
  const st = api.read();
  if (w.view !== VIEWS.LFO_TARGET_PARAM)
    fail("the ungrouped component did not open the param list");
  if (st.groups.length !== 0)
    fail("the previous component left " + st.groups.length + " groups behind; Back from " +
         "this param list would open a section screen that was never shown");
}

/* ---- 5. an unrouted LFO, and a key the module no longer offers ---- */
{
  for (const [tp, why] of [["", "unrouted"], ["vanished", "a key the module no longer offers"]]) {
    const w = makeWorld({ target: "fx1", targetParam: tp, grouping: GROUPED });
    const api = bind(w);
    api.enterLfoTargetPicker();
    api.enterLfoTargetParamPicker("fx1");
    const st = api.read();
    if (st.group !== 0 || st.param !== 0)
      fail(why + " must land at the top, got group " + st.group + " param " + st.param);
  }
}

/* ---- 6. an LFO-to-LFO target still works, with no hierarchy anywhere ---- */
{
  const w = makeWorld({ target: "lfo2", targetParam: "rate_hz",
    grouping: { grouped: false, groups: [],
                flat: [{ key: "depth", label: "Depth" }, { key: "rate_hz", label: "Rate Hz" }] } });
  const api = bind(w);
  api.enterLfoTargetPicker();
  if (api.read().comp !== 2) fail("did not land on the LFO 2 component row");
  api.enterLfoTargetParamPicker("lfo2");
  if (api.read().param !== 1) fail("did not land on rate_hz");
  if (w.view !== VIEWS.LFO_TARGET_PARAM) fail("LFO-to-LFO must skip the group step");
}

/* ---- 7. Back goes one screen, and the screen depends on step 2 ---- */
/* Lifted from the source so the two branches cannot drift from what ships. */
{
  const at = src.indexOf("case VIEWS.LFO_TARGET_PARAM:", src.indexOf("function handleBack"));
  const region = at >= 0 ? src.slice(at, at + 900) : "";
  if (!/lfoTargetGroups\.length\s*>\s*0/.test(region))
    fail("Back from the param list does not branch on whether a group step was shown");
  if (!/setView\(VIEWS\.LFO_TARGET_GROUP\)/.test(region))
    fail("Back from a GROUPED param list does not return to the group step");
  if (!/setView\(VIEWS\.LFO_TARGET_COMPONENT\)/.test(region))
    fail("Back from an UNGROUPED param list does not return to the component picker");
}

if (failures) process.exit(1);
console.log("PASS: the picker lands on the routing it already has, skips the group step " +
            "where there is nothing to group, and Back returns to the screen that was shown");
'
