#!/usr/bin/env node
/**
 * generate.mjs -- turn a catalogued module into preview assets.
 *
 *   node tools/probe/generate.mjs <id> [<id> ...] --out DIR [--tarball-dir D]
 *   node tools/probe/generate.mjs --all --out DIR
 *
 * Per module: fetch the release tarball -> probe the contract -> render the
 * knob pages -> render the audio -> encode. Emits <out>/index.json.
 *
 * EVERY FAILURE PUBLISHES A STATUS, NEVER A PLACEHOLDER. A module with no
 * preview shows no preview and the reason is in the JSON; a placeholder image
 * or a silent clip is indistinguishable from a working pipeline producing bad
 * output, which is how a broken preview sits in a catalog for months.
 */

import fs from "node:fs";
import path from "node:path";
import { execFileSync, execSync } from "node:child_process";

const ROOT = path.resolve(import.meta.dirname, "../..");
const SCORES = JSON.parse(fs.readFileSync(path.join(ROOT, "tools/probe/scores/scores.json"), "utf8"));
const CATALOG = JSON.parse(fs.readFileSync(path.join(ROOT, "module-catalog.json"), "utf8"));

const args = process.argv.slice(2);
const flag = (n, d = null) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const OUT = path.resolve(flag("--out", "build/previews"));
const TARBALLS = path.resolve(flag("--tarball-dir", "build/tarballs"));
const ids = args.filter((a) => !a.startsWith("--") && args[args.indexOf(a) - 1]?.startsWith("--") !== true);

/** The score rule for a module: its subcategory, else the type's `_all`. */
export function scoreFor(mod) {
  const table = SCORES[mod.component_type] || {};
  const rule = table[mod.subcategory] || table._all || null;
  if (!rule) return null;
  return { ...SCORES.default, ...rule };
}

const sh = (cmd, a, opts = {}) => execFileSync(cmd, a, { encoding: "utf8", ...opts });

function fetchTarball(mod) {
  fs.mkdirSync(TARBALLS, { recursive: true });
  const dir = path.join(TARBALLS, mod.id);
  if (fs.existsSync(dir)) return dir;
  const rel = `https://raw.githubusercontent.com/${mod.github_repo}/${mod.default_branch || "main"}/release.json`;
  let meta;
  try { meta = JSON.parse(sh("curl", ["-sL", "--max-time", "30", rel])); }
  catch { throw new Error(`no release.json at ${rel}`); }
  /* A repo publishing several catalog modules keys its releases by id. */
  const entry = meta.modules ? meta.modules[mod.id] : meta;
  if (!entry?.download_url) throw new Error(`release.json names no download_url for ${mod.id}`);
  const tgz = path.join(TARBALLS, `${mod.id}.tar.gz`);
  sh("curl", ["-sL", "--max-time", "120", "-o", tgz, entry.download_url]);
  fs.mkdirSync(dir, { recursive: true });
  sh("tar", ["xzf", tgz, "-C", dir, "--strip-components", "1"]);
  return dir;
}

/** The plugin object, by contract. An FX is `<id>.so`, a synth `dsp.so`. */
function findSo(dir, id) {
  for (const c of [`${id}.so`, "dsp.so"]) {
    const p = path.join(dir, c);
    if (fs.existsSync(p)) return p;
  }
  const any = fs.readdirSync(dir).find((f) => f.endsWith(".so"));
  return any ? path.join(dir, any) : null;
}

/**
 * 50% of the dry/wet range the module DECLARES. Never a guessed 0..1: the
 * range is read from the module's own metadata, and a module that declares no
 * range is left alone rather than written to.
 */
export function findWetParam(params) {
  const MIXY = /^(mix|dry_?wet|wet_?dry|wet|blend)$/i;
  for (const p of params || []) {
    if (!MIXY.test(p.key || "")) continue;
    if (p.type && p.type !== "float" && p.type !== "int") continue;
    if (p.min == null || p.max == null) continue;
    return { key: p.key, value: p.min + (p.max - p.min) * 0.5 };
  }
  return null;
}

/** An FX declares its params in module.json, not through get_param. */
export function paramsFromModuleJson(dir) {
  const p = path.join(dir, "module.json");
  if (!fs.existsSync(p)) return [];
  const caps = (JSON.parse(fs.readFileSync(p, "utf8")).capabilities) || {};
  const out = [];
  const walk = (o) => {
    if (Array.isArray(o)) o.forEach(walk);
    else if (o && typeof o === "object") {
      if (o.key && (o.min != null || o.max != null)) out.push(o);
      Object.values(o).forEach(walk);
    }
  };
  walk(caps.chain_params); walk(caps.ui_hierarchy);
  return out;
}


/* ------------------------------------------------------------ the pipeline */

const PROBE = path.join(ROOT, "build/probe");
const inDocker = fs.existsSync("/.dockerenv");
/* The probe is an ARM64 Linux binary. Off-device that means the same image
 * modules were built in; inside CI (already arm64 Linux) it runs directly. */
