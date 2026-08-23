#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Every slot-settings ACTION must actually run.
#
# runChainSettingAction is reachable in the UI only by pressing one specific row
# on one specific screen, and a ReferenceError inside a branch is swallowed by
# the tick try/catch into "UI error, recovering" — so a broken action looks like
# a mysterious view change rather than a crash. That is exactly how a stale
# `setting.key` reference in the LFO branch reached hardware: the file parsed,
# and no test ever executed the branch.
#
# This runs all five, on both an empty and a populated slot, and fails on any
# throw. It is deliberately blunt: it does not assert what each action DOES,
# only that it does it without exploding. Behaviour is the list view's job.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi
if ! command -v python3 >/dev/null 2>&1; then echo "FAIL: python3 required" >&2; exit 1; fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cp -R src "$TMP/src"
TMP="$TMP" python3 - <<'PY'
import os
root = os.environ["TMP"] + "/src"
open(root + "/os.mjs", "w").write("export function open(){return null}\nexport const O_RDONLY=0;\nexport default {};\n")
open(root + "/std.mjs", "w").write("export function open(){return null}\nexport function loadFile(){return null}\nexport default {};\n")
for base, _, files in os.walk(root):
    for f in files:
        if not f.endswith((".mjs", ".js")): continue
        p = os.path.join(base, f)
        try: s = open(p, encoding="utf8").read()
        except Exception: continue
        o = s
        s = s.replace("/data/UserData/schwung/", root + "/")
        for m in ("os", "std"):
            s = s.replace("from '%s'" % m, "from '%s/%s.mjs'" % (root, m))
            s = s.replace('from "%s"' % m, 'from "%s/%s.mjs"' % (root, m))
        if s != o: open(p, "w", encoding="utf8").write(s)
open(root + "/shadow/ui.mjs", "w", encoding="utf8").write(
    open(root + "/shadow/shadow_ui.js", encoding="utf8").read())
PY

TREE="$TMP/src" node --input-type=module -e '
const TREE = process.env.TREE;
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

for (const n of ["print","fill_rect","clear_screen","text_width","draw_line",
                 "draw_circle","fill_circle","draw_arc","flush_display",
                 /* the knob indicator ring LEDs go out through this one */
                 "move_midi_internal_send"]) globalThis[n] = () => 0;

const store = {};
/*
 * Record which SLOT each param access names. enterKnobEditor and makeSlotLfoCtx
 * both read through this, so it is how a wrong slot is caught without exposing
 * new state: asserting only that a view opened cannot tell slot 0 from slot 3,
 * and a mutant passing the wrong one survived until this was added.
 */
let slotsTouched = new Set();
globalThis.shadow_get_param = (slot, key) => { slotsTouched.add(slot); return (key in store ? store[key] : ""); };
globalThis.shadow_set_param = (slot, key, v) => { slotsTouched.add(slot); store[key] = String(v); return true; };
globalThis.shadow_get_shift_held = () => 0;
globalThis.host_file_exists = () => true;
globalThis.host_read_file = () => "";
globalThis.host_write_file = () => true;

await import(TREE + "/shadow/ui.mjs");
const { ctx } = await import(TREE + "/shadow/shadow_ui_ctx.mjs");

