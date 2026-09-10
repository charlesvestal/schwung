/*
 * PAD BLOCK MUST NOT SURVIVE THE SCREEN THAT ASKED FOR IT.
 *
 * `host_pad_block(1)` stops pad notes reaching Move and routes them to whoever
 * is on screen -- for EVERY module, not just the one that asked. Its two
 * callers (a canvas overlay and the text-entry keyboard) release it in their
 * close hooks, which is correct and is not enough: leave by any path that does
 * not run that hook and the flag is stranded ON, and every module afterwards
 * silently has no pads until a reboot. Reported from the device as "if I leave
 * the midi module the pads are not released".
 *
 * The fix mirrors the CC claims: DERIVED from what is on screen, never
 * bookkept, and one-directional -- the host only ever CLEARS, because setting
 * belongs to whoever wants the pads and a component that has gone cannot clear
 * anything.
 *
 * This lifts the reconciler out of shadow_ui.js and drives it, so the rule is
 * tested rather than the comment describing it.
 */
import { readFileSync } from "node:fs";

const src = readFileSync(new URL("../../src/shadow/shadow_ui.js", import.meta.url), "utf8");
const m = src.match(/let padBlockWanted = false;\s*\nfunction reconcilePadBlock\(\) \{[\s\S]*?\n\}/);
if (!m) { console.log("FAIL: reconcilePadBlock() not found in shadow_ui.js"); process.exit(1); }

let VIEWS = { CANVAS: "canvas", PARAM_PAGES: "grid", HIERARCHY_EDITOR: "hier",
              COMPONENT_EDIT: "component" };
let view, coRunView = null, textActive = false, blocked = null, shown = 1;
/* A loaded module UI with a tick is the OTHER legitimate owner, and it is the
 * one this reconciler was originally written for. */
let loadedModuleUi = null;
const host_pad_block = (v) => { blocked = v; };
/* Move's own pad colours, as the shim mirrors them into overlay SHM. */
const MOVE_PADS = {};
for (let n = 68; n <= 99; n++) MOVE_PADS[String(n)] = (n % 7) + 1;
let restored = [];
const shadow_get_pad_led_snapshot = () => MOVE_PADS;
const move_midi_internal_send = (m) => restored.push(m);
const coRunUiActive = () => coRunView !== null;
const isTextEntryActive = () => textActive;
/* `view` and `coRunView` are module-level lets in shadow_ui.js; the lifted
 * function reads both, so both are threaded in as accessors. */
const shadow_get_display_mode = () => shown;
const fn = new Function("VIEWS", "host_pad_block", "coRunUiActive",
                        "isTextEntryActive", "getView", "getCoRunView",
                        "shadow_get_display_mode", "getLoadedModuleUi",
                        "shadow_get_pad_led_snapshot", "move_midi_internal_send",
    m[0].replace(/\bcoRunView\b/g, "getCoRunView()")
        .replace(/(^|[^\w.])view\b/g, "$1getView()")
        .replace(/\bloadedModuleUi\b/g, "getLoadedModuleUi()") +
    "\nreturn reconcilePadBlock;")
    (VIEWS, host_pad_block, coRunUiActive, isTextEntryActive,
     () => view, () => coRunView, shadow_get_display_mode, () => loadedModuleUi,
     shadow_get_pad_led_snapshot, move_midi_internal_send);

let bad = 0;
const step = (what, setup, wantBlocked) => {
    setup();
    fn();
    const ok = blocked === wantBlocked;
    console.log(`  ${what.padEnd(46)} pad_block=${String(blocked).padEnd(5)} ${ok ? "ok" : "FAIL want " + wantBlocked}`);
    if (!ok) bad = 1;
};

/* The canvas takes the pads and the host leaves it alone while it is up. */
step("canvas open, module set the flag",
     () => { view = VIEWS.CANVAS; blocked = 1; }, 1);

/* Leaving to the hierarchy editor WITHOUT the close hook running. */
step("left the module (no close hook ran)",
     () => { view = VIEWS.HIERARCHY_EDITOR; }, 0);

/*
 * The CLEAR stays unconditional on every tick -- that is the existing
 * behaviour and it is idempotent, and it is also the only thing that can
 * recover a block stranded by something this reconciler never saw take it.
 * What must happen exactly ONCE is the LED restore, which the latch governs
 * and which the last section of this file checks.
 */
step("still released on later ticks (idempotent)",
     () => { blocked = "untouched"; }, 0);

/*
 * DISMISSING THE SHADOW UI WITH THE CANVAS OPEN. `view` still says CANVAS --
 * it is where the UI would RESUME, not what is on screen -- so testing the
 * view alone kept the pads blocked for exactly the case this net exists for:
 * leaving to use another module.
 */
step("canvas open again",
     () => { view = VIEWS.CANVAS; shown = 1; blocked = 1; }, 1);
step("shadow UI dismissed -> released",
     () => { shown = 0; }, 0);
step("...and stays released while away",
     () => { blocked = "untouched"; }, 0);
step("come back to it",
     () => { shown = 1; blocked = 1; }, 1);

/*
 * A LOADED MODULE UI owns the pads too -- the case this reconciler was written
 * for. Pinned here because the canvas support added beside it must not cost
 * that one.
 */
step("module UI on screen with a tick",
     () => { view = VIEWS.COMPONENT_EDIT; loadedModuleUi = { tick(){} }; blocked = 1; }, 1);
step("module UI gone -> released",
     () => { view = VIEWS.HIERARCHY_EDITOR; loadedModuleUi = null; }, 0);

/* Text entry is the other legitimate owner and must not be cut off. */
step("text entry open",
     () => { view = VIEWS.PARAM_PAGES; shown = 1; textActive = true; blocked = 1; }, 1);
step("text entry closed -> released",
     () => { textActive = false; }, 0);

/* Co-run canvas counts as on screen. */
step("co-run canvas open",
     () => { view = VIEWS.PARAM_PAGES; coRunView = VIEWS.CANVAS; blocked = 1; }, 1);
step("co-run ended -> released",
     () => { coRunView = null; }, 0);

/*
 * AND THE PADS COME BACK LIT.
 *
 * Move writes a pad LED only when its OWN value changes, so releasing the
 * block while the grid still holds our colours -- or is blank -- leaves pads
 * that respond and are invisible. Reported from the device as "the pads are
 * dead". The shim mirrors Move's state into SHM continuously; the release puts
 * it back.
 */
restored = [];
view = VIEWS.CANVAS; shown = 1; blocked = 1; fn();     /* on screen */
view = VIEWS.HIERARCHY_EDITOR; fn();                   /* left */
const all32 = restored.length === 32;
const matches = restored.every((m) => m[3] === MOVE_PADS[String(m[2])]);
const anyLit = restored.some((m) => m[3] !== 0);
console.log(`  ${"all 32 pads restored".padEnd(46)} ${restored.length} ${all32 ? "ok" : "FAIL"}`);
console.log(`  ${"to MOVE's colours, not blanked".padEnd(46)} ${matches && anyLit ? "yes  ok" : "no   FAIL"}`);
if (!all32 || !matches || !anyLit) bad = 1;

console.log("\n" + (bad
  ? "FAIL: pad_block can outlive the screen that asked for it"
  : "pad_block is released whenever nothing on screen wants the pads"));
process.exit(bad);
