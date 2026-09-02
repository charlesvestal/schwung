#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Global Settings expressed as a synthesised module contract.
#
# PURE, like the slot / Master FX contract next to it: hand it accessors and it
# tests with no UI, no device and no framebuffer. So everything below runs in a
# bare node process with no host globals defined — which is itself assertion 1,
# because a contract that reached for shadow_get_param would throw here and
# would ALSO throw on the device the first time it was built before the shim
# answered.
#
# The rest pins the properties that fail SILENTLY:
#
#   - a section that grew a ninth param does not error, it paginates, and the
#     "one section, one page" property that makes sections-as-levels work is
#     gone without a symptom. Audio sits at exactly 8.
#   - an enum with no `options` is not divable and shows a bare index.
#   - an option longer than three characters does not overflow the enum square,
#     it wraps across two lines of the 5x3 font and reads as gibberish
#     ("THRU" -> "THR/U"). short_options is the one mechanism for that, and
#     usbc_out_persist's wire annotation goes through it rather than through a
#     per-surface special case.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const FS = await import("node:fs");
const G = await import(R + "/src/shadow/shadow_ui_global_grid.mjs");
const { planPages, PAGE_KNOBS, PAGE_MENU, KNOBS_PER_PAGE } =
    await import(R + "/src/shared/param_pages/page_plan.mjs");
const { validateContract } = await import(R + "/src/shared/param_pages/validate_contract.mjs");
const { buildMetaIndex } = await import(R + "/src/shared/param_pages/param_meta.mjs");

/* ---- 1. purity ----------------------------------------------------------
 *
 * No host global is defined in this process, so a contract that read one would
 * throw right here. Asserted twice: once by CALLING it, and once against the
 * source, because a read guarded by `typeof x === "function"` would survive the
 * call and still be a global reaching into a module documented as pure.
 */
let contract = null;
{
  const io = { readParam: () => "0", writeParam: () => {} };
  try {
    contract = G.buildGlobalSettingsContract(io);
  } catch (e) {
    fail("buildGlobalSettingsContract threw with no host globals defined: " + (e && e.message));
  }
  if (!contract || !contract.hierarchy || !contract.chainParams) {
    fail("the contract must be { hierarchy, chainParams }, got " + JSON.stringify(contract));
  }
  /* Built with NO io at all: the contract is a declaration, and a declaration
   * that needs an accessor to exist has already stopped being one. */
  try { G.buildGlobalSettingsContract(); }
  catch (e) { fail("the contract must build with no io at all: " + (e && e.message)); }

  const src = FS.readFileSync(R + "/src/shadow/shadow_ui_global_grid.mjs", "utf8");
  const code = src.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
  const forbidden = /\b(shadow_get_param|shadow_set_param|host_[a-z_]+|tts_[a-z_]+|overlay_knobs_[a-z_]+|display_mirror_[a-z_]+|globalThis)\b/;
  const m = code.match(forbidden);
  if (m) fail("the contract module reads a host global: " + m[0]);
}
if (!contract) { console.error("FAIL: no contract to test"); process.exit(1); }

const { hierarchy, chainParams } = contract;

/* ---- 2. the seven levels ------------------------------------------------ */
const WANT = ["display", "audio", "accessibility", "set_pages", "shortcuts", "services", "updates"];
{
  for (const lv of WANT) {
    if (!hierarchy.levels[lv]) fail("missing level: " + lv);
  }
  /* Seven and only seven, plus root. An eighth level is a page nobody asked
   * for, and [Help...] becoming one is the specific way that happens. */
  const got = Object.keys(hierarchy.levels).filter((k) => k !== "root").sort();
  if (got.join(",") !== WANT.slice().sort().join(",")) {
    fail("levels should be exactly " + WANT.join(",") + " (plus root), got " + got.join(","));
  }
}

