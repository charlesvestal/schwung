#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Fleet-capture logic (tools/param-pages/dump_contracts.mjs).
#
# The capture is pure with respect to the device — it asks three injected
# questions — so it can be driven by a SIMULATED Move built from the existing
# fixture. Round-tripping the fixture through the dumper proves the logic
# without hardware: whatever comes out must plan to the same pages as what went
# in, or the capture is losing something.
#
# The device wiring itself (dump_contracts_device.js) is NOT covered here and is
# marked unverified in its own header.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the dump tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./tools/param-pages/dump_contracts.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("node:fs"),
]).then(([D, P, fs]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));

  /* A Move made of the fixture: loading a module makes its declarations the
   * answers, exactly as the real device serves them from the loaded DSP. */
  const makeDevice = (modules, opts = {}) => {
    let loaded = null;
    return {
      listModules: () => modules.map((m) => ({
        id: m.id, category: m.category, componentKey: m.component_key,
        name: m.name, version: m.version,
      })),
      loadModule: (m) => {
        if (opts.failOn && opts.failOn.includes(m.id)) return false;
        loaded = modules.find((x) => x.id === m.id);
        return !!loaded;
      },
      getParam: (m, key) => {
        if (!loaded || loaded.id !== m.id) fail("getParam before loadModule for " + m.id);
        if (key === "ui_hierarchy") return loaded.ui_hierarchy ? JSON.stringify(loaded.ui_hierarchy) : null;
        if (key === "chain_params") return loaded.chain_params ? JSON.stringify(loaded.chain_params) : null;
        if (loaded.presets && key === loaded.presets.count_param) return String(loaded.presets.count);
        return null;
      },
      now: () => new Date(0),
    };
  };

  /* ---- 1. round trip: capture reproduces the fixture ------------------- */
  {
    const dump = D.dumpContracts(makeDevice(fx.modules));
    if (dump.module_count !== fx.modules.length) {
      fail("captured " + dump.module_count + " modules, fixture has " + fx.modules.length);
    }
    if (dump.failures.length) fail("clean device produced failures: " + JSON.stringify(dump.failures));

    for (const captured of dump.modules) {
      const original = fx.modules.find((m) => m.id === captured.id);
      if (JSON.stringify(captured.ui_hierarchy) !== JSON.stringify(original.ui_hierarchy)) {
        fail(captured.id + ": ui_hierarchy did not survive the round trip");
      }
      if (JSON.stringify(captured.chain_params) !== JSON.stringify(original.chain_params)) {
        fail(captured.id + ": chain_params did not survive the round trip");
      }
    }
  }

  /* ---- 2. the captured dump plans identically -------------------------- */
  {
    const dump = D.dumpContracts(makeDevice(fx.modules));
    for (const captured of dump.modules) {
      const original = fx.modules.find((m) => m.id === captured.id);
      const a = P.planPages({ hierarchy: original.ui_hierarchy, chainParams: original.chain_params });
      const b = P.planPages({ hierarchy: captured.ui_hierarchy, chainParams: captured.chain_params });
      if (a.fingerprint !== b.fingerprint) fail(captured.id + ": a captured module plans differently");
    }
  }

  /* ---- 3. preset counts are captured, names never are ------------------ */
  {
    const dump = D.dumpContracts(makeDevice(fx.modules));
    const jv = dump.modules.find((m) => m.id === "minijv");
    if (!jv.presets) fail("minijv preset metadata was not captured");
    /* The COUNT is asserted as "large enough to need paginating", not as an
     * exact number. It used to be pinned at 2427, which is not a property of
     * the capture at all -- it is how many patches the CAPTURING DEVICE happened to have
     * installed. Re-capturing on a Move with a smaller JV-880 ROM set brought
     * it back as 192, and a settle-until-stable wait confirmed 192 is where it
     * stops: nothing was captured early, the banks are simply not there.
     *
     * Pinning the asset inventory of one device makes every future recapture
     * fail for a reason that has nothing to do with the code under test. */
    if (!(jv.presets.count > 1)) fail("minijv preset count wrong: " + jv.presets.count);
    /* Names read one call at a time would take minutes and produce a fixture
     * nobody can read -- that is the invariant, and it holds at any count. */
    for (const m of dump.modules) {
      if (m.presets && m.presets.names !== null) fail(m.id + ": preset names must not be captured");
    }
  }

  /* ---- 4. a module that will not load is recorded, not fatal ----------- */
  {
    const dump = D.dumpContracts(makeDevice(fx.modules, { failOn: ["obxd", "surge"] }));
    if (dump.failures.length !== 2) fail("expected 2 failures, got " + dump.failures.length);
    if (dump.module_count !== fx.modules.length) fail("a failed module should still appear in the dump");
    const failed = dump.modules.find((m) => m.id === "obxd");
    if (failed.status !== "load-failed") fail("a failed module should be marked load-failed");
    const ok = dump.modules.find((m) => m.id === "sf2");
    if (ok.status !== "ok") fail("one failure must not poison the rest of the run");
  }

  /* ---- 5. the diff describes what moved -------------------------------- */
  {
    const before = D.dumpContracts(makeDevice(fx.modules));
    const shrunk = JSON.parse(JSON.stringify(before));
    shrunk.modules = shrunk.modules.filter((m) => m.id !== "arp");
    const target = shrunk.modules.find((m) => m.id === "obxd");
    target.chain_params = target.chain_params.slice(0, 5);

    const d = D.diffDumps(before, shrunk);
    if (!d.removed.includes("arp")) fail("diff missed a removed module");
    if (!d.changed.some((c) => c.id === "obxd")) fail("diff missed a module that lost params");
    if (d.added.length) fail("diff invented an added module");

    /* An identical dump is a no-op diff — a fixture refresh with no real change
     * must not look like a change. */
    const same = D.diffDumps(before, D.dumpContracts(makeDevice(fx.modules)));
    if (same.added.length || same.removed.length || same.changed.length) {
      fail("re-capturing the same device reported a diff");
    }
  }

  console.log("PASS: fleet capture — round-trips the fixture, plans identically, " +
              "records load failures, and diffs cleanly");
});
'
