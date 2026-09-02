#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Parameter-lock DECORATIONS, in every layout the device can be set to.
#
# The renderer half of parameter locks is the `decorations` contract the shared
# library already exposed: a per-slot value override plus a `locked` flag. The
# risk it carries is not that it draws the wrong pixels — it is that a layout
# IGNORES it and the lock becomes invisible while remaining perfectly
# placeable. That is what LAYOUT_MOVY did, and it is the layout the native grid
# picks whenever the param view is set to Knobs.
#
# So each layout is asserted to actually CHANGE when a decoration arrives.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the lock decoration tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./tools/param-pages/fake_device.mjs"),
  import("./tools/param-pages/harness.mjs"),
]).then(([C, D, H]) => {
  const fail = (msg) => { console.log("FAIL: " + msg); process.exit(1); };

  const build = (layout) => {
    const dev = D.createFakeDevice({ id: "obxd" });
    const ctl = C.createController(dev);
    ctl.load({ slot: 0, component: "synth" });
    ctl.setLayout(layout);
    /* Let the staggered value cursor fill in — the grid reads one key per
     * tick by design, so a single tick would draw mostly "--". */
    for (let i = 0; i < 64; i++) ctl.tick();
    return { dev, ctl };
  };

  const frame = (ctl) => {
    const fb = H.createFramebuffer();
    ctl.render(H.drawContext(fb), { title: "S1 > OBXD" });
    return fb.toAscii();
  };

  /* A decoration on the first cell, with a value deliberately unlike whatever
   * the module declares, so "locked" cannot coincide with the base reading. */
  const decorate = (ctl) => {
    const page = ctl.page;
    if (!page || !page.keys || !page.keys[0]) fail("fixture has no knob page");
    const decorations = [];
    decorations[0] = { locked: true, value: 0.125 };
    ctl.setDecorations(decorations);
  };

  for (const [name, layout] of [["MOVY", C.LAYOUT_MOVY], ["LIST", C.LAYOUT_LIST]]) {
    if (layout === undefined) fail(name + " layout is not exported");

    const { ctl } = build(layout);
    const before = frame(ctl);

    decorate(ctl);
    const after = frame(ctl);

    if (before === after) {
      fail(name + " layout ignores decorations — a parameter lock would be "
           + "placeable but invisible in this layout");
    }

    /* And it must go back: releasing the step returns the cells to the values
     * the patch actually saved, or the editor would keep showing a step it is
     * no longer holding. */
    ctl.setDecorations(null);
    if (frame(ctl) !== before) {
      fail(name + " layout does not return to base values when decorations clear");
    }
  }

  /* setLockedValues maps FULL param keys onto knob slots.
   *
   * This is the join that has to be made inside the controller, and the bug it
   * guards is silent: locks arrive keyed "synth:cutoff" while a page holds bare
   * keys, so comparing the two directly matches nothing and every lock is
   * placeable, saved, audible — and invisible. Nothing throws. */
  {
    const { ctl } = build(C.LAYOUT_MOVY);
    const page = ctl.page;
    const bare = page && page.keys && page.keys[0];
    if (!bare) fail("fixture has no knob page");

    /* The bare key alone must NOT match — that is the mistake being guarded. */
    ctl.setLockedValues({ [bare]: 0.125 });
    if (ctl.state.decorations) {
      fail("a bare page key must not be treated as a full param key");
    }

    /* Prefixed, it must. */
    ctl.setLockedValues({ ["synth:" + bare]: 0.125 });
    const dec = ctl.state.decorations;
    if (!dec || !dec[0] || !dec[0].locked) {
      fail("setLockedValues did not map a full param key onto its knob slot");
    }
    if (dec[0].value !== 0.125) fail("setLockedValues lost the locked value");
    if (dec[1]) fail("an unlocked slot must carry no decoration");

    /* And it must render — the whole chain from full key to pixels. */
    const plain = frame(ctl);
    ctl.setLockedValues(null);
    if (frame(ctl) === plain) fail("setLockedValues decorations did not reach the renderer");
  }

  /* Graphics stand down while decorations are live. A viz group replaces
   * several slots with one picture, which would hide WHICH of them is locked —
   * the rule render_page.mjs states, asserted for the controller that has to
   * apply it to both layouts. */
  {
    const { ctl } = build(C.LAYOUT_MOVY);
    decorate(ctl);
    const st = ctl.state;
    if (st.decorations === null || st.decorations === undefined) {
      fail("setDecorations did not take");
    }
  }

  /* ---- the gesture itself, end to end through the controller ----
   *
   * This is the half that shipped broken: the editor lived in the native
   * binding, 9W9 draws its own grid, and nothing ever reached the DSP. Now the
   * controller owns it, so the SAME fake device a module-owned grid would hand
   * it is enough to prove the whole path: step down reads the locks, a detent
   * writes lock:set and never the base, release restores the cells, and a
   * detent after release is a base write again. */
  {
    const dev = D.createFakeDevice({ id: "obxd" });
    const lockAt = {};          /* step -> {"synth:key": value} the DSP would answer */
    const writes = [];          /* every setParam, in order */
    const io = Object.assign({}, dev, {
      getParam: (k) => {
        const m = /^lock:at:(\d+)$/.exec(k);
        if (m) return JSON.stringify(lockAt[m[1]] || {});
        return dev.getParam(k);
      },
      setParam: (k, v) => { writes.push([k, String(v)]); dev.setParam(k, v); },
    });
    const ctl = C.createController(io);
    ctl.load({ slot: 0, component: "synth", prefix: "synth" });
    ctl.setLayout(C.LAYOUT_MOVY);
    for (let i = 0; i < 64; i++) ctl.tick();

    const key = ctl.page && ctl.page.keys && ctl.page.keys[0];
    if (!key) fail("fixture has no knob page");
    const fk = "synth:" + key;
    const baseBefore = dev.getParam(fk);

    /* Step 9 already holds a lock on this parameter — what lock:at answers. */
    lockAt[9] = { [fk]: "0.125" };

    /* --- down: decorations come from the DSP answer --- */
    ctl.onStepButton(9, true);
    if (ctl.heldStep !== 9) fail("controller did not take the held step");
    const dec = ctl.state.decorations;
    if (!dec || !dec[0] || !dec[0].locked || String(dec[0].value) !== "0.125") {
      fail("step-down did not decorate the locked cell from lock:at");
    }

    /* --- a detent while held: lock:set, not the base --- */
    const nWrites = writes.length;
    ctl.onKnobTurn(0, 1, 100000);
    const lockWrites = writes.slice(nWrites).filter(([k]) => k === "lock:set");
    const baseWrites = writes.slice(nWrites).filter(([k]) => k === fk);
    if (lockWrites.length !== 1) fail("a detent while a step is held must write exactly one lock:set (got " + lockWrites.length + ")");
    if (baseWrites.length !== 0) fail("a detent while a step is held must not write the base value");
    const spec = lockWrites[0][1];
    if (!spec.startsWith(fk + ":9:")) fail("lock:set is not addressed to the held step: " + spec);
    if (dev.getParam(fk) !== baseBefore) fail("the base value moved under a lock");
    if (ctl.state.values[key] !== baseBefore && ctl.state.values[key] !== undefined) {
      fail("the controller moved its own copy of the base while locking");
    }
    /* The cell shows the NEW lock value immediately, on the detent. */
    const dec2 = ctl.state.decorations;
    if (!dec2 || !dec2[0] || String(dec2[0].value) === "0.125") {
      fail("the decoration did not follow the detent");
    }

    /* --- a throttled detent, then release: the flush is still a lock --- */
    ctl.onKnobTurn(0, 1, 100001);                 /* inside SETPARAM_THROTTLE_MS: queued */
    const nBefore = writes.length;
    ctl.onStepButton(9, false);
    const flushed = writes.slice(nBefore);
    if (!flushed.some(([k]) => k === "lock:set")) fail("the throttled detent was not flushed as a lock on release");
    if (flushed.some(([k]) => k === fk)) fail("a detent made while the step was held leaked into the base on release");
    if (ctl.heldStep !== -1) fail("release did not clear the held step");
    if (ctl.state.decorations) fail("release did not clear the decorations");

    /* --- after release: a detent is a base write again --- */
    const nAfter = writes.length;
    ctl.onKnobTurn(0, 1, 200000);
    const after = writes.slice(nAfter);
    if (!after.some(([k]) => k === fk)) fail("a detent after release did not write the base");
    if (after.some(([k]) => k === "lock:set")) fail("a detent after release still wrote a lock");

    /* --- a step held on a synthesised contract does nothing --- */
    const slotDev = D.createFakeDevice({ id: "obxd", prefix: "slot" });
    const slotCtl = C.createController(slotDev);
    slotCtl.load({ slot: 0, component: "slot", prefix: "slot" });
    if (slotCtl.onStepButton(3, true) !== false) fail("a synthesised contract must refuse the step");
    if (slotCtl.heldStep !== -1) fail("a synthesised contract took a held step");
  }

  console.log("PASS: parameter-lock decorations are honoured in every layout, and the gesture writes locks, never the base");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
