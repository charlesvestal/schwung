#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Opening a component editor must not decide from one param read.
#
# Reported from the device: "MiniJV and Osirus sometimes do not appear in the
# module editor after loading. A blank editor is unacceptable."
#
# The chain:
#   1. enterHierarchyEditor read <prefix>:ui_hierarchy once and branched on
#      `if (!hierarchy)`, which is true for BOTH "" (served, empty) and null
#      (the read did not complete).
#   2. null is what you get while the chain host is inside dlopen +
#      create_instance on the SPI callback thread that also serves param
#      requests, and while a plugin's boot thread — born SCHED_FIFO 90 — is
#      starving it. MiniJV and Osirus are the two slowest of these.
#   3. so the gate concluded "this module declares no hierarchy" and committed
#      to enterComponentEditFallback. Neither module ships a ui_chain.js, and
#      the preset reads that fallback makes were failing for the same reason,
#      so it drew an editor with nothing in it.
#   4. it stuck: everything that knows how to wait for a slow module — the knob
#      grid's "Loading..." hold, its contract retry, its recovery probe, the
#      list editor's is_loading re-fetch — lives BEHIND this gate.
#
# Both halves below are required. The first alone would be passed by a gate
# that simply holds forever and never opens anything; the second alone by one
# that never holds, which is the bug.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the component-load-gate tests" >&2
  exit 1
fi