/* ---- 3. page kinds: six grids and one menu ------------------------------ */
const plan = planPages({ hierarchy, chainParams });
{
  const byLevel = {};
  for (const p of plan.pages) byLevel[p.level || p.name] = p.kind;

  if (byLevel["updates"] !== PAGE_MENU) {
    fail("updates must plan to PAGE_MENU (two actions, nothing to show), got " +
         JSON.stringify(byLevel["updates"]));
  }
  for (const lv of WANT.filter((x) => x !== "updates")) {
    if (byLevel[lv] !== PAGE_KNOBS) {
      fail(lv + " must plan to PAGE_KNOBS, got " + JSON.stringify(byLevel[lv]));
    }
  }
  /* Exactly seven pages. One per section is the property that makes
   * sections-as-levels work without the bank bar papering over a split, and it
   * is the thing a ninth param in any section silently destroys. */
  if (plan.pages.length !== 7) {
    fail("expected exactly 7 pages, one per section, got " + plan.pages.length + ": " +
         plan.pages.map((p) => p.name + "/" + p.kind).join(", "));
  }
  /* [Help...] is a navigation entry into the existing help stack. It must not
   * become an eighth page. */
  if (plan.pages.some((p) => /help/i.test(String(p.name)))) {
    fail("[Help...] must not plan to a page: " + plan.pages.map((p) => p.name).join(", "));
  }
}

/* ---- 4. every section fits ONE page ------------------------------------- */
{
  /* The counts from GLOBAL_SETTINGS_SECTIONS, transcribed. Asserted per level
   * rather than only in aggregate: a param that moved from one section to
   * another keeps the total at 24 and both totals-based checks green. */
  /* BOTH Display and Audio now sit at eight, which is the page limit exactly —
   * one more param in either and the section paginates silently. */
  /* Audio is still 8: the two Skipback rows moved to Shortcuts to make room
   * for the metronome pair, and then Audition (browser_preview) moved to
   * Display to make room for Save Stems. The "exactly 7 pages" and "Audio is
   * at KNOBS_PER_PAGE exactly" assertions below are UNCHANGED — they are what
   * catches a spill, and a spill is the thing those moves exist to avoid.
   *
   * With Display at 8 there is no longer a section with a free slot. The next
   * global setting has to displace one, and the displacement has to be
   * argued for in the declaration rather than discovered here. */
  const WANT_COUNT = { display: 8, audio: 8, accessibility: 6, set_pages: 1, shortcuts: 4, services: 2 };
  for (const p of plan.pages) {
    if (p.kind !== PAGE_KNOBS) continue;
    const keys = (p.keys || []).filter(Boolean);
    if (keys.length > KNOBS_PER_PAGE) {
      fail(p.name + " holds " + keys.length + " params — a section must fit ONE page, " +
           "or it paginates silently and sections-as-levels stops holding");
    }
    const want = WANT_COUNT[p.level];
    if (want === undefined) { fail("unexpected grid page level: " + p.level); continue; }
    if (keys.length !== want) {
      fail(p.level + " should hold " + want + " params (from GLOBAL_SETTINGS_SECTIONS), got " +
           keys.length + ": " + keys.join(", "));
    }
  }
  /* Audio is at the limit exactly. Stated on its own so the reason survives. */
  const audio = plan.pages.find((p) => p.level === "audio");
  if (!audio || (audio.keys || []).filter(Boolean).length !== KNOBS_PER_PAGE) {
    fail("Audio has exactly " + KNOBS_PER_PAGE + " params — one more and it paginates");
  }

  /* Every knob param must resolve to declared metadata, or the grid invents a
   * float 0..1 step 0.01 and writes 0.058750 into an enum. */
  const meta = buildMetaIndex({ hierarchy, chainParams });
  for (const p of plan.pages) {
    for (const k of (p.keys || [])) {
      if (!k) continue;
      if (meta.getOrGuess(k).guessed) fail(k + " has no declared metadata — the grid would guess it");
    }
  }
}