if (typeof ctx.runSlotAction !== "function") {
  fail("ctx.runSlotAction is missing — the actions are unexecutable from a test again");
} else {
  const V = ctx.VIEWS;
  /*
   * ctx.slots is EMPTY here — the slot config is loaded in init(), which the
   * harness does not call — so slots[0] must be created, not mutated. Writing
   * `ctx.slots[0].name` silently did nothing, and every "named slot" case was
   * really testing the unnamed path. isExistingPreset() is just this name, so
   * it is the only thing the Save/Delete branches actually depend on.
   */
  const setName = (n) => {
    if (!ctx.slots) return;
    ctx.slots[0] = Object.assign({}, ctx.slots[0] || {}, { name: n });
  };
  const reset = () => {
    ctx.setView(V.CHAIN_SETTINGS);
  };

  /*
   * Each action must have an OBSERVABLE effect.
   *
   * "It did not throw" is too weak: deleting a whole branch turns the action
   * into a silent no-op and a throw-only test still passes. Both of those
   * mutants slipped through before this was tightened.
   *
   * `expect` runs after the action and says what should have changed.
   */
  const CASES = [
    { key: "knobs",  name: "My Patch",
      expect: () => ctx.view === V.KNOB_EDITOR,
      why: "Knob Mapping must open the knob editor" },
    { key: "lfo1",   name: "My Patch",
      expect: () => ctx.view === V.LFO_EDIT,
      why: "LFO 1 must open the LFO editor" },
    { key: "lfo2",   name: "My Patch",
      expect: () => ctx.view === V.LFO_EDIT,
      why: "LFO 2 must open the LFO editor" },
    /* An unnamed slot has nothing to overwrite, so Save asks for a name. */
    { key: "save",   name: "",
      expect: () => ctx.showingNamePreview === true,
      why: "Save on an unnamed slot must offer the name preview" },
    /* A named one already has a target, so Save confirms the overwrite. */
    { key: "save",   name: "My Patch",
      expect: () => ctx.confirmingOverwrite === true,
      why: "Save on a named slot must confirm the overwrite" },
    { key: "save_as", name: "My Patch",
      expect: () => ctx.showingNamePreview === true,
      why: "Save As must offer the name preview" },
    /* Delete does nothing at all unless the preset exists — running it only on
     * an empty slot leaves its body untested, which is how a bad identifier in
     * there survived a mutation check. */
    { key: "delete", name: "My Patch",
      expect: () => ctx.confirmingDelete === true,
      why: "Delete on an existing preset must raise the confirm" },
  ];

  /*
   * Delete on an EMPTY slot must do nothing. Checked BEFORE the cases below,
   * and against its prior value: confirmingDelete has only a getter, so
   * once a later case raises it nothing here can lower it again, and running
   * this afterwards would assert on a flag the test itself had set.
   */
  reset();
  setName("");
  const deleteFlagBefore = ctx.confirmingDelete;
  try {
    ctx.runSlotAction(0, "delete");
    if (ctx.confirmingDelete !== deleteFlagBefore) {
      fail("Delete raised a confirm on a slot with no preset");
    }
  } catch (e) { fail("Delete on an empty slot threw: " + e); }

  for (const c of CASES) {
    reset();
    setName(c.name);
    slotsTouched = new Set();
    let threw = null;
    try { ctx.runSlotAction(0, c.key); } catch (e) { threw = e; }
    if (threw) { fail("action " + JSON.stringify(c.key) + " threw: " + threw); continue; }
    let ok = false;
    try { ok = !!c.expect(); } catch (e) { ok = false; }
    if (!ok) fail(c.why + " (key=" + c.key + ", name=" + JSON.stringify(c.name) +
                  ", view=" + ctx.view + ")");
    /* Whatever it touched, it must have touched THIS slot and no other. */
    const others = [...slotsTouched].filter((n) => n !== 0);
    if (others.length) {
      fail("action " + JSON.stringify(c.key) + " read or wrote slot(s) " +
           others.join(",") + " when asked for slot 0 — the slot argument is " +
           "not reaching the code that uses it");
    }
  }


  /* An unknown key must be a no-op, not a throw — the grid can only send what
   * the contract declares, but the contract is data and data drifts. */
  reset();
  try { ctx.runSlotAction(0, "no_such_action"); }
  catch (e) { fail("an unknown action key threw instead of doing nothing: " + e); }
}

if (failures) process.exit(1);
console.log("PASS: slot actions — every branch runs AND has its expected effect, " +
            "Delete is inert without a preset, unknown keys are no-ops");
'
