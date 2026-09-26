#!/usr/bin/env bash
# Master FX Settings -> Surface Layout (control_editor.mjs): pages listed and
# added, a page renamed / moved / deleted, its knobs listed and cleared --
# and nothing on the screen destroys on a SINGLE click.
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
node --input-type=module -e '
import { createLayoutEditor, createCCEditor } from "./src/shared/control_editor.mjs";
import { emptyControls, addPage, assignKnob } from "./src/shared/control_map.mjs";
let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };
let doc = addPage(addPage(emptyControls(), null, "Drums"), null, "Bass");
doc = assignKnob(doc, 0, 2, { kind: "param", slot: 1, component: "synth", key: "cutoff", module: "obxd", label: "Cutoff" });
const ed = createLayoutEditor({ controls: () => doc, edit: (fn) => { doc = fn(doc); return doc; } });
const labels = () => ed.rows().map((r) => r.label);

eq("pages, then Add Page", labels(), ["Drums", "Bass", "Add Page"]);
eq("each page says how many knobs it drives", ed.rows()[0].value, "1 knobs");
ed.jog(2); ed.click();
eq("Add Page adds one and lands on it", [labels(), ed.cursor], [["Drums", "Bass", "Page 3", "Add Page"], 2]);

ed.jog(-2); ed.click();
eq("a page opens its actions, then its sixteen knobs", [ed.level, ed.rows().length, ed.rows()[6].label], ["page", 20, "Knob 3 S2"]);
eq("a knob names what it drives", ed.rows()[6].value, "Cutoff");
ed.jog(6); ed.click();
eq("one click on a knob only ASKS", [ed.rows()[6].value, !!doc.surface.pages[0].knobs[2]], ["Click: clear", true]);
ed.jog(1); ed.jog(-1); ed.click();
eq("moving the cursor drops the question", !!doc.surface.pages[0].knobs[2], true);
ed.click(); ed.click();
eq("the second click clears", doc.surface.pages[0].knobs[2], null);

ed.jog(-6);
eq("Rename asks the host for a keyboard", ed.click(), { rename: { page: 0, name: "Drums" } });
ed.rename(0, "Kit");
eq("...and the name lands", doc.surface.pages[0].name, "Kit");
ed.jog(2); ed.click();
eq("Move Later moves the page and follows it", [doc.surface.pages.map((p) => p.name), ed.page], [["Bass", "Kit", "Page 3"], 1]);
ed.jog(1); ed.click();
eq("Delete Page asks first", doc.surface.pages.length, 3);
eq("Back drops the question, not the screen", [ed.back(), ed.level], [false, "page"]);
ed.click(); ed.click();
eq("the second click deletes, back to the list", [doc.surface.pages.map((p) => p.name), ed.level], [["Bass", "Page 3"], "pages"]);
eq("Back at the top leaves", ed.back(), true);
/* ---- the CC Map editor: Back must NOT cancel a learn -- you leave the
 * screen to find the parameter (hardware, 2026-09-26: learn never worked). */
{
  let learning = null, cancelled = 0;
  const ccMap = { get learning() { return learning; }, beginLearn() { learning = { cc: null, target: null }; },
                  cancelLearn() { learning = null; cancelled++; }, reload() {} };
  const cdoc = { cc: [] };
  const ce = createCCEditor({ controls: () => cdoc, edit: () => cdoc, ccMap });
  eq("the list ends with Learn New", ce.rows().map((r) => r.label), ["Learn New"]);
  ce.click();
  eq("clicking starts learn, and the row says what it waits for, SHORT",
     [ce.rows()[0].label, ce.rows()[0].value], ["Cancel Learn", "knob+param?"]);
  eq("Back leaves the screen...", ce.back(), true);
  eq("...and learn keeps running", [!!learning, cancelled], [true, 0]);
  ce.click();
  eq("Cancel Learn cancels", [learning, cancelled], [null, 1]);
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