/* ---- 5. every enum is listable, and long values carry short forms -------- */
{
  let enums = 0, shorts = 0;
  for (const cp of chainParams) {
    if (cp.type !== "enum") continue;
    enums++;
    if (!Array.isArray(cp.options) || cp.options.length === 0) {
      fail(cp.key + " is an enum with no options — it is not divable and shows a bare index");
      continue;
    }
    const tooLong = cp.options.filter((o) => String(o).length > 3);
    if (tooLong.length && !Array.isArray(cp.short_options)) {
      fail(cp.key + " has options the 3-char enum square cannot hold (" +
           JSON.stringify(tooLong.slice(0, 3)) + ") and no short_options");
    }
    if (Array.isArray(cp.short_options)) {
      shorts++;
      if (cp.short_options.length !== cp.options.length) {
        fail(cp.key + " declares " + cp.options.length + " options and " +
             cp.short_options.length + " short_options — the square would read undefined");
      }
      for (const s of cp.short_options) {
        if (String(s).length > 3) {
          fail(cp.key + " short_option " + JSON.stringify(s) + " does not fit the square");
        }
      }
    }
  }
  if (enums < 10) fail("only " + enums + " enums found — the transcription looks incomplete");
  if (shorts < 10) fail("only " + shorts + " enums carry short_options");

  /* The stored-value tables, where an enum index is not the stored value.
   * resample_bridge is [0, 2] — an index-is-value assumption writes 1, which
   * is a mode that does not exist. */
  for (const key in G.GLOBAL_ENUM_VALUES) {
    const cp = chainParams.find((p) => p.key === key);
    if (!cp) { fail("GLOBAL_ENUM_VALUES names a param that is not declared: " + key); continue; }
    if (G.GLOBAL_ENUM_VALUES[key].length !== cp.options.length) {
      fail(key + " has " + cp.options.length + " options and " +
           G.GLOBAL_ENUM_VALUES[key].length + " stored values — one index maps nowhere");
    }
  }
  if ((G.GLOBAL_ENUM_VALUES.resample_bridge || []).join(",") !== "0,2") {
    fail("resample_bridge stores [0, 2], not consecutive indexes — got " +
         JSON.stringify(G.GLOBAL_ENUM_VALUES.resample_bridge));
  }
}

/* ---- 6. usbc_out_persist IS A BOOL; the annotation is a readout ---------
 *
 * It is On or Off -- whether Schwung restores the USB-C out source at boot. The
 * "(Main Out)" suffix REPORTS the source last seen on the wire, which matters
 * because Move own Settings screen goes stale after Schwung restores the value.
 *
 * It was briefly three options, then four, and both were wrong: putting the
 * annotation in the option SET turns one choice into several indistinguishable
 * "On"s to jog past, and implies the source is selectable here. It is not --
 * it is read-only and Move own menu chooses it. Reported from the device:
 * "it should be on or off and the () shows the last saved value".
 */
{
  const usbc = chainParams.find((p) => p.key === "usbc_out_persist");
  if (!usbc) fail("usbc_out_persist is missing from the contract");
  else {
    if (usbc.type !== "enum") fail("usbc_out_persist must be an enum");
    const opts = (usbc.options || []).map(String);
    if (opts.length !== 2) {
      fail("usbc_out_persist is a BOOL and must declare exactly two options -- the " +
           "wire source is a readout on the On label, not a state you can jog to. " +
           "Got " + JSON.stringify(opts));
    }
    if (opts[0] !== "Off") fail("usbc_out_persist index 0 must be Off, got " + JSON.stringify(opts[0]));
    if (!/^On/.test(opts[1] || "")) fail("usbc_out_persist index 1 must start with On, got " + JSON.stringify(opts[1]));
    if (!Array.isArray(usbc.short_options) || usbc.short_options.length !== 2
        || usbc.short_options[0] !== "OFF" || usbc.short_options[1] !== "ON") {
      fail("usbc_out_persist square must read OFF/ON, got " + JSON.stringify(usbc.short_options));
    }
  }
}

