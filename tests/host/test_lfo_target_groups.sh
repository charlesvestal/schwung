#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The LFO target picker groups a component's modulatable params by the levels
# its ui_hierarchy declares, instead of offering one flat run of every key.
#
# Measured on the 95-module fleet capture before the change: minijv offered 418
# rows in a single list, surge 303, forge 250, mrdrums 213. The author's own
# section names were sitting unused in the same hierarchy the knob grid pages
# from, and 84 of the 95 publish them.
#
# The two properties that make grouping safe rather than merely nicer are what
# this pins:
#
#   LOSSLESS -- the union of the groups is EXACTLY the flat list. Grouping must
#   never cost a target, because the routing it would have made is one the DSP
#   would have honoured. Asserted over every module in the fixture, on keys AND
#   labels AND count, so a group that silently duplicates or renames a param
#   fails too.
#
#   NAMED THE SAME as the grid's pages -- both come out of level_walk.mjs. No
#   screen shows a page title next to the picker's row for the same level, so a
#   second copy of the naming rule would drift in silence.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the LFO target grouping test" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/lfo_target_groups.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("node:fs"),
]).then(([G, PP, fs]) => {
  let failures = 0;
  const fail = (m) => { console.log("FAIL: " + m); failures++; };

  const fixture = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const modules = fixture.modules;
  const call = (m) => G.groupLfoTargetParams({
    hierarchy: m.ui_hierarchy, chainParams: m.chain_params });

  /* ---------------------------------------------------- 1. losslessness */

  let groupedCount = 0;
  for (const m of modules) {
    const r = call(m);
    if (!r.grouped) {
      if (r.groups.length !== 0)
        fail(m.id + ": grouped=false but returned " + r.groups.length + " groups; " +
             "the caller skips the group step on that flag and would never show them");
      continue;
    }
    groupedCount++;

    const union = r.groups.flatMap((g) => g.params);
    if (union.length !== r.flat.length)
      fail(m.id + ": groups hold " + union.length + " params, the flat list has " +
           r.flat.length + " -- grouping " +
           (union.length < r.flat.length ? "LOST" : "duplicated") + " targets");

    const seen = new Set();
    for (const p of union) {
      if (seen.has(p.key)) fail(m.id + ": " + p.key + " appears in two groups");
      seen.add(p.key);
    }
    for (const p of r.flat) {
      if (!seen.has(p.key))
        fail(m.id + ": " + p.key + " is offered by the flat list and by no group");
    }
    /* Labels too: the flat list is the single source of naming, so a group
       must not re-spell a param out of the hierarchy. */
    const flatLabel = new Map(r.flat.map((p) => [p.key, p.label]));
    for (const p of union) {
      if (flatLabel.get(p.key) !== p.label)
        fail(m.id + ": " + p.key + " is labelled " + JSON.stringify(p.label) +
             " in its group and " + JSON.stringify(flatLabel.get(p.key)) + " in the flat list");
    }

    /* A group with no params is a nav node the user cannot pick anything out of. */
    for (const g of r.groups) {
      if (!g.params.length) fail(m.id + ": group " + JSON.stringify(g.label) + " is empty");
      if (!g.label) fail(m.id + ": a group has no label");
    }
    /* Two rows with the same text are two rows the user cannot tell apart. */
    const labels = r.groups.map((g) => g.label);
    if (new Set(labels).size !== labels.length)
      fail(m.id + ": duplicate group labels -- " + labels.join(" | "));
  }

  if (groupedCount < 40)
    fail("only " + groupedCount + " of " + modules.length + " modules group; the " +
         "capture had 84 publishing levels and 54 clearing the size threshold");

  /* -------------------------------------- 2. when NOT to add a step */

  const byId = (id) => modules.find((m) => m.id === id);

  /* 11 of 95 publish no levels at all. */
  const po32 = byId("po32-drum");
  if (po32 && call(po32).grouped)
    fail("po32-drum publishes no levels and must not be grouped");

  /* A menu level over six rows costs a click and saves no scrolling. */
  const short = G.groupLfoTargetParams({
    hierarchy: { levels: { root: { name: "R", params: ["a", "b"] },
                           x: { name: "X", params: ["c"] } } },
    chainParams: [{ key: "a", type: "float" }, { key: "b", type: "float" },
                  { key: "c", type: "float" }],
  });
  if (short.grouped) fail("3 params were grouped; the threshold is " + G.MIN_PARAMS_TO_GROUP);
  if (short.flat.length !== 3) fail("the flat list must still be returned when ungrouped");

  /* One group is the flat list with a click in front of it. */
  const oneLevel = byId("cloudseed");
  if (oneLevel && call(oneLevel).grouped)
    fail("cloudseed declares a single level; one group is the flat list with a click " +
         "in front of it and must fall back");

  /* Only continuous types: a string or an action has no range for a depth to
     mean anything against -- and wav_position DOES have one. */
  {
    const r = G.flatLfoTargetParams([
      { key: "a", type: "float" }, { key: "s", type: "string" },
      { key: "w", type: "wav_position" }, { key: "f", type: "filepath" },
      { key: "e", type: "enum" }, { key: "i", type: "int" },
    ]);
    const keys = r.map((p) => p.key).join(",");
    if (keys !== "a,w,e,i")
      fail("the modulatable type allowlist offers " + keys + ", not a,w,e,i -- " +
           "wav_position is a ranged number the modulation engine scales like any other");
  }

  /* --------------------------------- 3. group names ARE the page names */

  /* Not a restatement: this is the reason level_walk.mjs exists as its own
     module. A group called something the grid does not call the same level
     leaves the user with two names for one thing and no way to connect them. */
  for (const m of modules) {
    const r = call(m);
    if (!r.grouped) continue;
    const plan = PP.planPages({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params });
    const pageNames = new Set(plan.pages.map((p) => p.name));
    /* Only the levels the grid also reaches -- the picker deliberately walks
       every mode, and "Other" is ours. Of the rest, a group label must be a
       name the grid uses for something, modulo the " - N" that either side may
       append to break a collision the other did not have. */
    for (const g of r.groups) {
      if (g.key === G.OTHER_GROUP_KEY) continue;
      const onPage = plan.pages.some((p) => p.level === g.key);
      if (!onPage) continue;
      /* The one sanctioned divergence: the walker calls its root "Main", and
         with `modes` the picker has two roots, so it uses each mode name
         instead. minijv is grouped under "Patch"/"Performance" where the grid
         shows whichever is active as "Main". */
      if (plan.pages.some((p) => p.level === g.key && p.name === "Main")) continue;
      const base = g.label.replace(/ - \d+$/, "");
      const hit = [...pageNames].some((n) => n.replace(/ - \d+$/, "") === base);
      if (!hit)
        fail(m.id + ": level " + g.key + " is called " + JSON.stringify(g.label) +
             " in the picker and has no page of that name in the grid -- " +
             "the two naming rules have drifted");
    }
  }

  /* minijv walks BOTH modes: the grid gates on the active one, the picker does
     not, because a routing at a mode-inactive param is still a routing the DSP
     honours. */
  {
    const mj = byId("minijv");
    if (mj) {
      const r = call(mj);
      const labels = r.groups.map((g) => g.label);
      if (!labels.some((l) => /^Tone1\//.test(l)) || !labels.some((l) => /Part|Perf/i.test(l)))
        fail("minijv did not reach both mode trees: " + labels.join(" | "));
      if (labels.filter((l) => /^Main/.test(l)).length > 1)
        fail("two roots both claimed \"Main\": " + labels.join(" | "));
    }
  }

  /* ------------------------------------------------- 4. where to land */

  {
    const groups = [
      { key: "a", label: "A", params: [{ key: "p1" }, { key: "p2" }] },
      { key: "b", label: "B", params: [{ key: "p3" }, { key: "p4" }, { key: "p5" }] },
    ];
    const at = G.locateLfoTargetParam(groups, "p5");
    if (at.groupIndex !== 1 || at.paramIndex !== 2)
      fail("a stored target resolved to group " + at.groupIndex + " param " +
           at.paramIndex + ", not 1/2 -- reopening the picker would start at the " +
           "top and cost the same scroll as the first time");
    /* A module swapped out from under the routing: the key is still stored and
       still valid, but there is no row to land on. */
    const gone = G.locateLfoTargetParam(groups, "vanished");
    if (gone.groupIndex !== 0 || gone.paramIndex !== 0)
      fail("an unoffered key must fall back to the top, got " + JSON.stringify(gone));
    if (G.locateLfoTargetParam(groups, "").groupIndex !== 0)
      fail("an unrouted LFO must land at the top");
    if (G.indexOfKey([{ key: "x" }, { key: "y" }], "y") !== 1)
      fail("indexOfKey did not find a present key");
    if (G.indexOfKey([{ key: "x" }], "nope") !== 0)
      fail("indexOfKey must fall back to 0 for a key that is gone");
  }

  if (failures) process.exit(1);
  console.log("PASS: LFO target groups are lossless across " + modules.length +
              " modules, named as the grid names the same levels, skipped where a " +
              "step would not pay, and seeded from the stored routing");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
