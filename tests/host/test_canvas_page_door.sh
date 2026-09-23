#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# AN ENTERABLE CANVAS PAGE IS A DOOR.
#
# A canvas page is a PAGE_KNOBS page whose body the module paints, and it was
# draw-only: the jog paged past it, a click dived the cell under your hand, and
# the module received nothing. So a module could draw a file browser or a
# settings menu and had no way to let anyone walk it.
#
# `enterable: true` makes that page a door -- the host concept that already
# exists for menus, preset browsers and items lists. Click enters, the bracket
# frame says so, Shift+click still reaches the section picker, and while entered
# the jog and the click are the module's, handed over as the CCs the hardware
# actually sends so ONE script serves a page and a fullscreen dive without
# knowing which it is on.
#
# Back is the half that has to be right. A door exits on Back, which for a
# browser would mean leaving the browser instead of going up a folder -- the
# exact bug the feature exists to fix. So the module answers first:
#
#     handleBack() === true   "I went up a level"     -> stay in the door
#     anything else           "I am at my top level"  -> leave the door
#
# Driven through the real controller, not pinned: every claim below is a
# gesture in and an observable out.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the canvas door tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./tools/param-pages/fake_device.mjs"),
]).then(([C, D]) => {
  let fail = 0;
  const ok = (c, m) => { console.log((c ? "  ok   " : "  FAIL ") + m); if (!c) fail++; };

  /* A module with one knob and one enterable canvas page. */
  const contract = (canvasExtra) => ({
    "synth:chain_params": JSON.stringify([
      { key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1, step: 0.01 },
      Object.assign({ key: "browse", name: "Browse", type: "canvas",
                      canvas_script: "b.js", as_page: true }, canvasExtra),
    ]),
    "synth:ui_hierarchy": JSON.stringify({
      modes: null,
      levels: { root: { label: "S", knobs: ["cutoff"],
                        params: [{ key: "cutoff", label: "Cutoff" },
                                 { key: "browse", label: "Browse" }] } },
    }),
  });

  /* The module behind the page: records what it was handed, and climbs two
     levels before declaring itself at the top. */
  const makeModule = () => {
    const seen = [];
    let depth = 2;
    return {
      seen,
      hook: (canvas, hook, payload) => {
        seen.push({ hook, data: payload && payload.data });
        if (hook === "handleBack") { if (depth <= 0) return false; depth--; return true; }
        return undefined;
      },
    };
  };

  /* A controller over a hand-written contract. The fake device serves whole
     fixture modules and this needs one exact shape, so the two contract reads
     are answered directly -- the same interception test_param_pages_controller
     uses for its own one-param fixture. */
  const build = (canvasExtra, hookFn) => {
    const c = contract(canvasExtra);
    return C.createController({
      getParam: (k) => (k in c ? c[k] : ""),
      setParam: () => {},
      canvasPageHook: hookFn,
    });
  };

  const setup = (canvasExtra) => {
    const mod = makeModule();
    const ctl = build(canvasExtra, (canvas, hook, payload) => mod.hook(canvas, hook, payload));
    ctl.load({ slot: 0, component: "synth" });
    return { ctl, mod };
  };

  const goToCanvas = (ctl) => {
    for (let i = 0; i < ctl.pages.length; i++) {
      if (ctl.page && ctl.page.canvas) return true;
      ctl.onJog(1, Date.now());
    }
    return !!(ctl.page && ctl.page.canvas);
  };

  /* ---- 1. the page exists and is a door -------------------------------- */
  {
    const { ctl } = setup({ enterable: true });
    ok(goToCanvas(ctl), "the module gets a canvas page");
    ok(ctl.isDoor() === true, "an enterable canvas page is a door");
  }

  /* ---- 2. NOT a door without the flag. The default must not move. ------ */
  {
    const { ctl } = setup({});
    ok(goToCanvas(ctl), "a plain canvas page still exists");
    ok(ctl.isDoor() === false, "a canvas page without the flag is NOT a door");
  }

  /* ---- 3. click enters, then the gestures belong to the module --------- */
  {
    const { ctl, mod } = setup({ enterable: true });
    goToCanvas(ctl);
    ctl.onClick(-1);
    ok(mod.seen.length === 0, "the click that ENTERS is not also sent to the module");
    ctl.onJog(1, Date.now());
    const jog = mod.seen.find((e) => e.hook === "onMidi" && e.data && e.data[1] === 14);
    ok(!!jog, "the jog reaches the module as CC 14");
    ok(jog && jog.data[2] === 1, "...carrying the detent, +1 -> 1");
    ctl.onJog(-1, Date.now());
    const back = mod.seen.filter((e) => e.data && e.data[1] === 14).pop();
    ok(back && back.data[2] === 127, "...and -1 as 127, the relative encoding the hardware sends");
    ctl.onClick(-1);
    ok(mod.seen.some((e) => e.data && e.data[1] === 3 && e.data[2] === 127),
       "a click INSIDE the door reaches the module as CC 3");
  }

  /* ---- 4. the exit contract -------------------------------------------- */
  {
    const { ctl, mod } = setup({ enterable: true });
    goToCanvas(ctl);
    ctl.onClick(-1);
    ok(ctl.menuEntered() === true, "click entered the door");
    ctl.exitMenu();
    ok(ctl.menuEntered() === true, "Back with handleBack true STAYS in the door");
    ctl.exitMenu();
    ok(ctl.menuEntered() === true, "...and again, for as long as the module is climbing");
    ctl.exitMenu();
    ok(ctl.menuEntered() === false, "Back leaves once the module says it is at its top");
    ok(mod.seen.filter((e) => e.hook === "handleBack").length === 3,
       "the module was asked every time, and only until it declined");
  }

  /* ---- 5. a module that answers nothing must not trap anyone ----------- */
  {
    const ctl = build({ enterable: true }, () => undefined);  /* no hook, or one that threw */
    ctl.load({ slot: 0, component: "synth" });
    goToCanvas(ctl);
    ctl.onClick(-1);
    ok(ctl.menuEntered() === true, "entered a module with no handleBack");
    ctl.exitMenu();
    ok(ctl.menuEntered() === false, "one Back leaves it -- silence is never a claim");
  }

  /* ---- 6. a consumer with no canvasPageHook at all --------------------- */
  {
    const ctl = build({ enterable: true }, undefined);   /* io member simply absent */
    ctl.load({ slot: 0, component: "synth" });
    goToCanvas(ctl);
    ctl.onClick(-1);
    ctl.exitMenu();
    ok(ctl.menuEntered() === false, "a consumer that cannot deliver hooks still lets you out");
  }

  /* ---- 6b. the module can say IT is done ------------------------------- */
  {
    /* Picking a sample IS leaving the browser. A module answers a gesture with
       the close sentinel and the door lets it go, without a second press. */
    const ctl = build({ enterable: true },
                      (canvas, hook) => (hook === "onMidi" ? { close: true } : undefined));
    ctl.load({ slot: 0, component: "synth" });
    goToCanvas(ctl);
    ctl.onClick(-1);
    ok(ctl.menuEntered() === true, "entered");
    ctl.onClick(-1);
    ok(ctl.menuEntered() === false, "a module that answers close leaves the door");
  }

  /* A PLAIN true must not close it -- that is an ordinary consumed gesture. */
  {
    const ctl = build({ enterable: true }, () => true);
    ctl.load({ slot: 0, component: "synth" });
    goToCanvas(ctl);
    ctl.onClick(-1);
    ctl.onClick(-1);
    ok(ctl.menuEntered() === true, "a hook returning plain true does NOT close the door");
  }

  /* ---- 7. preset_browser + enterable is refused, not resolved ---------- */
  {
    const { ctl } = setup({ enterable: true, preset_browser: true });
    const p = ctl.pages.find((x) => x.canvas);
    ok(!p || !p.canvas.enterable,
       "a canvas declaring BOTH is not enterable -- a preset page is already a door with every control spoken for");
  }

  if (fail) { console.log("test_canvas_page_door: " + fail + " FAILURE(S)"); process.exit(1); }
  console.log("PASS: an enterable canvas page is a door, and Back is the modules to climb");
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
'