/* ---- 6b. the On label reports the observed source, and only when observed --
 *
 * annotateUsbcOption is where the readout lives, applied per entry because the
 * contract is rebuilt every time the screen opens. Unobserved must leave a
 * PLAIN "On": naming a source nothing has seen misleads exactly the user who
 * came here because Move screen was lying.
 */
{
  const decl = () => [{ key: "usbc_out_persist", type: "enum",
                        options: ["Off", "On"], short_options: ["OFF", "ON"] }];
  const mk = (src) => G.annotateUsbcOption(decl(),
      { readParam: (k) => (k === "usbc_out_source" ? src : "0") })[0].options[1];

  if (mk("1") !== "On (Main Out)") fail("source 1 must annotate On (Main Out), got " + JSON.stringify(mk("1")));
  if (mk("0") !== "On (Mic)")      fail("source 0 must annotate On (Mic), got " + JSON.stringify(mk("0")));
  if (mk("-1") !== "On")           fail("an unobserved source must leave a plain On, got " + JSON.stringify(mk("-1")));
  if (mk("") !== "On")             fail("an unserved source must leave a plain On, got " + JSON.stringify(mk("")));

  /* Must not mutate its input: the declaration is module-level and shared, so a
   * mutating annotate would carry one entry annotation into the next. */
  const shared = decl();
  G.annotateUsbcOption(shared, { readParam: () => "1" });
  if (shared[0].options[1] !== "On") {
    fail("annotateUsbcOption mutated its input -- the shared declaration would keep " +
         "a previous entry annotation. Got " + JSON.stringify(shared[0].options[1]));
  }
}

/* ---- 6c. names are written out in full ----------------------------------
 *
 * The first cut spelled every name for the eight-cell knob grid -- "Pad Typ",
 * "Text Prv", "Move>Sch", "Brws Prv", "Auto Chk" -- and then the screen was
 * pinned to the LIST, which has room for the whole word. Reported from the
 * device: "why are these truncated?"
 *
 * They were wrong for the grid too: labelForCell / WORD_ABBREV in
 * render_page_movy.mjs already squeeze a name into a cell, per word and with a
 * fixed mnemonic per concept. Abbreviating here duplicates a renderer job and
 * does it worse.
 *
 * The values are the labels the bespoke screen used, restored verbatim. Pinned
 * because an abbreviation is a plausible-looking edit that nothing else fails
 * on -- and because one of them ("MIDI Ch" for midi_indicator_enabled) collided
 * with Master FX genuine MIDI Ch, its listen channel, which is a different
 * setting entirely.
 */
{
  const EXPECT = {
    display_mirror: "Mirror Display", overlay_knobs: "Overlay",
    pad_typing: "Pad Typing", text_preview: "Show Typed",
    midi_indicator_enabled: "Show MIDI", param_view: "Param View",
    stay_in_shadow: "Keep Schwung",
    link_audio_routing: "Move->Schwung", link_audio_publish: "Schwung->Link",
    latency_comp_enabled: "Latency Comp", resample_bridge: "Resample",
    skipback_shortcut: "Skipback", skipback_seconds: "Skipback Len",
    /* "Audition", not "Audition Files": this row now gates the User Presets
       scroll audition as well as the file browser WAV preview, so the noun
       narrowed it to something it no longer only means. Still written out in
       full, which is what this pin is actually protecting. */
    browser_preview: "Audition", usbc_out_persist: "USB-C",
    screen_reader_enabled: "Screen Reader", screen_reader_engine: "Engine",
    screen_reader_speed: "Speed", screen_reader_pitch: "Pitch",
    screen_reader_volume: "Volume", screen_reader_debounce: "Speak Delay",
    set_pages_enabled: "Set Pages", shadow_ui_trigger: "Open With",
    recall_quantize: "Recall Q",
    /* Names written out in full, like every row above: the cell renderer
       abbreviates (labelForCell / WORD_ABBREV), the declaration does not. */
    metronome_mode: "Metronome", metronome_level: "Click Vol",
    /* One word, because it names the whole question the three options answer
       ("Master / Stems / Both") and the options are right beside it. */
    save_stems: "Save",
    filebrowser_enabled: "File Browser",
    analytics_enabled: "Analytics",
  };
  let seen = 0;
  for (const p of chainParams) {
    const want = EXPECT[p.key];
    if (want === undefined) { fail("unexpected param in the contract: " + p.key); continue; }
    seen++;
    if (p.name !== want) {
      fail("param " + p.key + " is named " + JSON.stringify(p.name) + ", expected " +
           JSON.stringify(want) + " -- names are written out in full here; the cell " +
           "renderer abbreviates (labelForCell / WORD_ABBREV)");
    }
  }
  const total = Object.keys(EXPECT).length;
  if (seen !== total) {
    fail("expected " + total + " params in the contract, saw " + seen +
         " -- a param was added or removed without updating this list");
  }
}

