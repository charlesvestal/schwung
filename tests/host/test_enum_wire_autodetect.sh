#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The knob grid must LEARN which enum wire format a plugin speaks, the way the
# other two enum writers already do, instead of requiring a declaration.
#
# Background: `formatParamForSet` used to write `String(index)` for every enum
# unless the metadata declared `options_as_string: true`. A plugin whose
# set_param is a strcmp ladder over the option NAMES with no trailing else --
# `chord_set_param` is exactly that -- silently discards every one of those
# writes, so the value never moves while the grid renders the index it just
# invented. The user watches it change and snap back.
#
# Declaring the flag on the three built-in MIDI FX fixed those three modules.
# It does nothing for a third-party module, and nothing tells its author. The
# other two writers in shadow_ui.js never needed a declaration:
#
#     const pluginUsesIndex = (ctx.meta.options.indexOf(currentVal) < 0);
#
# -- they ask what the PLUGIN currently reports and answer in kind. This pins
# the same behaviour for the grid.
#
# THE TRAP, and the reason a naive version of this fix fails: the grid caches
# values in `s.values`, and its OWN WRITES populate that cache. Detect from
# there and the first index write teaches it "this plugin uses indices" for the
# rest of the session -- a self-fulfilling verdict that looks right in a
# one-detent test. Detection may only consider a value that came back from the
# DSP.
#
# So every case below runs MANY consecutive detents with the read cursor
# turning between them, and checks the DSP's own state, not the UI's.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the enum wire autodetect test" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("node:fs"),
]).then(([C, RM, fs]) => {
  let failures = 0;
  const fail = (m) => { console.log("FAIL: " + m); failures++; };

  const PREFIX = "midi_fx1";

  /* -------------------------------------------------------------- DSPs --
   * Two plugins, both real conventions found in the fleet.
   */

  /* chord.c: a strcmp ladder over the NAMES, no trailing else, and get_param
     answers with the name. An unrecognised write is silently dropped. */
  function nameOnlyDsp(options, initial) {
    const st = { value: options[initial], rejected: 0 };
    return {
      st,
      get: () => st.value,
      set: (val) => {
        if (options.indexOf(val) >= 0) st.value = val;
        else st.rejected++;
      },
      index: () => options.indexOf(st.value),
    };
  }

  /* The other half of the fleet: an atoi()-style set_param that only
     understands a numeric index, and reports one back. */
  function indexOnlyDsp(options, initial) {
    const st = { value: initial, rejected: 0 };
    return {
      st,
      get: () => String(st.value),
      set: (val) => {
        const n = Number(val);
        if (isFinite(n) && String(Math.round(n)) === String(val).trim() &&
            n >= 0 && n < options.length) st.value = Math.round(n);
        else st.rejected++;
      },
      index: () => st.value,
    };
  }

  /* ------------------------------------------------------------ harness --
   * The real controller, wired to a fake device that serves the contract and
   * routes one enum key to a simulated DSP. Reads and writes are counted so
   * this test also pins that autodetection costs no extra IPC.
   */
  function harness({ hierarchy, chainParams, key, dsp, others = {} }) {
    const stats = { reads: 0, writes: 0, readKeys: [] };
    const store = Object.assign({
      [PREFIX + ":ui_hierarchy"]: JSON.stringify(hierarchy),
      [PREFIX + ":chain_params"]: JSON.stringify(chainParams),
    }, others);
    const io = {
      getParam: (k) => {
        stats.reads++; stats.readKeys.push(k);
        if (k === PREFIX + ":" + key) return dsp.get();
        return store[k] !== undefined ? store[k] : null;
      },
      setParam: (k, v) => {
        stats.writes++;
        if (k === PREFIX + ":" + key) { dsp.set(v); return; }
        store[k] = v;
      },
      now: () => tms,
    };
    let tms = 1000;
    const ctl = C.createController(io);
    ctl.setLayout(RM.LAYOUT_MOVY);      /* what the device actually draws */
    ctl.load({ slot: 0, component: "midi_fx1", prefix: PREFIX });

    /* Find the grid page and knob slot carrying `key`. */
    let pageIdx = -1, slot = -1;
    ctl.pages.forEach((p, i) => {
      if (p.kind !== "knobs") return;
      const at = (p.keys || []).indexOf(key);
      if (at >= 0 && pageIdx < 0) { pageIdx = i; slot = at; }
    });
    if (pageIdx < 0) throw new Error("no grid page carries " + key);
    ctl.goToPage(pageIdx);

    return {
      ctl, stats, slot,
      advance: (ms) => { tms += ms; },
      /* Let the staggered read cursor sweep the whole page once. */
      settle: (n) => { for (let i = 0; i < n; i++) { tms += 25; ctl.tick(); } },
      /* One physical detent, then a tick -- exactly the interleaving on the
         device, and the interleaving that exposes the cache trap. */
      detent: (dir) => {
        tms += 25;
        ctl.onKnobTurn(slot, dir === undefined ? 1 : dir, tms);
        ctl.tick();
      },
    };
  }

  /* chord`s real declaration, with the override deliberately STRIPPED -- this
     is what a third-party module that never heard of the flag looks like. */
  const chordJson = JSON.parse(fs.readFileSync("src/modules/midi_fx/chord/module.json", "utf8"));
  function chordContract({ withOverride }) {
    const h = JSON.parse(JSON.stringify(chordJson.capabilities.ui_hierarchy));
    for (const lvl of Object.values(h.levels)) {
      for (const p of (lvl.params || [])) {
        if (p && typeof p === "object" && "options_as_string" in p) {
          if (withOverride) p.options_as_string = true;
          else delete p.options_as_string;
        }
      }
    }
    const declared = h.levels.root.params.find((p) => p.key === "type");
    /* chain_params exactly as chain_host.c renders it: it never emits the
       flag, so the hierarchy is the only carrier. */
    const chainParams = [{ key: "type", name: "Type", type: "enum", options: declared.options }];
    return { hierarchy: h, chainParams, options: declared.options };
  }

  /* ================================================== 1. name-only, no flag */
  {
    const { hierarchy, chainParams, options } = chordContract({ withOverride: false });
    const dsp = nameOnlyDsp(options, options.indexOf("major"));
    const h = harness({ hierarchy, chainParams, key: "type", dsp });
    h.settle(hierarchy.levels.root.knobs.length + 2);   /* cursor reads `type` */

    const before = dsp.index();
    for (let i = 0; i < 120; i++) h.detent(1);

    if (dsp.st.rejected)
      fail("a name-only plugin with NO options_as_string declaration rejected " +
           dsp.st.rejected + " of the grid`s writes -- the grid is still " +
           "writing String(index) into a strcmp ladder over the names, so the " +
           "value stays at " + JSON.stringify(dsp.st.value) + " while the grid " +
           "renders the index it invented");
    if (dsp.index() <= before)
      fail("after 120 detents the undeclared name-only plugin sits at " +
           JSON.stringify(dsp.st.value) + ", where it started");
  }

  /* ============================================ 2. index-only, no regression */
  {
    const { hierarchy, chainParams, options } = chordContract({ withOverride: false });
    const dsp = indexOnlyDsp(options, 1);
    const h = harness({ hierarchy, chainParams, key: "type", dsp });
    h.settle(hierarchy.levels.root.knobs.length + 2);

    const before = dsp.index();
    for (let i = 0; i < 120; i++) h.detent(1);

    if (dsp.st.rejected)
      fail("an index-taking plugin rejected " + dsp.st.rejected + " writes -- " +
           "autodetection has flipped the whole fleet onto names");
    if (dsp.index() <= before)
      fail("after 120 detents the index-taking plugin never moved off " + before);
  }

  /* ================================= 3. options_as_string is still an OVERRIDE
   * A plugin that reports an INDEX but declares the flag must still be written
   * to by NAME. The declaration outranks anything inferred; deleting it, or
   * letting detection beat it, breaks the eight shipped declarations.
   */
  {
    const { hierarchy, chainParams, options } = chordContract({ withOverride: true });
    const dsp = indexOnlyDsp(options, 1);
    const seen = [];
    const wrapped = { st: dsp.st, get: dsp.get, index: dsp.index,
                      set: (v) => { seen.push(v); dsp.set(v); } };
    const h = harness({ hierarchy, chainParams, key: "type", dsp: wrapped });
    h.settle(hierarchy.levels.root.knobs.length + 2);
    for (let i = 0; i < 40; i++) h.detent(1);

    if (!seen.length) fail("the override case wrote nothing at all");
    const numeric = seen.filter((v) => options.indexOf(v) < 0);
    if (numeric.length)
      fail("options_as_string: true no longer forces names -- " + numeric.length +
           " of " + seen.length + " writes were indices, e.g. " +
           JSON.stringify(numeric.slice(0, 3)));
  }

  /* ============================================== 4. THE CACHE-POLLUTION TRAP
   * Detection must still hold after the grid has written many times, and
   * across a page-away-and-back rebuild of the knob state. The grid`s own
   * writes land in s.values; a verdict derived from that cache flips to
   * "index" the moment the grid writes an index once, and never recovers.
   */
  {
    const { hierarchy, chainParams, options } = chordContract({ withOverride: false });
    const dsp = nameOnlyDsp(options, options.indexOf("major"));
    const h = harness({ hierarchy, chainParams, key: "type", dsp });
    h.settle(hierarchy.levels.root.knobs.length + 2);

    /* Five separate gestures, each several detents, with the read cursor
       sweeping in between -- the cache is thoroughly polluted by the end. */
    const marks = [];
    for (let g = 0; g < 5; g++) {
      for (let i = 0; i < 24; i++) h.detent(g % 2 ? -1 : 1);
      h.settle(hierarchy.levels.root.knobs.length + 2);
      marks.push(dsp.st.rejected);
    }
    if (marks[marks.length - 1] !== 0)
      fail("detection survived the first gesture but not the rest: rejects after " +
           "each gesture were " + JSON.stringify(marks) + ". The grid`s own write " +
           "is in s.values, so a verdict taken from that cache locks to `index` " +
           "as soon as the grid writes one");

    /* And the value the grid holds still reads as its option name, not as a
       stray index -- what the cell and the screen reader show. */
    const shown = h.ctl.state.values.type;
    if (options.indexOf(String(shown)) < 0)
      fail("after five gestures the grid holds " + JSON.stringify(shown) +
           " for an enum whose plugin speaks names");
  }

  /* ============================ 4b. THE FIRST GESTURE BEATS THE FIRST READ
   * The read cursor takes `keys + 1` ticks to come round, so a hand already on
   * the knob when the page opens turns it before any value has been read. An
   * implementation that only learns on the cursor writes indices until then --
   * and each of those indices lands in s.values, which is exactly the value a
   * cache-derived verdict would go on to read. This is the ordering that turns
   * "detected late" into "detected wrong, permanently".
   */
  {
    const { hierarchy, chainParams, options } = chordContract({ withOverride: false });
    const dsp = nameOnlyDsp(options, options.indexOf("major"));
    const h = harness({ hierarchy, chainParams, key: "type", dsp });
    /* No settle: straight from load to the knob. */
    for (let i = 0; i < 60; i++) h.detent(1);

    if (dsp.st.rejected)
      fail("turning before the read cursor arrives cost " + dsp.st.rejected +
           " rejected writes. Detection has to have an answer by the FIRST " +
           "write -- onKnobTurn already reads the value when it seeds the knob " +
           "state, and that read is from the device");
    if (dsp.index() <= options.indexOf("major"))
      fail("60 detents from a cold page left the value at " +
           JSON.stringify(dsp.st.value));
    /* And it kept the answer once the cursor started reporting too. */
    h.settle(hierarchy.levels.root.knobs.length + 2);
    const after = dsp.st.rejected;
    for (let i = 0; i < 60; i++) h.detent(-1);
    if (dsp.st.rejected !== after)
      fail("the verdict flipped once the read cursor caught up: " +
           (dsp.st.rejected - after) + " writes rejected afterwards");
  }

  /* ================================================= 5. no extra IPC per tick
   * The read that detection uses must be one the grid ALREADY makes. A param
   * round trip is ~2.8ms against a 1.68ms whole-page render; a per-frame or
   * per-detent read added here would be more expensive than the bug.
   */
  {
    const { hierarchy, chainParams, options } = chordContract({ withOverride: false });
    const dsp = nameOnlyDsp(options, options.indexOf("major"));
    const h = harness({ hierarchy, chainParams, key: "type", dsp });
    h.settle(hierarchy.levels.root.knobs.length + 2);

    h.stats.reads = 0;
    for (let i = 0; i < 20; i++) { h.advance(25); h.ctl.tick(); }
    if (h.stats.reads > 20)
      fail("20 ticks issued " + h.stats.reads + " reads -- the cursor budget is " +
           "one per tick and detection must not add another");

    h.stats.reads = 0;
    for (let i = 0; i < 40; i++) { h.advance(25); h.ctl.onKnobTurn(h.slot, 1, undefined); }
    if (h.stats.reads > 1)
      fail("40 detents issued " + h.stats.reads + " reads -- detection is reading " +
           "per detent. It must use the value the read cursor already fetched");
  }

  if (failures) process.exit(1);
  console.log("PASS: the knob grid learns an enum`s wire format from the plugin, " +
              "survives its own write cache, keeps index plugins on indices, and " +
              "adds no reads");
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
