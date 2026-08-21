#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# `access` — which direction a parameter actually means something in.
#
#   "readwrite"  (default) an ordinary control
#   "read"       a READOUT: the value means something, writing means nothing
#   "write"      a TRIGGER: writing does something, the value means nothing
#
# Two ends of one axis, and both were unexpressible before 1.0.
#
# The read end is a design gap we walked into: keydetect's `detected_key` is 25
# key names with no set_param branch at all, deliberately, and documented as
# such back when an enum could only be nudged one detent. Enums became divable
# in 1.0, so the picker opened on it and silently discarded the choice.
#
# The write end is the dangerous one. euclidrum's `rnd_preset` declares
# ["—","Rnd!"] and fires on anything that is not the em-dash — so an INDEX
# write of "0", which MEANS the em-dash, "do nothing", randomises all eight
# lanes and destroys the kit. A trigger must be fired through the module's own
# enum wire, never as a bare number.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/page_controller.mjs"),
]).then(([M, C]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* ---- the axis itself --------------------------------------------- */
  const ix = M.buildMetaIndex({ chainParams: [
    { key: "cutoff",       type: "float", min: 0, max: 1 },
    { key: "mode",         type: "enum",  options: ["LP", "HP"] },
    { key: "detected_key", type: "enum",  options: ["C", "C#", "D"], access: "read" },
    { key: "rnd_preset",   type: "enum",  options: ["—", "Rnd!"], access: "write" },
  ]});
  const meta = (k) => ix.getOrGuess(k);

  /* An ordinary enum is unaffected — the default must stay readwrite. */
  if (!M.isTurnable(meta("mode"))) fail("a plain enum stopped being turnable");
  if (!meta("mode").divable)       fail("a plain enum stopped being divable");
  if (M.isReadOnly(meta("mode")) || M.isTrigger(meta("mode")))
    fail("a param with no access declared is not readwrite by default");
  if (!M.isTurnable(meta("cutoff"))) fail("a plain float stopped being turnable");

  /* A readout: nothing to set, nothing to open. */
  if (M.isTurnable(meta("detected_key"))) fail("a read-only param is still turnable");
  if (meta("detected_key").divable)       fail("a read-only param still opens a picker");
  if (!M.isReadOnly(meta("detected_key"))) fail("isReadOnly did not recognise access:read");

  /* A trigger: fired, not scrubbed, not opened. */
  if (M.isTurnable(meta("rnd_preset")))
    fail("a trigger is still turnable — turning it walks THROUGH the fire value");
  if (meta("rnd_preset").divable) fail("a trigger still opens a picker");
  if (!M.isTrigger(meta("rnd_preset"))) fail("isTrigger did not recognise access:write");

  /* ---- clicking a trigger FIRES it, through the module wire ---- */
  const writes = [];
  const HIER = JSON.stringify({ modes: null, levels: { root: { label: "S",
      knobs: ["rnd_preset", "detected_key"],
      params: [{ key: "rnd_preset" }, { key: "detected_key" }] } } });
  const CP = JSON.stringify([
    { key: "rnd_preset",   name: "Randomise", type: "enum", options: ["—", "Rnd!"], access: "write" },
    { key: "detected_key", name: "Key",       type: "enum", options: ["C", "C#"],        access: "read"  },
  ]);
  const ctl = C.createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return HIER;
      if (b === "chain_params") return CP;
      if (b === "rnd_preset") return "—";      /* a trigger reports its idle spelling */
      if (b === "detected_key") return "C#";
      return "0";
    },
    setParam: (k, v) => writes.push([k, v]),
    announce: () => {},
  });
  ctl.load({ slot: 0, component: "synth" });
  for (let i = 0; i < 6; i++) ctl.tick();

  const slotOf = (key) => (ctl.page.keys || []).indexOf(key);
  const trigSlot = slotOf("rnd_preset");
  const roSlot   = slotOf("detected_key");
  if (trigSlot < 0 || roSlot < 0) fail("fixture did not put both params on the grid");

  /* Clicking the trigger must WRITE, and must not hand back an open intent. */
  writes.length = 0;
  const intent = ctl.onClick(trigSlot);
  if (intent) fail("clicking a trigger returned an open intent instead of firing");
  if (writes.length !== 1) fail("clicking a trigger wrote " + writes.length + " times, expected 1");
  /* THE bug: the module fires on anything that is not the em-dash, so a bare
   * "0" would mean "do nothing" and a bare "1" is not its spelling either.
   * The module reports names, so the wire must be the NAME of option 1. */
  if (writes[0][1] !== "Rnd!")
    fail("a trigger fired with " + JSON.stringify(writes[0][1]) + ", expected \"Rnd!\" — " +
         "a bare index is exactly what destroys euclidrum kits");

  /* Turning a trigger must do nothing at all. */
  writes.length = 0;
  ctl.onKnobTurn(trigSlot, 1, 5000);
  if (writes.length) fail("turning a trigger wrote " + JSON.stringify(writes));

  /* A readout: click opens nothing, turn writes nothing. */
  writes.length = 0;
  if (ctl.onClick(roSlot)) fail("clicking a readout returned an intent");
  ctl.onKnobTurn(roSlot, 1, 6000);
  if (writes.length) fail("a readout was written to: " + JSON.stringify(writes));

  console.log("  ok  default is readwrite; plain enums and floats unaffected");
  console.log("  ok  readout: not turnable, not divable, never written");
  console.log("  ok  trigger: fires once on click, through the module wire (\"Rnd!\"), not turnable");
  console.log("PASS: access read/write/readwrite");
});
'