function runProbe(a, cwd = ROOT) {
  if (inDocker || process.platform === "linux") return sh(PROBE, a, { cwd, stdio: ["ignore", "pipe", "pipe"] });
  try {
    return sh("docker", ["run", "--rm", "--platform", "linux/arm64",
                         "-v", `${ROOT}:/w`, "-v", `${TARBALLS}:${TARBALLS}`, "-v", `${OUT}:${OUT}`,
                         "-w", "/w", "debian:bookworm", "build/probe", ...a],
              { stdio: ["ignore", "pipe", "pipe"] });
  } catch (e) {
    /* The probe explains itself on stderr; surfacing only the docker command
     * line turns a diagnosable failure into "Command failed". */
    const why = (e.stderr || "").toString().trim().split("\n").filter(Boolean).slice(-3).join(" | ");
    throw new Error(why || String(e.message).slice(0, 120));
  }
}

function have(bin) { try { execSync(`command -v ${bin}`, { stdio: "ignore" }); return true; } catch { return false; } }

/** One module, end to end. Returns its index.json entry -- never throws. */
async function generate(mod) {
  const entry = { id: mod.id, name: mod.name, component_type: mod.component_type,
                  subcategory: mod.subcategory, harness: "probe-isolated",
                  screens: { status: "error" }, audio: { status: "error" } };
  const rule = scoreFor(mod);
  const outDir = path.join(OUT, mod.id);
  fs.mkdirSync(outDir, { recursive: true });

  if (String(mod.requires || "").trim() !== "") {
    entry.screens = { status: "needs-assets", detail: mod.requires };
    entry.audio   = { status: "needs-assets", detail: mod.requires };
    return entry;
  }

  let dir, so;
  try {
    dir = fetchTarball(mod);
    so = findSo(dir, mod.id);
  } catch (e) {
    entry.screens = entry.audio = { status: "error", detail: String(e.message) };
    return entry;
  }
  if (!so) {
    /* A module with no DSP is not a failure -- an overtake or tool module is
     * JS and owns the whole surface, so it has no plugin to load and no knob
     * grid to plan. Its screens need the separate QuickJS pass. */
    const expected = mod.component_type === "overtake" || mod.component_type === "tool";
    const st = expected ? "js-only" : "error";
    const detail = expected ? "no DSP plugin; screens need the JS UI pass"
                            : "no .so in the tarball";
    entry.screens = { status: st, detail };
    entry.audio = { status: expected ? "not-applicable" : "error", detail };
    return entry;
  }

  // ---- contract -> screens
  const contractPath = path.join(outDir, "contract.json");
  try {
    runProbe(["--contract", dir, "--so", so, "--id", mod.id, "-o", contractPath]);
    const c = JSON.parse(fs.readFileSync(contractPath, "utf8"));
    entry.version = readVersion(dir);
    if (c.ui_hierarchy == null) {
      /* null is a FAILED read, not "this module has no hierarchy" -- never
       * let it become a plan, a default, or a published picture. */
      entry.screens = { status: "no-screens", detail: "ui_hierarchy unserved" };
    } else {
      const shots = renderScreens(mod, c, entry.version, outDir, dir);
      entry.screens = shots.length
        ? { status: "ok", kind: "grid-synthesised", hero: shots[0], gallery: shots.slice(1) }
        : { status: "no-screens", detail: "the planner produced no pages" };
    }
  } catch (e) {
    entry.screens = { status: "error", detail: String(e.message).slice(0, 200) };
  }

  // ---- audio
  if (rule?.audio === null) {
    entry.audio = { status: "not-applicable", detail: rule._why || "no deterministic source" };
    return entry;
  }
  if (!have("ffmpeg")) { entry.audio = { status: "error", detail: "ffmpeg not found" }; return entry; }
  try {
    entry.audio = renderAudio(mod, rule, dir, so, outDir);
  } catch (e) {
    entry.audio = { status: "error", detail: String(e.message).slice(0, 200) };
  }
  return entry;
}

function readVersion(dir) {
  try { return JSON.parse(fs.readFileSync(path.join(dir, "module.json"), "utf8")).version || ""; }
  catch { return ""; }
}

/** Feed the contract to the renderer the DEVICE uses (movy), not the dial grid. */
function renderScreens(mod, contract, version, outDir, dir) {
  const fixture = {
    _source: "Captured off-device by tools/probe.",
    generated_at: new Date().toISOString(), module_count: 1, not_captured: [],
    modules: [{ id: mod.id, category: mod.component_type,
                component_key: mod.component_type === "audio_fx" ? "fx1" : "synth",
                status: "ok", name: mod.name, version,
                ui_hierarchy: contract.ui_hierarchy, chain_params: contract.chain_params, presets: null }],
  };
  const fx = path.join(outDir, "fixture.json");
  fs.writeFileSync(fx, JSON.stringify(fixture));
  const shotDir = path.join(outDir, "screens");
  fs.mkdirSync(shotDir, { recursive: true });
  const a = [path.join(ROOT, "tools/param-pages/preview.mjs"), mod.id,
             "--fixture", fx, "--all", "--layout", "movy", "--png", shotDir, "--scale", "4"];
  /* A module's OWN widgets, if it ships any. Without this every `custom:` kind
   * falls through to a built-in dial -- a correct fallback and a WRONG
   * PICTURE, with nothing logged, so the page looks fine and is not what the
   * device draws. */
  if (fs.existsSync(path.join(dir, "canvas.js"))) a.push("--widgets", dir);
  sh("node", a, { cwd: ROOT, stdio: ["ignore", "pipe", "pipe"] });
  return fs.readdirSync(shotDir).filter((f) => f.endsWith(".png")).sort()
           .map((f) => `${mod.id}/screens/${f}`);
}

