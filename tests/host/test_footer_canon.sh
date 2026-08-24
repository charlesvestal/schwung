#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The NEW footer's vocabulary canon.
#
# The OLD text footer has had one since the 2026-06 cleanup
# (tests/shadow/test_footer_verb_consistency.sh), written because it had
# drifted into verb soup: "Push:" and "Click:" for the same jog click, "Exit"
# and "exit" for the same word. The new pill footer never got the equivalent,
# and it had already started down the same road.
#
# Two rules, and only the second needed a judgement call:
#
#   1. KEYS name a physical control, so the set is closed by the hardware.
#   2. BACK's ACTION says where BACK goes. EXIT leaves the view; OUT rises one
#      level within it. Both are kept ON PURPOSE - see FOOTER_CANON. A third
#      synonym (RTRN, UP, BACK) is drift and fails here.
#
# Reads the sources with node rather than rg, which is not installed on the
# dev machines and is why ~32 tests/host scripts fail there.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node -e '
const fs = require("fs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const CANON_SRC = "src/shared/param_pages/render_page_movy.mjs";
const canonText = fs.readFileSync(CANON_SRC, "utf8");

/* The canon must be declared where the renderer lives, so it is found by
 * anyone adding a hint rather than living only in a test. */
if (!/export const FOOTER_CANON/.test(canonText)) {
  fail(CANON_SRC + " does not export FOOTER_CANON");
  process.exit(1);
}
const keys = (canonText.match(/keys: Object\.freeze\(\[([^\]]*)\]/) || [])[1] || "";
const backs = (canonText.match(/backActions: Object\.freeze\(\[([^\]]*)\]/) || [])[1] || "";
const KEYS = keys.match(/"([A-Z]+)"/g) || [];
const BACKS = backs.match(/"([A-Z]+)"/g) || [];
const strip = (a) => a.map((s) => s.replace(/"/g, ""));
const KEYSET = new Set(strip(KEYS));
const BACKSET = new Set(strip(BACKS));

if (!KEYSET.size) fail("FOOTER_CANON.keys parsed empty");
if (!BACKSET.has("EXIT") || !BACKSET.has("OUT")) {
  fail("FOOTER_CANON.backActions must keep EXIT and OUT - they mean different " +
       "things (leaves the view vs rises one level), and collapsing them tells " +
       "the user Back does one thing when it does two");
}

/* Files that draw the NEW footer. The old text footer is out of scope: it has
 * its own canon and its own test, and it is on its way out. */
const FILES = [
  "src/shadow/shadow_ui.js",
  "src/shadow/shadow_ui_param_pages.mjs",
  "src/shadow/shadow_ui_presets.mjs",
];

/* A hint pair literal, and the orderedHints({jog, click}) spelling. */
const PAIR = /\[\s*"([A-Z]+)"\s*,\s*"([A-Z0-9]+)"\s*\]/g;

let seenBack = 0, seenPairs = 0;
const emptyCalls = [];
const allCalls = [];
for (const f of FILES) {
  const s = fs.readFileSync(f, "utf8");
  /* Only look inside footer calls, so enum option pairs like ["OFF","ON"] and
   * ["UNI","BI"] are not mistaken for hints - they are values, not gestures.
   *
   * The call is extracted by BALANCING PARENS, not by a lazy regex. A lazy
   * `[\s\S]*?\)` stops at the first closing paren, and the chain editor calls
   * `drawMovyFooter(movy, isShiftHeld() ? [...] : [...])` - so the match ended
   * at isShiftHeld() and the hints were never scanned. That hole let a
   * deliberately planted non-canonical key pass. */
  const calls = [];
  const open = /(drawMovyFooter|drawFooter|orderedHints)\s*\(/g;
  let o;
  while ((o = open.exec(s)) !== null) {
    let depth = 0, i = o.index + o[0].length - 1;
    for (; i < s.length; i++) {
      if (s[i] === "(") depth++;
      else if (s[i] === ")") { depth--; if (depth === 0) break; }
    }
    calls.push(s.slice(o.index, i + 1));
    allCalls.push(s.slice(o.index, i + 1));
  }
  for (const call of calls) {
    let m, hereBefore = seenPairs;
    PAIR.lastIndex = 0;
    while ((m = PAIR.exec(call)) !== null) {
      const [, key, action] = m;
      seenPairs++;
      if (!KEYSET.has(key)) {
        fail(`${f}: hint key ${key} is not in FOOTER_CANON.keys [` +
             [...KEYSET].join(", ") + "] - keys name a physical control");
      }
      if (key === "BACK") {
        seenBack++;
        if (!BACKSET.has(action)) {
          fail(`${f}: BACK action ${action} is not canon. Use EXIT when ` +
               "Back leaves the view, OUT when it rises one level within it");
        }
      }
    }
    /* The probe for "this call HAS a hint literal" must look for a movy PAIR,
     * i.e. an ALL-CAPS key followed by a comma or a close. A looser `["[A-Z]`
     * also matches the OLD text footer, whose drawMenuFooter is imported into
     * shadow_ui.js under the alias `drawFooter` and now takes an ordered array
     * of hint strings: `drawFooter(["Back: exit", "Jog: select"])`. Those are
     * sentence-case hints for a different renderer, not pairs, so yielding no
     * pair from them is correct and flagging it is a false alarm. */
    if (seenPairs === hereBefore && /\[\s*"[A-Z0-9]+"\s*[,\]]/.test(call)) emptyCalls.push(call);
  }
}

/* Guard the guard: if the scan stops matching, every assertion above passes
 * vacuously and the canon quietly stops being enforced.
 *
 * A total-count floor is a weak version of this - the truncation bug that this
 * scanner replaced still yielded 18 pairs and sailed past a floor of 8. So the
 * real check is per-call: any footer call containing a pair literal must
 * actually YIELD a pair. That catches a truncated extraction structurally,
 * whatever the totals happen to be. */
for (const c of emptyCalls) {
  fail("a footer call contains a hint literal but the scan extracted nothing " +
       "from it - the extraction is truncating: " + c.slice(0, 90).replace(/\s+/g, " "));
}
/*
 * The load-bearing one. A truncated capture is UNBALANCED - the lazy regex
 * that this replaced captured `drawMovyFooter(movy, isShiftHeld()`, which has
 * two opens and one close. Checking balance catches that directly, where the
 * emptyCalls check above does not: the truncation cut the text off BEFORE the
 * hint literal, so that call looked like it simply had no hints in it.
 */
for (const c of allCalls) {
  const opens = (c.match(/\(/g) || []).length;
  const closes = (c.match(/\)/g) || []).length;
  if (opens !== closes) {
    fail("a captured footer call has unbalanced parens (" + opens + " open, " +
         closes + " close) - the extraction is truncating and everything past " +
         "the cut is unchecked: " + c.slice(0, 90).replace(/\s+/g, " "));
  }
}
if (seenPairs < 8) fail("only found " + seenPairs + " hint pairs - the scan is not matching the call sites");
if (seenBack < 4)  fail("only found " + seenBack + " BACK hints - the scan is not matching them");

if (failures) process.exit(1);
console.log("PASS: footer canon - " + seenPairs + " hints, " + seenBack +
            " BACK; keys are canonical and BACK says where it goes");
'
