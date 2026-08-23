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
   * another keeps the total at 25 and both totals-based checks green. */
  const WANT_COUNT = { display: 6, audio: 8, accessibility: 6, set_pages: 1, shortcuts: 1, services: 3 };
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

/* ---- 6. usbc_out_persist carries the annotation as a LONG option --------
 *
 * It renders as "On (Main Out)": a bool annotated with the source last seen on
 * the wire, because Move own Settings screen goes stale after Schwung restores
 * the value, so this row is the only honest read of what is actually routed. A
 * 3-char square cannot show that — which is what short_options is FOR. One
 * declaration, two renderings, no per-surface branch.
 */
{
  const usbc = chainParams.find((p) => p.key === "usbc_out_persist");
  if (!usbc) fail("usbc_out_persist is missing from the contract");
  else {
    if (usbc.type !== "enum") fail("usbc_out_persist must be an enum to carry the annotation");
    const opts = (usbc.options || []).map(String);
    if (!opts.some((o) => /Main Out/.test(o))) {
      fail("usbc_out_persist long options must carry the Main Out annotation, got " +
           JSON.stringify(opts));
    }
    if (!opts.some((o) => /Mic/.test(o))) {
      fail("usbc_out_persist long options must carry the Mic annotation, got " + JSON.stringify(opts));
    }
    if (!Array.isArray(usbc.short_options) || usbc.short_options.length !== opts.length) {
      fail("usbc_out_persist needs a short_options of matching length for the square");
    } else {
      for (let i = 0; i < opts.length; i++) {
        const on = /^On/.test(opts[i]);
        const want = on ? "ON" : "OFF";
        if (usbc.short_options[i] !== want) {
          fail("usbc_out_persist square at index " + i + " should read " + want +
               " (the square shows the BOOL; the annotation is the long form), got " +
               JSON.stringify(usbc.short_options[i]));
        }
      }
    }
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

if (failures) process.exit(1);
console.log("PASS: global settings contract — seven levels (6/8/6/1/1/3 params + Updates as a " +
            "menu), every section one page with Audio at the limit, every enum listable with a " +
            "matching short_options, usbc_out_persist annotated in the long form only, " +
            "validator clean, and no host global read");
'
