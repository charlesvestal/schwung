#!/usr/bin/env bash
#
# AN LFO CAN TARGET THE SLOT'S SEND AMOUNTS.
#
# Two halves, and the C half is the one with a data-loss trap in it.
#
# The picker: send amounts are not a component's chain_params, so the generic
# read cannot answer for them -- chainComponentParamKey refuses a key that is
# not a chain position. That is why an LFO could already target another LFO only
# through a hardcoded short-circuit, and the sends follow the same precedent.
#
# The DSP: the LFO-to-LFO path writes its target field directly and snapshots a
# base to put back. Copying that here would be a DATA-LOSS bug, not a style
# difference -- `buses:main_send<N>` is read back by saveSendLevels(), which
# writes what it reads to send_levels.json, so an autosave landing while the LFO
# was at the top of its cycle would persist the modulated number as the user's
# level, permanently, with the LFO still sweeping over it. So the contribution
# is an OFFSET applied at the drain and the base is never written.
set -euo pipefail

cd "$(dirname "$0")/../.."
UI="src/shadow/shadow_ui.js"
HOST="src/modules/chain/dsp/chain_host.c"
[ -f "$UI" ] && [ -f "$HOST" ] || { echo "FAIL: missing sources"; exit 1; }

node --input-type=module -e '
import { readFileSync } from "node:fs";
const src = readFileSync(process.argv[1], "utf8");
const host = readFileSync(process.argv[2], "utf8");
let bad = 0;
const fail = (m) => { console.log("FAIL: " + m); bad = 1; };
const ok = (m) => console.log("  ok  " + m);
const grab = (n) => {
  const m = src.match(new RegExp("^function " + n + "\\([^]*?^}", "m"));
  if (!m) { fail("could not lift " + n + "()"); process.exit(1); }
  return m[0];
};

/* ---- the picker offers them, and can answer for them ------------------ */
{
  const body = [
    "const SEND_TARGET_PARAMS = " +
      JSON.stringify([{ key: "main_send1", label: "Send A" },
                      { key: "main_send2", label: "Send B" }]) + ";",
    "const SENDS_LFO_TARGET_KEY = \"buses\";",
    "const LFO_TARGET_PARAMS = [];",
    "function chainTargetGetParam() { return null; }",
    "function flatLfoTargetParams() { return []; }",
    "function debugLog() {}",
    grab("lfoTargetParamsFor"), grab("lfoTargetGroupsFor"),
    "return { p: lfoTargetParamsFor(null, \"buses\", \"t\"),",
    "         g: lfoTargetGroupsFor(null, \"buses\", \"t\") };",
  ].join("\n");
  const r = new Function(body)();
  const keys = r.p.map((x) => x.key);
  if (JSON.stringify(keys) !== JSON.stringify(["main_send1", "main_send2"])) {
    fail("the sends target offers " + JSON.stringify(keys)
         + ", expected the two main_send keys");
  }
  /* The generic path returns [] here (chainTargetGetParam answers null), so a
     non-empty answer proves the short-circuit ran rather than the fallback. */
  if (!r.p.length) {
    fail("the sends target fell through to the chain_params read, which cannot "
         + "answer for a key that is not a chain position");
  }
  if (r.g.grouped !== false || r.g.flat.length !== 2) {
    fail("two rows must not be grouped -- a group step over two entries is a "
         + "screen you click through to reach a screen");
  }
  ok("the picker answers for the sends target without a chain_params read");
}

/* The row has to be OFFERED, or nothing above is reachable. */
if (!/comps\.push\(\{\s*key:\s*SENDS_LFO_TARGET_KEY/.test(src)) {
  fail("getTargetComponents never offers the sends row");
}
ok("the slot LFO picker offers a Sends component");

/* ---- the DSP: an OFFSET, never a write to the base -------------------- */
{
  /*
   * TWO things this regex has to get right, and the first version got both
   * wrong. `\n}` stops at the first INNER block, not the function, because a C
   * function closes at column 0 -- so the body was truncated and the assertions
   * below passed over text that did not contain the code they were checking.
   * And `^static void mod_tick\(` alone matches the FORWARD DECLARATION 1300
   * lines earlier, whose body then runs to the next column-0 brace: a 33k-char
   * "body" with no main_send_mod in it at all. Requiring the opening brace is
   * what separates the definition from the declaration.
   */
  const m = host.match(/^static void mod_tick\([^)]*\)\s*\{[^]*?^}/m);
  if (!m) { fail("could not find mod_tick in chain_host.c"); }
  else {
    const t = m[0];
    if (/main_send_level\s*\[[^\]]*\]\s*=/.test(t)) {
      fail("mod_tick ASSIGNS main_send_level. That value is read back by "
           + "saveSendLevels() and written to send_levels.json, so a modulated "
           + "number would be persisted as the user setting.");
    }
    if (!/main_send_mod\s*\[[^\]]*\]\s*\+=/.test(t)) {
      fail("mod_tick does not ADD into main_send_mod -- two LFOs on one send "
           + "must sum, and an assignment lets the second clobber the first");
    }
    /* ZEROED EVERY BLOCK. Without it a disabled or retargeted LFO leaves its
       last offset in place forever: a send permanently detuned from the number
       on screen, with no gesture that puts it back. */
    const zero = /for\s*\(int sd = 0; sd < BUS_MIX_SENDS; sd\+\+\)\s*inst->main_send_mod\[sd\] = 0;/;
    if (!zero.test(t)) {
      fail("mod_tick does not zero main_send_mod at the top of the block, so a "
           + "disabled or retargeted LFO leaves its offset stuck");
    }
    /* and the zero must come BEFORE the per-route loop, or it wipes the result.
     *
     * The marker is looked up SEPARATELY and its absence is its own failure.
     * This was one expression using indexOf, and when the loop bound was
     * renamed LFO_COUNT -> MOD_ROUTE_COUNT the lookup returned -1, so
     * `search(zero) > -1` was trivially true and the test failed for a reason
     * that had nothing to do with the ordering it was checking. An assertion
     * whose landmark can vanish must say so rather than compare against -1.
     *
     * (No apostrophes anywhere in this file: the whole script is a single-
     * quoted -e argument, so one ends it mid-expression.) */
    const loopAt = t.indexOf("for (int i = 0; i < MOD_ROUTE_COUNT");
    if (loopAt < 0) {
      fail("could not find the per-route loop in mod_tick -- the ordering "
           + "check here has no landmark to compare against");
    } else if (t.search(zero) > loopAt) {
      fail("main_send_mod is zeroed INSIDE or after the route loop, which "
           + "erases the contribution it was meant to reset");
    }
    if (!bad) ok("mod_tick adds an offset, sums, and re-zeroes it every block");
  }
}

/* The drain must actually READ the offset, or none of it is audible. */
{
  const m = host.match(/^void chain_drain_main_send\([^)]*\)\s*\{[^]*?^}/m);
  if (!m) fail("could not find chain_drain_main_send");
  else if (!/main_send_level\[sd\]\s*\+\s*inst->main_send_mod\[sd\]/.test(m[0])) {
    fail("chain_drain_main_send does not add main_send_mod -- the LFO would "
         + "compute an offset nothing listens to");
  } else if (!/BUS_MIX_SEND_LEVEL_MAX/.test(m[0])) {
    fail("the summed amount is not clamped");
  } else if (!bad) ok("the drain adds the offset to the base and clamps");
}

if (bad) process.exit(1);
console.log("PASS");
' "$UI" "$HOST"