/* ---- 7. the shared validator accepts it --------------------------------- */
{
  const report = validateContract({ id: "global_settings", hierarchy, chainParams });
  const errors = (report.findings || []).filter((f) => f.level === "error");
  if (errors.length) {
    fail("validateContract reported " + errors.length + " error(s): " +
         errors.map((f) => f.rule + ": " + f.message).join("; "));
  }
  /* Warnings are allowed but not ignored — print them so a transcription slip
   * that only warns is still visible to whoever runs this. */
  const warns = (report.findings || []).filter((f) => f.level === "warn");
  for (const w of warns) console.error("  warn: " + w.rule + ": " + w.message);
  if (warns.length) fail(warns.length + " validator warning(s) — see above");
}

/* ---- 8. accessor routing: every key has a backend ------------------------
 *
 * A declared param with no routing entry reads BLANK and writes NOWHERE. The
 * grid does not error on that — it draws an empty cell and swallows the turn —
 * so it is exactly the shape of failure this file exists to catch.
 */
{
  const routed = Object.keys(G.GLOBAL_ROUTING);
  for (const cp of chainParams) {
    if (!G.GLOBAL_ROUTING[cp.key]) {
      fail(cp.key + " is declared in the contract but has no GLOBAL_ROUTING entry — " +
           "it would read blank and write nowhere");
    }
  }
  for (const k of routed) {
    if (!chainParams.some((cp) => cp.key === k)) {
      fail("GLOBAL_ROUTING routes " + k + ", which the contract does not declare");
    }
  }
  if (routed.length !== chainParams.length) {
    fail("GLOBAL_ROUTING has " + routed.length + " entries for " + chainParams.length + " params");
  }
  /* Every entry must actually name a backend on both sides. A half-filled row
   * is the same blank cell with a plausible-looking table above it. */
  for (const k of routed) {
    const r = G.GLOBAL_ROUTING[k];
    if (!r.read || !r.write) fail(k + " routing entry is missing a read or write backend");
    if (!(r.persist === null || r.persist === "save" || r.persist === "own")) {
      fail(k + " has an unknown persist kind: " + JSON.stringify(r.persist));
    }
  }
}