node -e '
import("./src/shared/component_load_gate.mjs").then((M) => {
  const { decideComponentEntry, holdProbeIntervalTicks,
          ENTRY_ENTER, ENTRY_HOLD, ENTRY_FALLBACK,
          HOLD_FAST_INTERVAL_TICKS, HOLD_FAST_LIMIT, HOLD_SLOW_INTERVAL_TICKS,
          HOLD_UNSERVED_READ_LIMIT } = M;

  let failed = 0;
  const check = (name, got, want) => {
    if (got !== want) { console.log(`FAIL: ${name}: got ${got}, want ${want}`); failed++; }
    else { console.log(`ok: ${name}`); }
  };

  const parse = (s) => { try { return JSON.parse(s); } catch (e) { return null; } };
  const HIER = JSON.stringify({ levels: { root: { label: "JV", params: [] } } });

  // Counts reads so we can prove the ordinary path did not get more expensive.
  const reader = (hierarchy, mod, loading) => {
    const n = { hierarchy: 0, module: 0, isLoading: 0 };
    return {
      n,
      io: {
        hierarchy: () => { n.hierarchy++; return hierarchy; },
        module: () => { n.module++; return mod; },
        isLoading: () => { n.isLoading++; return loading; },
      },
    };
  };

  // ---- the bug: a failed read must HOLD, not fall back -------------------
  check("null hierarchy holds",
        decideComponentEntry(reader(null, null, null).io, parse).action, ENTRY_HOLD);

  // ---- ...but not forever: a NAMED, NOT-LOADING module whose read still ----
  // ---- fails after HOLD_UNSERVED_READ_LIMIT probes does not serve the key ----
  // (9W9 answered ui_hierarchy with an error, not ""; a swap into it sat on
  //  Loading... until the user backed out.)
  check("unserved read: still holds on the first probes",
        decideComponentEntry(reader(null, "9w9", "0").io, parse, HOLD_UNSERVED_READ_LIMIT - 1).action, ENTRY_HOLD);
  {
    const d = decideComponentEntry(reader(null, "9w9", "0").io, parse, HOLD_UNSERVED_READ_LIMIT);
    check("unserved read: falls back once the limit is reached", d.action, ENTRY_FALLBACK);
    check("unserved read: says why", d.reason, "hierarchy-not-served");
  }
  check("unserved read: a module still LOADING keeps holding past the limit",
        decideComponentEntry(reader(null, "osirus", "1").io, parse, HOLD_UNSERVED_READ_LIMIT + 5).action, ENTRY_HOLD);
  check("unserved read: an UNNAMED position keeps holding past the limit",
        decideComponentEntry(reader(null, "", "0").io, parse, HOLD_UNSERVED_READ_LIMIT + 5).action, ENTRY_HOLD);
  check("unserved read: a failed module read keeps holding past the limit",
        decideComponentEntry(reader(null, null, "0").io, parse, HOLD_UNSERVED_READ_LIMIT + 5).action, ENTRY_HOLD);
  check("the limit is a few fast probes, not the whole fast phase",
        HOLD_UNSERVED_READ_LIMIT < HOLD_FAST_LIMIT, true);

  // "" from ui_hierarchy with a module that has not been published yet is the
  // other shape of the same window — the chain host only names the module
  // after create_instance returns.
  check("empty hierarchy + unpublished module holds",
        decideComponentEntry(reader("", "", "").io, parse).action, ENTRY_HOLD);

  check("empty hierarchy + failed module read holds",
        decideComponentEntry(reader("", null, null).io, parse).action, ENTRY_HOLD);

  // A module that says so itself.
  check("module reporting is_loading holds",
        decideComponentEntry(reader("", "virus", "1").io, parse).action, ENTRY_HOLD);

  // ---- and the gate must still OPEN, and still fall back -----------------
  const enter = decideComponentEntry(reader(HIER, "jv880", "0").io, parse);
  check("declared hierarchy enters", enter.action, ENTRY_ENTER);
  check("declared hierarchy is handed back parsed",
        enter.hierarchy && enter.hierarchy.levels && enter.hierarchy.levels.root.label, "JV");

  // The well-behaved fleet: loaded, named, declares no hierarchy. Must reach
  // its preset browser IMMEDIATELY — a hold here would put "Loading..." in
  // front of every module that has presets and no ui_hierarchy.
  check("named module with no hierarchy falls back at once",
        decideComponentEntry(reader("", "sf2", "0").io, parse).action, ENTRY_FALLBACK);
  check("named module that does not serve is_loading falls back at once",
        decideComponentEntry(reader("", "sf2", "").io, parse).action, ENTRY_FALLBACK);

  // A truncated / broken declaration is a broken module, not a slow one.
  check("unparseable hierarchy falls back",
        decideComponentEntry(reader("{\"levels\":", "x", "0").io, parse).action, ENTRY_FALLBACK);

  // ---- the ordinary path must not have got more expensive ----------------
  const r = reader(HIER, "jv880", "0");
  decideComponentEntry(r.io, parse);
  check("entering costs exactly one read", r.n.hierarchy, 1);
  check("entering reads no module", r.n.module, 0);
  check("entering reads no is_loading", r.n.isLoading, 0);

  const r2 = reader(null, "jv880", "0");
  decideComponentEntry(r2.io, parse);
  check("a failed hierarchy read asks nothing further", r2.n.module, 0);

  // ---- the probe slows down but never stops ------------------------------
  check("first probes are fast", holdProbeIntervalTicks(0), HOLD_FAST_INTERVAL_TICKS);
  check("last fast probe is fast", holdProbeIntervalTicks(HOLD_FAST_LIMIT - 1), HOLD_FAST_INTERVAL_TICKS);
  check("probing continues, slowly, past the fast budget",
        holdProbeIntervalTicks(HOLD_FAST_LIMIT), HOLD_SLOW_INTERVAL_TICKS);
  check("probing never stops", holdProbeIntervalTicks(100000), HOLD_SLOW_INTERVAL_TICKS);
  // The fast phase has to outlast a big dlopen and a fork-and-boot. At 60 Hz:
  const fastSeconds = (HOLD_FAST_LIMIT * HOLD_FAST_INTERVAL_TICKS) / 60;
  check("fast phase covers a slow module (>= 15s)", fastSeconds >= 15, true);

  if (failed) { console.log(`FAILED: ${failed} check(s)`); process.exit(1); }
  console.log("PASS: component load gate");
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
'
