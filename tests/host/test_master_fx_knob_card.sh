#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The knob card on Master FX: the two things about it that fail SILENTLY.
#
# 1. THE MIRROR. drawMasterFx is a dispatcher -- nine flags can put a text
#    entry, a confirm, a help page, the preset browser, the settings menu or
#    the module picker in FRONT of the chain diagram, and it early-returns into
#    each. The card is a modal over the diagram and is drawn on that last path
#    only, so the touch handler must not raise one while something is covering
#    it: the card would be state-set and never drawn, and the centred
#    name/value box it replaces would not be shown either. The knob would
#    simply give no feedback. masterFxChainDiagramVisible() in shadow_ui.js is
#    that guard, and it is a SECOND COPY of the dispatch list. A tenth
#    sub-screen added to one and not the other is invisible in review and
#    invisible on screen until somebody touches a knob at the wrong moment.
#    So both lists are derived from source here and compared.
#
# 2. THE VIEW TEST. The card shipped 2026-08-20 gated on
#    `view !== VIEWS.CHAIN_EDIT`, which made it a slot-chain-only feature one
#    day after it landed -- the concrete example section 1b of the Master FX
#    variable-length design exists to end. The gate is now chainEditorFocus(),
#    which answers for BOTH chains, and reintroducing a view test in the card
#    block would silently take Master FX back out.
#
# Neither of these is a pixel, so the snapshot cannot see them.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node -e '
const fs = require("fs");
let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };

const ui = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const mfx = fs.readFileSync("src/shadow/shadow_ui_master_fx.mjs", "utf8");

const slice = (src, from, what) => {
  const at = src.indexOf(from);
  if (at < 0) { fail(what + " is gone"); return ""; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + what); return ""; }
  return src.slice(at, end + 2);
};
/* Comments carry the flag names too, and a name mentioned in prose is not a
   branch. Strip them before reading conditions. */
const decomment = (s) => s.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
const norm = (c) => c.replace(/\s+/g, "");

/* ---- 1. the two lists of "something is covering the diagram" ----------- */

const guard = decomment(slice(ui, "function masterFxChainDiagramVisible()",
                              "masterFxChainDiagramVisible"));
const guardConds = [];
for (const m of guard.matchAll(/if\s*\(([^)]*(?:\([^)]*\))?[^)]*)\)\s*return\s+false;/g))
  guardConds.push(norm(m[1]));

/* drawMasterFx dispatches BEFORE it lays out the row; BOX_Y is the first line
   of the chain-diagram path, so everything above it is the dispatch chain. */
const draw = decomment(slice(mfx, "export function drawMasterFx()", "drawMasterFx"));
const prelude = draw.slice(0, draw.indexOf("const BOX_Y"));
if (prelude.length === 0 || draw.indexOf("const BOX_Y") < 0)
  fail("could not find the chain-diagram path in drawMasterFx");
const drawConds = [];
for (const m of prelude.matchAll(/if\s*\(([^)]*(?:\([^)]*\))?[^)]*)\)\s*\{\s*[a-zA-Z]/g))
  drawConds.push(norm(m[1]));

/* Non-vacuity FIRST: a regex that stopped matching would otherwise agree with
   another regex that stopped matching, and the whole file would pass empty. */
const KNOWN = 9;
if (guardConds.length < KNOWN)
  fail("masterFxChainDiagramVisible names only " + guardConds.length +
       " sub-screens, expected at least " + KNOWN + " -- the derivation is blind");
if (drawConds.length < KNOWN)
  fail("drawMasterFx dispatches on only " + drawConds.length +
       " conditions, expected at least " + KNOWN + " -- the derivation is blind");

const missing = drawConds.filter((c) => guardConds.indexOf(c) < 0);
const extra = guardConds.filter((c) => drawConds.indexOf(c) < 0);
if (missing.length)
  fail("drawMasterFx returns early on [" + missing.join(", ") + "] but " +
       "masterFxChainDiagramVisible does not -- a knob touched while one of " +
       "those is up raises a card that nothing draws, and no overlay either");
if (extra.length)
  fail("masterFxChainDiagramVisible hides the card for [" + extra.join(", ") +
       "] but drawMasterFx draws the diagram anyway -- the knob is left with " +
       "no feedback on a screen that could have shown the card");

/* ---- 2. drawMasterFx actually draws the card, on the diagram path ------ */

const after = draw.slice(draw.indexOf("const BOX_Y"));
if (after.indexOf("drawKnobCard") < 0)
  fail("drawMasterFx never calls drawKnobCard -- the Master FX knob is back on " +
       "the centred Value box the card replaced");
if (after.indexOf("knobCardDrawState") < 0)
  fail("drawMasterFx does not ask for the cards draw state");
