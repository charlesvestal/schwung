#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# EVERY SHIPPED help.json MUST BE READABLE, AND THE FAILURE IS SILENT.
#
# The loader's whole test is `if (helpData.children) helpMap[id] = ...`. A file
# that parses as valid JSON but names its topics anything else -- `sections`,
# `pages`, `parameters` -- is discarded WITHOUT A WORD, and the viewer says "No
# help content available" as though the file were absent. A 2026-08 catalog
# sweep found 12 modules in exactly that state, several carrying kilobytes of
# help nobody could read.
#
# This repo had one too: widget-test, the reference module for the three
# module-supplied draw surfaces, shipped `sections` and its help had never once
# been displayed. A module author copying the reference copies the bug, which is
# how this class of thing spreads.
#
# Also measures LINE WIDTH in pixels against the device font, because a help
# line is drawn, never wrapped and never truncated: everything past x=127 is
# dropped by set_pixel with no error anywhere.
#
# NO APOSTROPHES BELOW THIS LINE inside the node script.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
const R = process.cwd();
const fs = await import("node:fs");
const path = await import("node:path");
const { createFramebuffer } = await import(R + "/tools/param-pages/harness.mjs");

let fails = 0;
const fail = (m) => { console.error("FAIL: " + m); fails++; };

/* The real atlas, so this measures what the panel would draw. */
const fb = createFramebuffer(128, 64);

/* drawScrollableText prints at x=4, and the panel is 128 wide. */
const BUDGET = 128 - 4;

function walk(dir, out) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (e.name === "help.json") out.push(p);
  }
  return out;
}

const files = walk(path.join(R, "src", "modules"), []);
if (!files.length) fail("no help.json found under src/modules -- did the layout move?");

for (const f of files) {
  const rel = path.relative(R, f);
  let d;
  try { d = JSON.parse(fs.readFileSync(f, "utf8")); }
  catch (e) { fail(`${rel} is not valid JSON: ${e}`); continue; }

  if (!Array.isArray(d.children) || d.children.length === 0) {
    fail(`${rel} has no top-level children[] -- the loader discards it silently ` +
         `(keys: ${Object.keys(d).join(", ")})`);
    continue;
  }

  /* Every leaf line, measured. */
  const over = [];
  const seen = new Set();
  (function rec(nodes, trail) {
    for (const n of nodes || []) {
      if (!n || typeof n !== "object") continue;
      const where = trail ? `${trail} > ${n.title || "?"}` : (n.title || "?");
      if (Array.isArray(n.lines)) {
        for (const ln of n.lines) {
          const s = String(ln);
          const w = fb.textWidth(s);
          if (w > BUDGET) over.push(`${where}: ${w}px "${s}"`);
          for (const ch of s) if (ch.charCodeAt(0) > 126) seen.add(ch);
        }
      }
      if (Array.isArray(n.children)) rec(n.children, where);
    }
  })(d.children, "");

  if (over.length) {
    fail(`${rel} has ${over.length} line(s) running off the display:`);
    for (const o of over.slice(0, 5)) console.error("       " + o);
  }
  if (seen.size) {
    fail(`${rel} uses characters the bitmap font has no glyph for: ${[...seen].join(" ")}`);
  }
}

if (fails) { console.error(fails + " failure(s)"); process.exit(1); }
console.log(`PASS: ${files.length} help.json files load, fit the display, and stay in ASCII`);
'
