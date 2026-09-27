#!/usr/bin/env bash
# The control foundation (control_target.mjs, control_map.mjs): what a control
# drives, and the per-set document that holds the Custom layout's pages and,
# next, the CC map's bindings.
#
#   - a target is validated and addressed one way (the grid's own keys)
#   - learn turns a write on Move into a target -- and refuses what is not a
#     parameter (a module swap, a state blob) and an unknown module
#   - the document is TOLERANT: bad knobs empty, bad pages dropped, junk text
#     refused with ok:false so the caller does not save over it
#   - what this build does not understand (the cc section, any other key) is
#     carried through a save VERBATIM
#   - edits return new documents and respect the 16-page cap
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import * as T from "./src/shared/control_target.mjs";
import * as M from "./src/shared/control_map.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };

/* ---- targets ---- */
const cut = { kind: "param", slot: 1, component: "synth", key: "cutoff", module: "obxd", label: "Cutoff" };
eq("a param target is kept", T.normalizeTarget(cut), cut);
eq("...and addressed as the grid addresses it", T.targetAddress(cut), { slot: 1, key: "synth:cutoff" });
eq("a Master FX target is addressed at slot 0",
   T.targetAddress({ kind: "master", fx: 3, key: "mix", module: "cloudseed" }), { slot: 0, key: "master_fx:fx3:mix" });
eq("a master setting is addressed at slot 0",
   T.targetAddress(T.normalizeTarget({ kind: "setting", slot: null, key: "master_fx:filter" })), { slot: 0, key: "master_fx:filter" });
eq("a bad slot is refused", T.normalizeTarget(Object.assign({}, cut, { slot: 4 })), null);
eq("a key with a colon is refused", T.normalizeTarget(Object.assign({}, cut, { key: "a:b" })), null);
eq("module identity is never a knob", T.normalizeTarget(Object.assign({}, cut, { key: "module" })), null);
eq("a param with no module is refused", T.normalizeTarget(Object.assign({}, cut, { module: "" })), null);
eq("an arbitrary slot key is not a setting", T.normalizeTarget({ kind: "setting", slot: 0, key: "slot:receive_channel" }), null);
eq("an unknown kind is null, not an error", T.normalizeTarget({ kind: "sendfx" }), null);
eq("same position and key is the same target, whatever the label",
   T.sameTarget(cut, Object.assign({}, cut, { label: "x", module: "other" })), true);

/* ---- learn: a write on Move, as a target ---- */
const at = (m) => (scope) => m[JSON.stringify(scope)] || null;
const modules = at({ [JSON.stringify({ slot: 2, component: "fx1" })]: "freeverb", [JSON.stringify({ fx: 2 })]: "cloudseed" });
eq("a module parameter write learns a param target",
   T.targetFromWrite(2, "fx1:room", modules), { kind: "param", slot: 2, component: "fx1", key: "room", module: "freeverb", label: "" });
eq("a Master FX write learns a master target",
   T.targetFromWrite(0, "master_fx:fx2:size", modules), { kind: "master", fx: 2, key: "size", module: "cloudseed", label: "" });
eq("a slot volume write learns a setting", T.targetFromWrite(3, "slot:volume", modules),
   { kind: "setting", slot: 3, key: "slot:volume", label: "" });
eq("the master filter learns a master setting", T.targetFromWrite(0, "master_fx:filter", modules),
   { kind: "setting", slot: null, key: "master_fx:filter", label: "" });
eq("a module SWAP is not learned", T.targetFromWrite(2, "fx1:module", modules), null);
eq("a module that did not answer is not learned", T.targetFromWrite(1, "synth:cutoff", modules), null);
eq("a key that is no component is not learned", T.targetFromWrite(0, "lanes:record", modules), null);

/* ---- the document ---- */
eq("no file is an empty document", M.parseControls(null), { doc: M.emptyControls(), ok: true });
const junk = M.parseControls("{ not json");
eq("junk is refused, so the caller does not save over it", [junk.ok, junk.doc.surface.pages.length], [false, 0]);

let doc = M.emptyControls();
doc = M.addPage(doc, null, "Drums");
doc = M.assignKnob(doc, 0, 5, cut);
doc = M.addPage(doc, 0, "  ");
eq("pages insert where asked; a blank name is named for its place",
   doc.surface.pages.map((p) => p.name), ["Page 1", "Drums"]);
eq("the knob landed", doc.surface.pages[1].knobs[5], cut);
eq("an invalid target is refused, the knob unchanged",
   M.assignKnob(doc, 1, 5, { kind: "param" }).surface.pages[1].knobs[5], cut);
const before = doc;
M.clearKnob(doc, 1, 5);
eq("edits return new documents; the old one is untouched", before.surface.pages[1].knobs[5], cut);
eq("move", M.movePage(doc, 1, 0).surface.pages.map((p) => p.name), ["Drums", "Page 1"]);
eq("delete", M.deletePage(doc, 0).surface.pages.map((p) => p.name), ["Drums"]);
let full = M.emptyControls();
for (let i = 0; i < 20; i++) full = M.addPage(full);
eq("sixteen pages at most", full.surface.pages.length, M.MAX_PAGES);

/* round trip, preserving what this build does not understand */
const written = JSON.parse(M.serializeControls(doc));
written.cc = [{ cable: 2, channel: 0, cc: 74, mode: "abs", target: cut }];
written.future = { anything: 1 };
written.surface.pages.push({ name: "Bad", knobs: "nope" }, 7);
written.surface.pages[1].knobs[6] = { kind: "sendfx", x: 1 };
const back = M.parseControls(JSON.stringify(written));
eq("round trip keeps pages and targets", back.doc.surface.pages[1].knobs[5], cut);
eq("a bad knob loads empty", back.doc.surface.pages[1].knobs[6], null);
eq("a page with bad knobs keeps its name; a non-page is dropped",
   back.doc.surface.pages.map((p) => p.name), ["Page 1", "Drums", "Bad"]);
const again = JSON.parse(M.serializeControls(back.doc));
eq("the cc section round-trips", again.cc, written.cc);
eq("...and so does any other key", again.future, { anything: 1 });

/* ---- the CC map bindings ---- */
let c = M.emptyControls();
c = M.bindCC(c, { channel: 0, cc: 74, mode: "abs", target: cut });
c = M.bindCC(c, { channel: 0, cc: 74, mode: "rel", target: Object.assign({}, cut, { key: "reso" }) });
eq("one binding per (channel, cc): a new one replaces", [c.cc.length, c.cc[0].target.key, c.cc[0].mode], [1, "reso", "rel"]);
eq("a binding with a bad CC is refused", M.bindCC(c, { channel: 0, cc: 200, target: cut }).cc.length, 1);
eq("an unknown mode defaults to absolute", M.bindCC(M.emptyControls(), { channel: 1, cc: 1, mode: "x", target: cut }).cc[0].mode, "abs");
eq("mode toggles", M.setCCMode(c, 0, "abs").cc[0].mode, "abs");
eq("unbind", M.unbindCC(c, 0).cc.length, 0);
const kept = M.addPage(c, null, "P");
eq("a page edit keeps the bindings", kept.cc.length, 1);
eq("junk bindings load as nothing", M.parseControls(JSON.stringify({ version: 1, cc: [{ channel: 99 }, 5] })).doc.cc, []);

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