if (/typeof\s+(drawKnobCard|knobCardDrawState)/.test(draw))
  fail("the card block is behind a typeof guard. Under the lift in " +
       "test_chain_editor_snapshot.sh that makes it silently unreachable, and " +
       "the baseline then blesses a screen with the feature switched off -- " +
       "which is the bug 5c9fcd51 already shipped once");

/* ---- 3. no view test left in the card block ---------------------------- */

const cardAt = ui.indexOf("const KNOB_CARD_DECAY_MS");
const cardEnd = ui.indexOf("\n}\n", ui.indexOf("function showKnobFeedback(", cardAt));
const card = decomment(ui.slice(cardAt, cardEnd));
if (/view\s*[!=]==?\s*VIEWS\./.test(card))
  fail("the knob card block tests `view` again. That gate is what made the " +
       "card a slot-chain-only feature the day after it shipped; " +
       "chainEditorFocus() answers for both chains and is the only thing that " +
       "should decide whether a card is raised");
if (card.indexOf("chainEditorFocus") < 0)
  fail("the knob card block no longer goes through chainEditorFocus");

/* ---- 4. one knob-context builder, and no kind test inside it ----------- */

const builder = decomment(slice(ui, "function buildChainKnobContext(",
                                "buildChainKnobContext"));
if (!builder) fail("buildChainKnobContext is gone -- the two editors are " +
                   "building knob contexts separately again");
if (/target\.kind\s*[!=]==?/.test(builder) || /isMasterFx/.test(builder))
  fail("buildChainKnobContext branches on WHICH CHAIN it is looking at. A " +
       "shared function with a kind test in it drifts exactly as well as two " +
       "functions did, and states no reason for the difference");

/* Both editors must reach it, or half the convergence is decorative. */
const dispatch = decomment(slice(ui, "function buildKnobContextForKnob(",
                                 "buildKnobContextForKnob"));
const calls = (dispatch.match(/buildChainKnobContext\(/g) || []).length;
if (calls !== 2)
  fail("buildKnobContextForKnob calls buildChainKnobContext " + calls +
       " times, expected 2 -- one chain editor is not using it");
if (dispatch.indexOf("MASTER_CHAIN_TARGET") < 0 || dispatch.indexOf("slotChainTarget") < 0)
  fail("buildKnobContextForKnob does not pass both chain targets");

/* ---- 5. ONE fallback rule, and it is the declared-row one -------------- */
/*
 * The two builders this replaced disagreed here, silently: the slot editor
 * fell back to chain_params only when there was no hierarchy AT ALL, while
 * Master FX fell back whenever the hierarchy had no knob at that index. Six
 * audio-FX modules in tests/fixtures/module-contracts.json declare a hierarchy
 * with fewer than eight knobs and carry extra chain_params, so those six
 * behaved differently depending on which chain they were loaded into -- and on
 * psxverb the master rule duplicated knob 4 onto knob 5, while on smack it put
 * three trigger params ("Capture Now", "Arm Record", "Re-Roll") under knobs
 * 5-8 that the author had deliberately left off the row.
 *
 * The rule chosen is the declared-row one, and what pins it is that the
 * fallback is in the ELSE of "does this module declare a hierarchy at all".
 * Asserted structurally rather than by matching the sentence, because the way
 * this regresses is somebody moving the fallback out of the else.
 */
{
  const hierAt = builder.indexOf("if (hierarchy && hierarchy.levels)");
  const elseAt = builder.indexOf("} else {", hierAt);
  const ret = builder.indexOf("return generic(", elseAt);
  if (hierAt < 0 || elseAt < 0 || ret < 0) {
    fail("could not find the hierarchy / no-hierarchy split in buildChainKnobContext");
  } else {
    const withHier = builder.slice(hierAt, elseAt);
    const without = builder.slice(elseAt, ret);
    if (withHier.indexOf("chainTargetChainParams") < 0)
      fail("the mapped branch never fetches chain_params for the knobs meta");
    if (without.indexOf("chainParams[knobIndex]") < 0)
      fail("the no-hierarchy branch does not index chain_params by knob -- " +
           "there is nothing else to go on there, so that IS the fallback");
    if (withHier.indexOf("chainParams[knobIndex]") >= 0)
      fail("a module that DECLARED its knob row is being topped up from " +
           "chain_params by index. That was the Master FX rule and it is the " +
           "one that was dropped: the index into chain_params has no relation " +
           "to the knobs already mapped, so it duplicates one param and hides " +
           "another (psxverb), and it fires trigger params the author left off " +
           "the row (smack). See the comment above buildChainKnobContext");
  }
}

if (failures) process.exit(1);
console.log("PASS: Master FX knob card — " + guardConds.length + " sub-screens " +
            "mirrored between the guard and the dispatch, one builder for both " +
            "chains, one fallback rule");
'