function renderAudio(mod, rule, dir, so, outDir) {
  const audioDir = path.join(outDir, "audio");
  fs.mkdirSync(audioDir, { recursive: true });

  const scorePath = path.join(outDir, "score.json");
  if (!rule.midi) return { status: "not-applicable", detail: "no melodic score for this subcategory yet" };
  sh("node", [path.join(ROOT, "tools/probe/mid2score.mjs"),
              path.join(ROOT, "tools/probe/scores", "..", rule.midi), scorePath,
              "--seconds", String(rule.seconds)], { cwd: ROOT, stdio: ["ignore", "pipe", "pipe"] });

  const a = ["--render", dir, "--so", so, "--id", mod.id, "--score", scorePath,
             "-o", path.join(audioDir, `${mod.id}.wav`), "--presets", String(rule.presets)];
  if (rule.octave_fit) a.push("--octave-fit");

  /* An FX or a MIDI FX is previewed THROUGH a reference instrument, pinned, so
   * that a preview which changes means the MODULE changed -- and so effects
   * are comparable with each other and with the synths. */
  if (rule.source) {
    const srcMod = CATALOG.modules.find((x) => x.id === rule.source);
    if (!srcMod) throw new Error(`score names source "${rule.source}", which is not in the catalog`);
    const srcDir = fetchTarball(srcMod);
    const srcSo = findSo(srcDir, srcMod.id);
    if (!srcSo) throw new Error(`source ${rule.source} has no .so`);
    a.push("--source-dir", srcDir, "--source-so", srcSo);
  }

  /* Dry/wet at 50% of the DECLARED range, or not at all. */
  let wet = null;
  if (rule.wet != null) {
    const declared = paramsFromModuleJson(dir);
    wet = findWetParam(declared);
    if (wet) a.push("--set", `${wet.key}=${wet.value}`);
  }

  const renders = JSON.parse(runProbe(a));
  const clips = [];
  for (const r of renders) {
    const m4a = r.file.replace(/\.wav$/, ".m4a");
    /* -ar 44100 is REQUIRED: loudnorm resamples internally and ffmpeg then
     * picks 96 kHz for AAC. Every preview currently on the site is 96 kHz for
     * exactly this reason -- same bytes at 128 kbps, spent above 20 kHz. */
    sh("ffmpeg", ["-y", "-loglevel", "error", "-i", r.file,
                  "-af", "loudnorm=I=-16:TP=-1.5:LRA=11", "-ar", "44100",
                  "-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart", m4a]);
    fs.unlinkSync(r.file);
    clips.push({ preset: r.preset, name: r.name, rms_dbfs: r.rms_dbfs,
                 octave_shift: r.octave_shift, fit: r.fit,
                 file: path.relative(OUT, m4a) });
  }
  /* A clip that is effectively silent is a FAILURE, not an asset: loudnorm
   * would amplify the noise floor and publish it as a preview. */
  const usable = clips.filter((c) => c.rms_dbfs > -60);
  if (!usable.length) return { status: "error", detail: "every render was silent" };
  return { status: "ok", score: rule.midi, wet, clips: usable,
           dropped: clips.length - usable.length || undefined };
}

// ------------------------------------------------------------------ driver
const wanted = args.includes("--all")
  ? CATALOG.modules
  : CATALOG.modules.filter((m) => ids.includes(m.id));
if (!wanted.length) { console.error("usage: generate.mjs <id>... | --all  --out DIR"); process.exit(2); }

fs.mkdirSync(OUT, { recursive: true });
const index = { generated_at: new Date().toISOString(), harness_version: "1", modules: {} };
for (const mod of wanted) {
  process.stderr.write(`\n=== ${mod.id} (${mod.component_type}/${mod.subcategory})\n`);
  const e = await generate(mod);
  index.modules[mod.id] = e;
  process.stderr.write(`    screens: ${e.screens.status}${e.screens.hero ? ` (${(e.screens.gallery?.length ?? 0) + 1} pages)` : ""}` +
                       `   audio: ${e.audio.status}${e.audio.clips ? ` (${e.audio.clips.length} clips)` : ""}\n`);
  if (e.screens.detail) process.stderr.write(`      screens: ${e.screens.detail}\n`);
  if (e.audio.detail)   process.stderr.write(`      audio:   ${e.audio.detail}\n`);
}
fs.writeFileSync(path.join(OUT, "index.json"), JSON.stringify(index, null, 2));
process.stderr.write(`\nwrote ${path.join(OUT, "index.json")}\n`);

