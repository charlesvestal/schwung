#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Every SHIPPED built-in module ships help that the viewer can actually show.
#
# Two mechanisms put 13 modules into a state where they carried kilobytes of
# documentation nobody has ever read, and neither produces an error anywhere:
#
#  1. `if (helpData.children)` is the WHOLE loader test (shadow_ui.js). A
#     help.json that parses but names its topics anything else -- sections,
#     pages, parameters -- is discarded, and the viewer says "No help content
#     available", identical to shipping nothing at all.
#  2. Help lines clip in PIXELS, not characters. drawScrollableText prints at
#     x=4 and set_pixel drops anything past x=127, so the budget is 123px --
#     and load_font trims every glyph to its inked extent, which makes the
#     runtime font PROPORTIONAL even though the atlas is fixed-pitch. A "20
#     character" budget derived from the atlas called 11 shipped lines
#     defective that all fit. Measure pixels.
#
# The font is printable ASCII with no fallback, so a stray dash or ellipsis
# renders as a hole.
#
# gesture-test is excluded deliberately: it is a test fixture ("one knob row of
# every parameter kind"), not a module anyone reaches for. wav-player too -- it
# is a headless preview player driven by the file browser, with no UI and no
# parameters of its own.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node -e '
const fs = require("fs");
const path = require("path");
import("./tools/param-pages/harness.mjs").then((H) => {
  const fb = H.createFramebuffer();
  let failures = 0;
  const fail = (m) => { console.error("FAIL: " + m); failures++; };
  const ok = (m) => { console.error("ok: " + m); };

  /* The help viewer prints at x=4; set_pixel drops past x=127. */
  const BUDGET = 127 - 4;

  /* Modules with no user-facing surface to document. Listed by hand so that
     adding a real module cannot silently inherit an exemption. */
  const EXEMPT = new Set(["gesture-test", "wav-player"]);

  /* What actually ships is what package.sh copies -- read the source tree the
     same way, so a module added under src/modules is covered the day it
     lands rather than the day someone remembers this test. */
  const roots = ["src/modules"];
  const found = [];
  for (const root of roots) {
    for (const cat of fs.readdirSync(root)) {
      const catDir = path.join(root, cat);
      if (!fs.statSync(catDir).isDirectory()) continue;
      if (fs.existsSync(path.join(catDir, "module.json"))) { found.push([cat, catDir]); continue; }
      for (const id of fs.readdirSync(catDir)) {
        const dir = path.join(catDir, id);
        let st; try { st = fs.statSync(dir); } catch (e) { continue; }
        if (!st.isDirectory()) continue;
        if (fs.existsSync(path.join(dir, "module.json"))) found.push([id, dir]);
      }
    }
  }

  /* Source-only test fixtures never reach a device. Their module.json is the
     only thing distinguishing them, so match the names the build excludes. */
  const SOURCE_ONLY = /-test$|^config-test$|^text-test$|^controller$|^store$|^sysex_probe$/;

  let checked = 0;
  for (const [id, dir] of found) {
    if (SOURCE_ONLY.test(id) || EXEMPT.has(id)) continue;
    checked++;
    const hp = path.join(dir, "help.json");
    if (!fs.existsSync(hp)) {
      fail(id + " ships no help.json -- its Module page offers no Module Help row at all");
      continue;
    }
    let d = null;
    try { d = JSON.parse(fs.readFileSync(hp, "utf8")); }
    catch (e) { fail(id + " help.json does not parse: " + e.message); continue; }

    /* Mechanism 1. */
    if (!Array.isArray(d.children) || d.children.length === 0) {
      fail(id + " help.json has no top-level children array -- the loader " +
           "DISCARDS it and the viewer is indistinguishable from shipping nothing. " +
           "Top-level keys were: " + Object.keys(d).slice(0, 5).join(", "));
      continue;
    }

    /* Mechanism 2, plus the font. */
    let over = 0, nonAscii = 0, widest = 0, widestLine = "";
    for (const c of d.children) {
      if (!c || typeof c.title !== "string" || !c.title) { fail(id + " has a topic with no title"); break; }
      if (fb.textWidth(c.title) > BUDGET) { over++; console.error("     over: TITLE " + c.title); }
      for (const s of (c.lines || [])) {
        const w = fb.textWidth(s);
        if (w > widest) { widest = w; widestLine = s; }
        if (w > BUDGET) { over++; console.error("     over: " + w + "px " + JSON.stringify(s)); }
        if (/[^\x20-\x7e]/.test(s)) { nonAscii++; console.error("     non-ascii: " + JSON.stringify(s)); }
      }
    }
    if (over) fail(id + " has " + over + " line(s) past the " + BUDGET + "px the viewer has");
    else if (nonAscii) fail(id + " has " + nonAscii + " non-ASCII line(s) -- the font has no fallback");
    else ok(id + ": " + d.children.length + " topics, widest " + widest + "px");
  }

  if (checked === 0) fail("no modules were checked -- the discovery walk found nothing, so this test proves nothing");
  else ok("checked " + checked + " shipped built-in modules");

  /* The built-in help — Shortcuts, Slots, Master FX and the rest. Same
     viewer, same budget; only the shape of the file differs (a `sections`
     array of {title, lines} rather than `children`). */
  const SHARED_HELP = "src/shared/help_content.json";
  let shared = null;
  try { shared = JSON.parse(fs.readFileSync(SHARED_HELP, "utf8")); }
  catch (e) { fail(SHARED_HELP + " does not parse: " + e.message); }
  if (shared) {
    if (!Array.isArray(shared.sections) || shared.sections.length === 0) {
      fail(SHARED_HELP + " has no sections array");
    } else {
      /* RECURSE. The tree is sections -> children -> {title, lines}, so a
         walker that reads `sec.lines` measures section TITLES and nothing
         else — 64px, comfortably under budget, and every actual help line
         unexamined. That is the shape of a probe reporting green about the
         wrong thing, which is why the count below is asserted too. */
      let over = 0, nonAscii = 0, widest = 0, measured = 0;
      const visit = (node) => {
        if (!node || typeof node !== "object") return;
        for (const line of [].concat(node.title || [], node.lines || [])) {
          if (typeof line !== "string" || line === "") continue;
          measured++;
          const w = fb.textWidth(line);
          if (w > widest) widest = w;
          if (w > BUDGET) { over++; console.error("     over: " + w + "px " + JSON.stringify(line)); }
          if (/[^\x20-\x7e]/.test(line)) { nonAscii++; console.error("     non-ascii: " + JSON.stringify(line)); }
        }
        for (const c of (node.children || [])) visit(c);
      };
      for (const sec of shared.sections) visit(sec);
      /* A floor, so a future restructure that hides the lines from this walk
         fails loudly instead of passing on section titles alone. */
      if (measured < 100) fail(SHARED_HELP + " walk measured only " + measured +
                               " lines — the walker is missing the tree");
      if (over) fail(SHARED_HELP + " has " + over + " line(s) past the " + BUDGET + "px the viewer has");
      else if (nonAscii) fail(SHARED_HELP + " has " + nonAscii + " non-ASCII line(s)");
      else ok("help_content.json: " + measured + " lines, widest " + widest + "px");
    }
  }

  if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
  console.log("PASS");
}).catch((e) => { console.error("FAIL: " + e); process.exit(1); });
'
