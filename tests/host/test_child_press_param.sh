#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A module may ask to be told about LIVE pad presses.
#
# child_index_param lets a module own its focus. A drum module wants that focus
# on the pad it just HIT, and that is the one fact it cannot see: Move turns a
# press into an ordinary note before playing it, so a hit and a sequenced note
# reach on_midi as the same bytes. The UI still sees the raw pad event, so a
# level may declare child_press_param (sibling shape: focus_press_param at the
# top) and the grid writes "1" to it on each physical press -- a VOUCH, never a
# pad id, because the pad-to-note map is Move's.
#
# Pinned:
#   1. the pure half: the declarations resolve, and only on a child level
#   2. the controller writes the vouch to <prefix>:<param>, once per note-on,
#      and NOTHING for a module that declared no such param
#   3. WHICH messages are a press: note-on only, velocity-0 is a release, and
#      the pad range -- run for real, not grepped
#   4. the shim's forward is PASSIVE -- no `continue`, the pad still plays --
#      and gated on pad_observe, which the shim drops when the display closes
#   5. the host reconciles pad_observe every tick and clears it on exit, and
#      RESTATES it rather than memoising: the shim drops it on its own

fail() { echo "FAIL: $*" >&2; exit 1; }

if ! command -v node >/dev/null 2>&1; then
  fail "node is required"
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/child_key.mjs"),
  import("./src/shared/param_pages/voices.mjs"),
  import("./src/shared/param_pages/page_input.mjs"),
]).then(([PC, CK, V, PI]) => {
  let bad = 0;
  const fail = (m) => { console.log("FAIL: " + m); bad++; };

  /* ---- 1. the pure half ----------------------------------------------- */
  const PADS = {
    label: "Pads", child_count: 4, child_label: "Pad",
    child_prefix: "pad", child_index_param: "ui_current_pad",
    child_press_param: "ui_live_press", child_note_base: 36,
    knobs: ["vol"], params: [{ key: "vol", name: "Vol", type: "float", min: 0, max: 1 }],
  };
  if (CK.childPressParam(PADS) !== "ui_live_press") fail("childPressParam does not report the declared key");
  if (CK.childPressParam({ child_press_param: "x" }) !== null) fail("a level with no children reported a press param");
  if (CK.childPressParam(Object.assign({}, PADS, { child_press_param: "" })) !== null) fail("an empty press param was reported");
  if (V.focusPressParamOf({ focus_press_param: "hit" }) !== "hit") fail("focusPressParamOf does not report the declared key");
  if (V.focusPressParamOf({}) !== null) fail("focusPressParamOf invented a key");

  /* ---- 2. WHICH message is a press -------------------------------------- */
  /* Not a grep. Each of these is a bug that ships silently: a release vouched
   * as a press moves the focus twice per hit, and a step button
   * (16-31) or a track button (40-43) is a note on the same cable that is not
   * a pad at all. */
  const press = PI.isHardwarePadPress;
  if (press([0x90, 68, 100]) !== true) fail("a pad note-on is not recognised as a press");
  if (press([0x99, 99, 1]) !== true) fail("the pad range is not inclusive of 99, or the channel nibble is being read as status");
  if (press([0x80, 68, 100]) !== false) fail("a NOTE-OFF vouched as a press -- the module would move focus twice per hit");
  if (press([0x90, 68, 0]) !== false) fail("a velocity-0 note-on vouched as a press -- Move sends releases in that form too");
  if (press([0x90, 67, 100]) !== false) fail("note 67 vouched as a press -- below the pad range");
  if (press([0x90, 100, 100]) !== false) fail("note 100 vouched as a press -- above the pad range");
  if (press([0x90, 28, 100]) !== false) fail("a STEP button (16-31) vouched as a press -- it is a note on the same cable, and it is not a pad");
  if (press([0x90, 42, 100]) !== false) fail("a TRACK button (40-43) vouched as a press");
  if (press([0x90, 3, 100]) !== false) fail("a knob-touch note vouched as a press");
  if (press([0xB0, 71, 1]) !== false) fail("a CC vouched as a press");
  if (press(null) !== false || press([0x90, 68]) !== false) fail("a short or absent message threw or vouched");

  /* ---- 3. driven through the real controller ---------------------------- */
  const mk = (hier) => {
    const writes = [];
    const CP = [];
    for (let i = 0; i < 4; i++) CP.push({ key: `pad${i}_vol`, name: "Vol", type: "float", min: 0, max: 1 });
    const io = {
      getParam: (k) => {
        if (k.endsWith(":ui_hierarchy")) return JSON.stringify(hier);
        if (k.endsWith(":chain_params")) return JSON.stringify(CP);
        if (k.endsWith(":ui_current_pad")) return "0";
        if (k.endsWith(":preset_name")) return "";
        if (k.endsWith(":is_loading")) return "0";
        if (k.endsWith(":module")) return "dr32";
        return "0.5";
      },
      setParam: (k, v) => { writes.push([k, String(v)]); },
      announce: () => {}, now: () => 0,
    };
    const c = PC.createController(io);
    c.load({ slot: 0, component: "synth", prefix: "synth" });
    c.setLayout("movy");
    for (let i = 0; i < 40; i++) c.tick();
    return { c, writes };
  };

  {
    const { c, writes } = mk({ pad_layout: "drums", levels: { root: { params: [{ level: "pads", label: "Pads" }] }, pads: PADS } });
    if (c.livePressParam() !== "ui_live_press") fail("livePressParam did not find the child level declaration (got " + c.livePressParam() + ")");
    const before = writes.length;
    if (c.vouchLivePress() !== true) fail("vouchLivePress reported nothing written");
    const mine = writes.slice(before).filter(([k]) => k === "synth:ui_live_press");
    if (mine.length !== 1 || mine[0][1] !== "1")
      fail("expected exactly one write of synth:ui_live_press = \"1\", got " + JSON.stringify(writes.slice(before)));
    /* The vouch is the whole message. A host that wrote the pad number would
     * be baking one controller’s geometry into every module. */
    if (writes.slice(before).some(([k, v]) => k.endsWith(":ui_current_pad")))
      fail("the vouch path wrote the INDEX -- the module owns child_index_param, the host only reports the gesture");
  }
  {
    const { c } = mk({ pad_layout: "drums", focus_press_param: "hit",
      levels: { root: { params: [{ level: "kick", label: "Kick" }] }, kick: { name: "Kick", note: 36, knobs: ["pad0_vol"], params: [{ key: "pad0_vol" }] } } });
    if (c.livePressParam() !== "hit") fail("the sibling-shape focus_press_param was not found");
  }
  {
    const quiet = Object.assign({}, PADS); delete quiet.child_press_param;
    const { c, writes } = mk({ pad_layout: "drums", levels: { root: { params: [{ level: "pads", label: "Pads" }] }, pads: quiet } });
    if (c.livePressParam() !== null) fail("a module that declared nothing has a live-press param: " + c.livePressParam());
    const before = writes.length;
    if (c.vouchLivePress() !== false) fail("vouchLivePress claimed to write for a module that asked for nothing");
    if (writes.length !== before) fail("a press wrote something for a module that declared no press param: " + JSON.stringify(writes.slice(before)));
  }

  if (bad) process.exit(1);
  console.log("  ok  the declaration resolves; the controller vouches once per press, to the declared key only");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
' || fail "controller half"

# ---- 4. the shim forward is passive and gated ---------------------------------
shim="src/schwung_shim.c"
hdr="src/host/shadow_constants.h"
command grep -q 'volatile uint8_t pad_observe;' "$hdr" || fail "shadow_control_t has no pad_observe"
fwd=$(command grep -n 'shadow_control->pad_observe &&' "$shim" | head -1 | cut -d: -f1)
[ -n "$fwd" ] || fail "the shim never forwards pads under pad_observe"
if sed -n "$fwd,$((fwd+4))p" "$shim" | command grep -q 'continue;'; then
  fail "the pad_observe forward has a \`continue\` -- that BLOCKS the pad from the DSP, which is pad_block, not observation"
fi
sed -n "$fwd,$((fwd+3))p" "$shim" | command grep -q 'shadow_ui_midi_publish' \
  || fail "the pad_observe branch does not publish to the shadow UI"
# Dropped on the shadow display's CLOSING edge -- the same one the edit-CC
# claim is dropped on (#435), and deliberately the same `static int
# prev_display_mode`: two statics tracking one transition is how they drift.
command grep -B8 'shadow_control->pad_observe = 0;' "$shim" | command grep -q 'prev_display_mode && !shadow_display_mode' \
  || fail "the shim does not drop pad_observe on the shadow display's closing edge"
[ "$(command grep -c 'static int prev_display_mode;\|static int prev_display_mode = 0;' "$shim")" = "1" ] \
  || fail "more than one static tracks the shadow display's closing edge -- they drift; pad_observe and the edit-CC claim share one"
command grep -q '"host_pad_observe"' src/shadow/shadow_ui.c || fail "host_pad_observe is not bound for the shadow UI"
echo "  ok  the shim forwards pads passively, only under pad_observe, and drops it on display close"

# ---- 5. reconciled every tick, cleared on exit, and RESTATED ------------------
pp="src/shadow/shadow_ui_param_pages.mjs"
command grep -q 'reconcilePadObserve(!!controller.livePressParam());' "$pp" \
  || fail "tickParamPages does not reconcile pad_observe from livePressParam"
command grep -A1 '^export function exitParamPages() {' "$pp" | command grep -q 'reconcilePadObserve(false);' \
  || fail "exitParamPages does not clear pad_observe -- leaving the grid would keep pads streaming into the UI ring"
command grep -q 'return controller.vouchLivePress();' "$pp" \
  || fail "handleParamPagesMidi does not vouch on a pad note-on"
command grep -q 'if (isHardwarePadPress(data))' "$pp" \
  || fail "handleParamPagesMidi hand-rolls the press predicate instead of using page_input.mjs -- the assertions above then test nothing the device runs"
# The reconcile must RESTATE pad_observe, never compare against a JS-side
# mirror of it. The shim clears pad_observe on its own authority (the shadow
# display closes from four sites in the SPI callback -- Menu tap, Track tap,
# Shift+Track, Shift+Step -- none of which tell JS), so a mirror is stale from
# the first Menu dismiss onward and a reconcile that trusts it skips the write
# that would turn the feature back on. js_host_pad_observe is idempotent and
# logs only on a transition for exactly this reason.
if sed -n "/^function reconcilePadObserve/,/^}/p" "$pp" | command grep -qE '===[[:space:]]*padObserveOn|padObserveOn[[:space:]]*==='; then
  fail "reconcilePadObserve compares against a JS mirror of pad_observe -- the shim drops it without telling JS, so the mirror latches and live presses die silently after the first Menu dismiss"
fi
sed -n "/^function reconcilePadObserve/,/^}/p" "$pp" | command grep -q 'host_pad_observe(' \
  || fail "reconcilePadObserve never calls host_pad_observe"
echo "  ok  pad_observe follows the grid: on while a declaring component is shown, off on exit"

echo "PASS: test_child_press_param"