/* ---- 9. PERSISTENCE: a write that should save must call save --------------
 *
 * These keys set a cached module-level var AND call saveMasterFxChainConfig in
 * the code this contract replaces. A writeParam that skips either sets the
 * param and loses it on reboot -- silently. There is no error, no wrong value
 * on screen, and no symptom until the device comes back up, which is why it is
 * pinned here rather than trusted to review.
 */
{
  const spy = () => {
    const s = { writes: [], persists: 0, values: {} };
    s.io = {
      readParam: (k) => (k in s.values ? s.values[k] : "0"),
      writeParam: (k, v) => { s.writes.push([k, v]); },
      persist: () => { s.persists++; },
    };
    return s;
  };

  /* Transcribed from the six saveMasterFxChainConfig() calls in
   * adjustMasterFxSetting. Asserted against PERSISTING_KEYS as a SET, so a key
   * that quietly stopped persisting fails here, not on a device. */
  const WANT_PERSIST = ["overlay_knobs", "link_audio_routing", "link_audio_publish",
                        "latency_comp_enabled", "resample_bridge", "usbc_out_persist"].sort();
  const got = Array.from(G.PERSISTING_KEYS).sort();
  if (got.join(",") !== WANT_PERSIST.join(",")) {
    fail("PERSISTING_KEYS should be exactly " + WANT_PERSIST.join(",") + ", got " + got.join(","));
  }

  for (const k of WANT_PERSIST) {
    const s = spy();
    G.writeGlobalParam(s.io, k, 1);
    if (s.writes.length !== 1) {
      fail("writing " + k + " should write exactly once, got " + JSON.stringify(s.writes));
    }
    if (s.persists !== 1) {
      fail("writing " + k + " must call persist() (saveMasterFxChainConfig) — it sets a cached " +
           "var that config serialises, so without the save the setting is lost on reboot; " +
           "got " + s.persists + " calls");
    }
  }

  /* NOT VACUOUS. If every key persisted, the loop above would pass no matter
   * what writeGlobalParam did with the set. These four cover all three of the
   * other persistence kinds: an own saver (pad_typing, filebrowser_enabled),
   * a self-persisting backend (screen_reader_speed) and a feature flag
   * (analytics_enabled). None may reach the shared sink. */
  for (const k of ["pad_typing", "filebrowser_enabled", "screen_reader_speed", "analytics_enabled"]) {
    if (G.PERSISTING_KEYS.has(k)) {
      fail(k + " does not call saveMasterFxChainConfig in the code being replaced — " +
           "including it makes the persistence assertion vacuous");
    }
    const s = spy();
    G.writeGlobalParam(s.io, k, 1);
    if (s.persists !== 0) {
      fail("writing " + k + " must NOT call persist(): its persistence is elsewhere " +
           "(or nowhere), and a set where everything persists proves nothing");
    }
    if (s.writes.length !== 1) fail("writing " + k + " should still write once");
  }

  /* persist is OPTIONAL on the io — an io without one must not throw. */
  try {
    G.writeGlobalParam({ readParam: () => "0", writeParam: () => {} }, "resample_bridge", 1);
  } catch (e) {
    fail("writeGlobalParam must tolerate an io with no persist(): " + (e && e.message));
  }
}

/* ---- 10. stored values are not indexes ----------------------------------
 *
 * resample_bridge stores 0 and **2**. The knob engine and the enum picker both
 * work in INDEXES, so an index-is-value write sets mode 1 — which does not
 * exist — and the setting appears to do nothing at all. Round-tripped in both
 * directions because either half alone can be wrong and still look consistent.
 */
{
  const wrote = [];
  const io = { readParam: () => "0", writeParam: (k, v) => wrote.push(v) };

  G.writeGlobalParam(io, "resample_bridge", 0);
  G.writeGlobalParam(io, "resample_bridge", 1);
  if (wrote.join(",") !== "0,2") {
    fail("resample_bridge indexes 0 and 1 must store 0 and 2, got [" + wrote.join(", ") + "] — " +
         "writing the index sets mode 1, a mode that does not exist");
  }

  /* Back the other way: the STORED value must resolve to the index the grid
   * draws. Reading 2 as index 2 would run off the end of a 2-option list. */
  for (const [stored, index] of [["0", "0"], ["2", "1"]]) {
    const got = G.readGlobalParam({ readParam: () => stored }, "resample_bridge");
    if (got !== index) {
      fail("resample_bridge stored " + stored + " must read back as index " + index + ", got " +
           JSON.stringify(got));
    }
  }

  /* The same trap in the other two tables that are not 0..n-1. */
  const sec = [];
  const secIo = { readParam: () => "0", writeParam: (k, v) => sec.push(v) };
  G.writeGlobalParam(secIo, "skipback_seconds", 2);
  if (sec[0] !== "120") fail("skipback_seconds index 2 stores 120 seconds, got " + JSON.stringify(sec[0]));
  const eng = [];
  const engIo = { readParam: () => "espeak", writeParam: (k, v) => eng.push(v) };
  G.writeGlobalParam(engIo, "screen_reader_engine", 1);
  if (eng[0] !== "flite") fail("screen_reader_engine index 1 stores \"flite\", got " + JSON.stringify(eng[0]));
  if (G.readGlobalParam({ readParam: () => "flite" }, "screen_reader_engine") !== "1") {
    fail("screen_reader_engine stored \"flite\" must read back as index 1");
  }

  /* A failed read is not an index. null means the read did not complete and ""
   * means the channel served nothing; turning either into 0 reports "Native"
   * as fact. See the three-answers rule in CLAUDE.md. */
  for (const raw of [null, ""]) {
    const got = G.readGlobalParam({ readParam: () => raw }, "resample_bridge");
    if (got !== raw) {
      fail("a " + JSON.stringify(raw) + " read must pass through, not become an index; got " +
           JSON.stringify(got));
    }
  }
}

