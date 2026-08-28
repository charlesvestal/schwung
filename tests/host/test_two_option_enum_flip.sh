#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A TWO-OPTION enum is FLIPPED by the click, not opened.
#
# The picker earns its place on a Recv Ch with seventeen options or a Braids
# model with forty-seven. Over two options it is a menu whose entire content is
# the value already visible in the cell and the one other value there is, and it
# charges two gestures for a state one gesture can describe. Reported from the
# device against Mirror Display and Move->Schwung: "if an option has two values,
# clicking it should change the option ... we dont need a whole menu for two
# items".
#
# What this pins, and why each half is here:
#
#   1. the flip itself, BOTH WAYS. One direction passes with a hard-coded write.
#   2. three options still OPEN. Without this the fix is indistinguishable from
#      deleting the picker.
#   3. a TRIGGER still fires and a READOUT still does nothing. Both are
#      two-option enums in the wire format — euclidrum's ["—","Rnd!"] is the
#      dangerous one — so a predicate written on the option count ALONE turns
#      every momentary in the fleet into a latch that destroys a kit on the way
#      past. `divable` is what excludes them, which is why flipsOnClick requires
#      it rather than testing options.length in isolation.
#   4. the FOOTER says FLIP before it can say OPEN. A two-option enum is still
#      divable, so the OPEN branch would claim it if it came first, and the
#      footer would promise a screen the click never shows. That exact
#      promise-versus-behaviour drift is written up twice already in
#      paramPagesFooterHints.
#   5. the LIST editor flips too. The same parameter must not answer the same
#      gesture two different ways depending on the Param View setting.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node -e '
const fs = require("fs");
Promise.all([
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_pages/page_controller.mjs"),
]).then(async ([M, C]) => {
  let failures = 0;
  const fail = (m) => { console.error("FAIL: " + m); failures++; };

  /* ---- 1. the predicate ------------------------------------------------ */
  const ix = M.buildMetaIndex({ chainParams: [
    { key: "flip2",  type: "enum",  options: ["Off", "On"] },
    { key: "pick3",  type: "enum",  options: ["A", "B", "C"] },
    { key: "trig2",  type: "enum",  options: ["—", "Rnd!"], access: "write" },
    { key: "ro2",    type: "enum",  options: ["No", "Yes"],      access: "read"  },
    { key: "cutoff", type: "float", min: 0, max: 1 },
  ]});
  const meta = (k) => ix.getOrGuess(k);

  if (!M.flipsOnClick(meta("flip2"))) fail("a two-option enum should flip on click");
  if (M.flipsOnClick(meta("pick3")))  fail("a three-option enum must still open its list");
  if (M.flipsOnClick(meta("trig2")))
    fail("a TRIGGER is a two-option enum on the wire — flipping it would latch every " +
         "momentary in the fleet");
  if (M.flipsOnClick(meta("ro2")))    fail("a readout has nothing to set");
  if (M.flipsOnClick(meta("cutoff"))) fail("a float is not an enum");
  if (M.flipsOnClick(null))           fail("flipsOnClick(null) should be false");

  /* ---- 2..3. drive the real controller --------------------------------- */
  const store = { flip2: "Off", pick3: "A", trig2: "—", ro2: "No" };
  const writes = [];
  const spoken = [];
  const HIER = JSON.stringify({ modes: null, levels: { root: { label: "S",
      knobs: ["flip2", "pick3", "trig2", "ro2"],
      params: [{ key: "flip2" }, { key: "pick3" }, { key: "trig2" }, { key: "ro2" }] } } });
  const CP = JSON.stringify([
    { key: "flip2", name: "Mirror",  type: "enum", options: ["Off", "On"] },
    { key: "pick3", name: "Overlay", type: "enum", options: ["A", "B", "C"] },
    { key: "trig2", name: "Rnd",     type: "enum", options: ["—", "Rnd!"], access: "write" },
    { key: "ro2",   name: "Key",     type: "enum", options: ["No", "Yes"],      access: "read"  },
  ]);
  const ctl = C.createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return HIER;
      if (b === "chain_params") return CP;
      return b in store ? store[b] : "0";
    },
    setParam: (k, v) => { writes.push([String(k).replace(/^[^:]+:/, ""), v]); },
    announce: (t) => spoken.push(String(t)),
  });
  ctl.load({ slot: 0, component: "synth" });
  for (let i = 0; i < 8; i++) ctl.tick();

  const slotOf = (key) => (ctl.page.keys || []).indexOf(key);
  const s2 = slotOf("flip2"), s3 = slotOf("pick3");
  const sT = slotOf("trig2"), sR = slotOf("ro2");
  if (s2 < 0 || s3 < 0 || sT < 0 || sR < 0) fail("fixture did not put all four params on the grid");

  /* Off -> On. */
  writes.length = 0; spoken.length = 0;
  if (ctl.onClick(s2) !== null)
    fail("clicking a two-option enum handed back an open intent — the picker is still being raised");
  if (writes.length !== 1) fail("clicking a two-option enum wrote " + writes.length + " times, expected 1");
  if (writes[0][0] !== "flip2" || writes[0][1] !== "On")
    fail("the flip wrote " + JSON.stringify(writes[0]) + ", expected [flip2, On]");
  if (!spoken.some((t) => /On/.test(t)))
    fail("the flip announced nothing containing the new value — with TTS on the screen " +
         "reader is the only report the user gets");

  /* ...and BACK. A hard-coded "write option 1" passes the half above. */
  store.flip2 = "On";
  writes.length = 0;
  ctl.onClick(s2);
  if (writes.length !== 1 || writes[0][1] !== "Off")
    fail("the second click wrote " + JSON.stringify(writes) + ", expected Off — a flip must " +
         "return, or the control is a one-way write");

  /* Three options: unchanged, and the intent still carries the list. */
  writes.length = 0;
  const open = ctl.onClick(s3);
  if (!open || open.action !== "open") fail("a three-option enum stopped opening its picker");
  if (!Array.isArray(open.options) || open.options.length !== 3)
    fail("the open intent lost its option list");
  if (writes.length) fail("opening a picker wrote " + JSON.stringify(writes) + " — nothing may be " +
                          "written on the way in, or Back stops being a cancel");

  /* A trigger fires through the module wire; it does not flip. */
  writes.length = 0;
  if (ctl.onClick(sT) !== null) fail("clicking a trigger handed back an open intent");
  if (writes.length !== 1 || writes[0][1] !== "Rnd!")
    fail("clicking a trigger wrote " + JSON.stringify(writes) + ", expected [trig2, Rnd!]");

  /* A readout: nothing at all. */
  writes.length = 0;
  ctl.onClick(sR);
  if (writes.length) fail("clicking a readout wrote " + JSON.stringify(writes));

  /* ---- 4. the FOOTER promises FLIP, and does so BEFORE it can say OPEN -- */
  const FOOT = "src/shadow/shadow_ui_param_pages.mjs";
  const foot = fs.readFileSync(FOOT, "utf8");
  if (!/import \{[^}]*\bflipsOnClick\b[^}]*\}/.test(foot))
    fail(FOOT + " must IMPORT flipsOnClick, not restate the rule — two copies is how the " +
         "footer and the click come apart");
  const iFlip = foot.indexOf("click: \"FLIP\"");
  const iOpen = foot.indexOf("click: \"OPEN\"", foot.indexOf("meta.divable"));
  if (iFlip < 0) fail(FOOT + " no longer offers a FLIP hint");
  if (iOpen < 0) fail(FOOT + " lost the divable OPEN hint");
  if (iFlip > iOpen)
    fail("the FLIP branch must come BEFORE the divable OPEN branch — a two-option enum is " +
         "still divable, so OPEN would claim it and the footer would advertise a screen the " +
         "click never shows");

  /* ---- 5. the LIST editor flips the same parameter ---------------------- */
  const UI = "src/shadow/shadow_ui.js";
  const ui = fs.readFileSync(UI, "utf8");
  /* Anchored on the hierarchy editor'"'"'s OWN enum branch, not on the first
   * `type === "enum"` in the file. The loose anchor landed on
   * isTriggerEnumMeta 1500 lines earlier, whose own `options.length === 2`
   * satisfied the search — so deleting the flip left the probe GREEN. */
  const enumBranch = ui.indexOf("!hierEditorEditMode && meta && meta.type === \"enum\" &&");
  if (enumBranch < 0) fail(UI + " no longer has the hierarchy-editor enum branch");
  const picker = ui.indexOf("openEnumPicker({", enumBranch);
  const flip = ui.indexOf("meta.options.length === 2", enumBranch);
  if (flip < 0 || flip > picker)
    fail("the hierarchy editor still raises the picker for a two-option enum — the same " +
         "parameter would answer the same gesture differently depending on Param View");

  if (failures) process.exit(1);
  console.log("PASS: a two-option enum flips on click on both editors; three options still open; " +
              "triggers and readouts are untouched");
}).catch((e) => { console.error("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