/* ---- 11. usbc_out_persist: two indexes, two stored values ---------------
 *
 * The bool round-trips as itself. There is no annotated index to collapse any
 * more -- putting the annotation in the option set was the modelling mistake
 * this replaced, and the readout now rides the On LABEL (assertion 6b).
 */
{
  const wrote = [];
  const io = { readParam: () => "0", writeParam: (k, v) => wrote.push(v) };
  for (const idx of [0, 1]) G.writeGlobalParam(io, "usbc_out_persist", idx);
  if (wrote.join(",") !== "0,1") {
    fail("usbc_out_persist indexes 0/1 must store 0/1, got [" + wrote.join(", ") + "]");
  }

  const read = (on, src) => G.readGlobalParam({
    readParam: (k) => (k === "usbc_out_source" ? src : on),
  }, "usbc_out_persist");
  /* The source must NOT move the index -- that is what made it selectable. */
  if (read("0", "1") !== "0") fail("usbc_out_persist off must read index 0 whatever the wire says");
  if (read("1", "1") !== "1") fail("usbc_out_persist on must read index 1 with source Main Out");
  if (read("1", "0") !== "1") fail("usbc_out_persist on must read index 1 with source Mic");
  if (read("1", "-1") !== "1") fail("usbc_out_persist on must read index 1 with the source unobserved");
  if (read(null, "1") !== null) fail("a failed usbc_out_persist read must pass through as null");
}

/* ---- 6d. EVERY ROW FITS ITS OWN WIDTH -----------------------------------
 *
 * A list row is one line: cursor prefix, label, gap, value, right edge. The
 * label room is therefore whatever the WIDEST value of that parameter leaves
 * behind -- so a name cannot be judged on its own. "Overlay Knobs" is a fine
 * name beside "Off" and an impossible one beside "+Jog Touch", and the first
 * cut of these names was chosen without measuring either.
 *
 * Measured with the real device font through the harness, not a 6px-per-glyph
 * estimate: that over-reserves by up to 4px a character and would condemn rows
 * that are fine.
 *
 * OURS ONLY. A sweep of the fleet fixture found 682 of 4598 module rows over
 * their width -- 14%, and 165 of those by seven characters or more. Truncation
 * on a list row is therefore normal, the marquee on the selected row is what
 * answers it, and module authors own their own names. What this pins is that WE
 * do not add another.
 */
{
  const M = await import(R + "/tools/param-pages/measure_labels.mjs");
  for (const r of chainParams.map((cp) => M.measureRow(cp)).filter((x) => !x.fits)) {
    fail("row " + JSON.stringify(r.name) + " needs " + r.need + "px but its widest "
       + "value (" + JSON.stringify(r.value) + ") leaves " + r.room + "px -- over by "
       + r.over + "px. Shorten the NAME; or if the VALUE is what does not fit, "
       + "shorten the option, because a four-character label is worse than a "
       + "shorter value.");
  }
}

if (failures) process.exit(1);
console.log("PASS: global settings contract — seven levels (8/8/6/1/4/2 params + Updates as a " +
            "menu), every section one page with Audio at the limit, every enum listable with a " +
            "matching short_options, usbc_out_persist a bool whose On label reports the observed source, " +
            "validator clean, no host global read, every key routed to a backend, the six " +
            "saveMasterFxChainConfig keys persisting and four others provably not, and " +
            "resample_bridge round-tripping [0, 2] rather than its indexes");
'
