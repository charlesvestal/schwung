# Codebase Cleanup Review — 2026-06-11

Full-codebase review for consolidation, dead-code removal, legibility, UI/UX
consistency, and stability. Six parallel review passes: shadow UI JS, shim/shadow C,
Signal Chain subsystem, host runtime C, shared JS + modules, build/docs/hygiene.

**Constraint honored throughout: module compatibility.** The frozen surfaces are:
`plugin_api_v1.h` (filename + v2 ABI), `audio_fx_api_v2.h`, `midi_fx_api_v1.h`,
SHM layouts (`shadow_constants.h`, `link_audio.h`, `norns_display_shm.h`),
the JS module contract (`globalThis.init/tick/onMidiMessage*`, `host_*` function
names/signatures), `module.json` semantics, and `shared/*.mjs` exports — external
repos import `sound_generator_ui.mjs`, `chain_ui_views.mjs`, `menu_*.mjs`,
`scrollable_text.mjs`, `knob_engine.mjs`, `store_utils.mjs` directly.
Nothing below breaks these; behavioral items are flagged.

---

## 0. Actual bugs found during review (fix before/independent of cleanup)

| # | Bug | Location | Severity |
|---|-----|----------|----------|
| B1 | **MPE THRU bypass missing.** `any_thru_slot_active()` exists but is never called; `shim_remap_cable2_channels` rewrites channels even when a slot is forward=THRU, destroying per-note expression. CLAUDE.md documents the bypass as existing. | `src/schwung_shim.c:5751–5758` (dead fn), `:5760` (missing check) | HIGH |
| B2 | **Silent patch truncation.** `v2_save_patch`/`v2_update_patch` wrap chain JSON into `char final_json[8192]` via snprintf. Surge-class synth state (~8–16 KB; SHM transport allows 64 KB) writes a truncated, unparseable patch file and logs "Saved patch". Same family as the autosave silent-bail bug. | `src/modules/chain/dsp/chain_host.c:5678` and the `v2_update_patch` twin | HIGH |
| B3 | **MIDI_IN stride inconsistency.** The remap code correctly strides 8 bytes (and documents why); the main filter loop (`:6068`), `midi_monitor` (`:4342`), indicator scans (`:4975`, `:4992`), Shift+Menu scan (`:6233`) stride 4 — timestamp words can decode as plausible events; the co-run knob injection (`:6207`) can write a packet into a timestamp slot. | `src/schwung_shim.c` (multiple) | MED-HIGH |
| B4 | **`load_feature_config` truncates `features.json` at 512 bytes** and strstr's 11 keys; keys past the cut silently revert to defaults (`ext_midi_remap_enabled`, `shadow_ui_trigger` are at risk). | `src/schwung_shim.c:916` | MED |
| B5 | **`doSavePreset` silent bail** — `if (!json) { /* TODO: show error message */ return; }`; user presses Save, nothing happens. Autosave bails were fixed; the interactive path was missed. Also `host_write_file` results ignored at `:4544/:4569` and `lastSavedSlotSignature` updated even on failed write. | `src/shadow/shadow_ui.js:4580` | MED |
| B6 | **settings-schema.json missing `midi_indicator_enabled` and `analytics_enabled`** — schwung-manager web UI can't show/edit them. shadow_ui.js:880 says "keep both in sync". | `src/shared/settings-schema.json` vs `shadow_ui.js:883–963` | MED |
| B7 | **`knob_forward_value` covers fx3 but silently not fx4** despite `MAX_AUDIO_FX = 4`. | `chain_host.c:2745` | LOW-MED |
| B8 | Overtake loader passes the **entire module.json** as `json_defaults` instead of the defaults object (works only because modules strstr their keys). | `src/schwung_shim.c:1504` | LOW |

---

## 1. Stability / realtime-safety (highest-priority theme)

The SPI callback path is `shim_pre_transfer` (`schwung_shim.c:4595`) →
`shim_post_transfer` (`:5839`), FIFO 90 on core 3, ~900 µs budget. The project's
own rules (CLAUDE.md, REALTIME_SAFETY.md) ban file I/O / allocation / locks /
`unified_log` here. Violations found, worst first:

### RT-1 (HIGH) Sampler stop does pthread_join + full WAV rewrite on the SPI thread
`sampler_stop_recording()` (`src/host/shadow_sampler.c:720–840`) joins the writer
thread, then fseek/fread/fwrite-rewrites the whole recording to trim preroll.
Called from the SPI path (`schwung_shim.c:2613`, fallback at `shadow_sampler.c:903`).
Stopping a long recording = guaranteed multi-hundred-ms stall. **Fix:** SPI path
sets a flag; a detached worker joins/trims/renames.

### RT-2 (HIGH) Sampler start does fopen/malloc/fork on the SPI thread
`fopen(sampler_cmd_path)` at `schwung_shim.c:2595`; ring-buffer `malloc` +
WAV `fopen` + `run_command(mkdir)` → **fork+execvp+waitpid** of MoveOriginal
(`shadow_sampler.c:397–631`, `schwung_shim.c:880–892`). Pre-allocate the ring at
init; mkdir from the UI process; route commands through SHM control instead of side
files. Also `pthread_create` for skipback resize at `:2584` and `preview_play()`
open/fstat/mmap from the same path.

### RT-3 (HIGH) `shim_run_command` children inherit SCHED_FIFO 90
`schwung_shim.c:880–892` has no `sched_setscheduler(SCHED_OTHER)` before exec —
the exact class CLAUDE.md says shadow_process.c exists to prevent. Same for raw
`system()` calls at `:6033/:6036` (overtake exit hooks, inside `shim_post_transfer`).
**Fix:** consolidate on the shadow_process.c helper (3 divergent `run_command`
implementations exist: shim no-reset, shadow_ui.c:1081 no-reset, shadow_process.c
correct).

### RT-4 (HIGH) `shadow_poll_current_set()` walks the filesystem on the SPI thread
Called every ~1.5 s from `schwung_shim.c:4863`; implementation
(`shadow_set_pages.c:407–470`) does fopen of Settings.json + opendir + per-entry
getxattr + nested opendir — scales with Set count. Header comment claims "zero file
ops on the audio thread". **Fix:** background poller publishing an atomic snapshot
(the `spi_timing_logger_thread` at `:7717` is the in-repo template).

### RT-5 (HIGH) Flite TTS priority inversion
`shadow_mix_tts()` (RT) → `flite_tts_get_audio` (`tts_engine_flite.c:342`) takes
`ring_mutex` also held by the SCHED_OTHER synthesis thread while copying whole
utterances; disable-edge calls `flite_save_state()` (file I/O) + `unified_log()` on
the RT thread. The eSpeak engine already uses a lock-free volatile ring
(`tts_engine_espeak.c:32–45`) — port Flite to it; check eSpeak's own disable-edge
save too. Related locks on the RT path: sampler ring mutex/cond signal
(`shadow_sampler.c:880`), `tts_speak` synth_mutex via Shift+Menu, `dbus_on_send`
mutex on every process `send()` (`shadow_dbus.c:412`).

### RT-6 (MED) Per-frame / per-event syscalls in the hot path
- `access()` flag-file polls every frame: `spi_snap_trigger` (`:4621`),
  `spi_sysex_inject` (`:4654`), `slot_fx_dump_trigger` (`:7417`), align/main-fx dump
  checks inside `shadow_inprocess_mix_from_buffer` (`:2125–2197`).
- `shadow_midi_out_log_enabled()` calls `access()` on **every call**
  (`shadow_chain_mgmt.c:115` — its sibling at `:106` is 1-in-200 gated), invoked per
  SPI frame from `shadow_inprocess_process_midi`.
- `parse_debug_log` does a `stat()` on **every `v2_set_param`**
  (`chain_host.c:7292→6053`) — every knob tick.
- ~39 `shadow_log`/`unified_log` calls inside the SPI callbacks; `unified_log_v`
  does access/fopen/fprintf/fflush on the calling thread when logging is enabled —
  i.e. enabling `debug_log_on` to chase a glitch *adds* RT file I/O (Heisenbug
  generator).

**Fix (one change):** a single background "debug flags" poller (1 Hz) publishing a
bitmask, plus a lock-free log ring drained by the existing logger thread.

### RT-7 (MED) Misc
- `SHADOW_TIMING_LOG` paths write to `/tmp` on device (`:60`, `:4836`, `:4882`,
  `:5119`, `:7444`) — violates the hard /tmp rule if ever flipped on. Repoint or delete.
- `shadow_fd_trace.c` hooks `read()`/`close()` for the whole MoveOriginal process
  permanently (32-entry linear scan per read; periodic `access()` per close) — and
  `track_fd()` appears to have no caller anymore. Compile-gate or delete.
- dlopen/patch-load runs on the SPI thread by design (accepted load glitch) —
  document the tradeoff in REALTIME_SAFETY.md so it isn't re-investigated.
- `preview_render` streams cold MAP_PRIVATE pages at FIFO 90 — add MAP_POPULATE.
- `init_shadow_shm` ignores `ftruncate` results (full /dev/shm → SIGBUS later, not
  a clean failure); failures printf to stdout instead of `unified_log_crash`.

### Stability (JS side)
- **No try/catch around the tick() draw switch** (`shadow_ui.js:15646–15885`): a
  throw in any draw fn repeats every frame → frozen screen, no recovery. The
  overtake case already has the pattern. Cheap, high-value fix.
- **Blocking work on the UI thread in tick():** `autosaveAllSlots()` does up to 4
  blocking IPC retries per component + sync file writes (`:15438`, `:4313–4326`);
  dirty-cache poll 4 IPC calls/15 ticks; FX name poll up to 12 IPC calls/30 ticks.
  Stagger slots across ticks / batch the polls.
- **Blocking network on the UI thread:** `fetchStoreCatalogSync` (`:6760`) and
  `checkForUpdatesInBackground` (`:1449` — name says background, body is sync).
- Per-MIDI-event template-string allocation before the debugLog enabled-check
  (`:15969`, `:16058`) — constant garbage under aftertouch streams; make debugLog lazy.
- `song-mode/ui.js:221–229` JSON.stringifies the whole song 44×/sec as a dirty
  check — replace with a dirty flag.

---

## 2. Dead code (≈7,000+ deletable lines, all zero-compat-risk)

| Item | Size | Notes |
|------|------|-------|
| **chain_host.c v1 half**: v1 globals, v1 load_*, v1 patch persistence, v1 plugin API impl | ~2,300 lines **+ 7.6 MB BSS** per process (`g_synth_params`/`g_fx_params`/`g_patches`) | Every loader resolves `move_plugin_init_v2` first and chain_host exports it — v1 entry unreachable. External modules' v1 contract untouched. |
| chain_host.c recording subsystem (CC 118) | ~300 lines | Only reachable via dead v1 path; shim skipback/sampler replaced it. Remove fields from `chain_instance_t`; fix CLAUDE.md "Recording" section. |
| chain_host.c JS-MIDI-FX remnants + `chain/midi_fx/*.mjs` | ~100 lines + 3 files | Parsed only by dead v1 path. |
| v1 sub-plugin fallback branches inside live v2 code (`synth_plugin`/`fx_plugins[]`/`source_plugin` always NULL) | scattered | `v2_render_block:8841–8922`, `knob_forward_value:2745`, `inst_send_note_to_synth:7052`. |
| **`src/modules/chain/ui.js`** (standalone chain editor) + `chain/test_patches/` | 2,666 lines | Dead on device (shadow-only); already diverged (regex bug at `:186` — `\\s` in a regex literal; stale `BUILTIN_MIDI_FX` tables; third patch-JSON builder). Decide: delete or mark sim-only. |
| shadow_ui.js dead sweep | ~700 lines | `performCoreUpdate_disabled` (1329–1446), `VIEW_NAMES`, `calcKnobAccel`+constants (superseded by knob_engine), legacy param shims (3791–3841), `syncSettingsFromConfigFile` (6347–6465), `handleOvertakeMenuInput`, `enterStoreFromSettings`, `generateUniquePresetName`, `saveConfigMasterFx`, write-only `lastCC` (which also forces redraw on every CC — `:16021`), unreachable `VIEWS.SLOT_SETTINGS` (~150 lines — **caveat:** `ctx` is a documented fork surface; confirm no fork calls `enterSlotSettings`). |
| shim dead code | ~600 lines | `any_thru_slot_active` (wire it in — bug B1 — don't delete), `biquad_peak/hs`, `shadow_checksum` (dup of shadow_ui's), `within_window`, write-only `wheelTouched`, `log_hotkey_state` scaffolding, `#if 0` SHADOW_AUDIO_REPLACE, `SHADOW_INPROCESS_POC` define (always 1 — the PoC is the product; remove the guards), empty if at `:7111`. |
| `src/shared/move_display.mjs`, `src/shared/chain_param_utils.mjs` | 207 lines | Zero consumers internal + across all sibling module repos. |
| Dead shared exports | — | `createSelectListRenderer`/`calculateVisibleRange` (chain_ui_views), `isPadNote`/`getPadIndex`/`resetEncoderAccel`/`resetAllEncoderAccel` (input_filter), `createDivider`, `getTextEntryBuffer`, `clearManualCache`. Deprecation-comment first (they're published API), remove on a major bump. Keep `dismissOverlayOnInput` (2 external consumers). |
| `store/ui.js` legacy (~40%) | ~400 lines | `removeModule`/`STATE_REMOVING` never invoked; `STATE_UPDATING_HOST` never entered; duplicated catalog fetch vs `store_utils`; unused imports. Slim to a shell over store_utils. |
| `src/host/web_shim.c` + package.sh stanza; `src/host/pfx_track_shm.h` | 137 + header | Not compiled since 0.9.2 / zero includes. Fix FORKING.md:285 reference. |
| Orphaned scripts | 6 files | `analyze_wavs.py`, `migrate-module-params.py`, `test-safety-fixes.sh`, `test-auto-update.sh`, `fix-ssh.sh`, `verify-flite-bundle.sh` — delete or move to `scripts/dev/`. |
| `scripts/lib/move_restart_helpers.sh` + its test | — | Stale fork of install.sh restart logic; the only test of it validates the su+nohup fallback install.sh explicitly abandoned. Re-point or delete both. |
| Shipped-but-never-run artifacts | — | `shadow_poc`, `midi_inject_test` ship by accident (whole-dir packaging); `start.sh`/`stop.sh` ship but launch the standalone host that never runs on device. |
| Root `move-anything.tar.gz` (29 MB, untracked), `SCREEN_READER_TEST.md`/`TESTING_SCREEN_READER.md`, `recon/`, `AGENTS.md` codex path | — | Delete/archive. |

**Explicitly NOT dead — do not delete:**
- `src/schwung_host.c` + `module_manager.c` + `settings.c` + `menu_ui.js`/`menu_*.mjs`:
  standalone host never runs on device, but **Sim A (`sim-a-mac-host` branch) is built
  on it** and the Sim B plan targets it by line range. Gate behind `BUILD_STANDALONE`
  instead (also dodges the reported Docker QuickJS link failure on that target — verify).
- `sound_generator_ui.mjs` (537 lines): zero internal importers but ~12 external
  module repos import it — **compat-critical**; document as stable API so it stops
  looking dead.
- dev/test modules (`controller`, `*-test`): current and referenced by docs; one
  drift — `splash-test`'s tuned animation constants never made it back to
  `menu_ui.js:36–51`.
- `src/patches/linein_freeverb.json`, `src/presets/track_presets/` — live defaults.

---

## 3. Consolidation / duplication

### C-1 (HIGH) 15 JS host bindings duplicated between `shadow_ui.c` and `schwung_host.c`
`run_command`, `validate_path`, file ops, tar extraction, http download, midi send,
etc. — verbatim copies that have **already drifted** (shadow grew `*_background`
variants the host lacks). A security fix to `validate_path`/tar handling in one file
won't reach the other. **Fix:** extract `src/host/js_host_common.c` compiled into
both. Compat: function names unchanged.

### C-2 (HIGH) Display stack duplicated despite `js_display.c` existing for the purpose
`js_display.c` opens with "used by both the main host and shadow UI" but is linked
only into shadow_ui; `schwung_host.c:189–580` carries a parallel copy (own STB
instantiation, own font loader, own packer). Port the host to js_display.c
(~450 lines removed; also de-risks Sim A, which inherits the stale copy).

### C-3 (HIGH) shadow_ui.js: four parallel "typed settings list" engines + duplicated preset-save flow
- Slot settings, chain settings, master-FX settings (which also secretly services
  Global Settings — rename it), LFO editor: each reimplements
  get-value/adjust/announce over `{key,label,type,min,max,step}` (~500 lines).
- Slot-preset vs master-preset save/confirm/overwrite/delete: ~350 near-identical
  lines across parallel variable sets. One parameterized save-modal component.

### C-4 (MED) Five hand-rolled strstr JSON parsers in C
chain_host `json_get_*` + param/hierarchy parsing; shadow_chain_mgmt `mfx_json_*`
(~590-line Master-FX LFO block re-implements chain_host's param-metadata machinery —
only `lfo_common.h` is shared); shadow_chain_mgmt capture extraction;
shadow_state.c strstr read-modify-write (must enumerate every config field or it
**drops them on save** — each new key is a chance to silently lose state).
**Fix:** extract `chain_params_meta.c` shared by chain_host + shadow_chain_mgmt;
consolidate ownership of `shadow_chain_config.json` on one side.

### C-5 (MED) SHM open/map boilerplate ×22
The 12-line shm_open/ftruncate/mmap/memset block repeats 11× in
`init_shadow_shm` and 11× in shadow_ui.c's `open_shadow_shm`. One
`shm_map(name, size, create, zero)` helper ≈ −250 lines, and makes the
required-vs-optional segment policy explicit (today it's accidental ordering).

### C-6 (MED) Shim repetition
Long-press state machines ×4 (track/menu/step2/step13); knob-interception blocks
(overlay vs native overlay) near-clones; encoder-delta decode idiom ×4; MIDI inject
packet boilerplate ×5; 7+ hand-rolled MIDI_IN scan loops with **inconsistent stride**
(see bug B3) — a single `FOREACH_MIDI_IN_EVENT` iterator fixes both.

### C-7 (MED) TTS engines ~70% structurally duplicated
espeak/flite each reimplement ring, enable/disable state machine, config load/save;
dispatch is 28 extern decls + switches. Vtable per backend + one shared front-end
(~−400 lines, and the Flite RT bug becomes structurally impossible). Keep the stub.

### C-8 (MED) schwung-manager built/injected by four code paths
build.sh (build + gunzip/tar-append/gzip), package.sh (conditional include),
install.sh (rebuild + re-inject), release.yml (fourth build + inject). Build it into
`build/` early; let package.sh be the single packaging authority.

### C-9 (LOW-MED) Shared JS layering
- `chain_ui_views.mjs` duplicates every layout constant of `menu_layout.mjs`, and
  generic menu_layout imports `truncateText` *from* chain-specific chain_ui_views
  (inverted layering). Move constants+truncateText into menu_layout, re-export from
  chain_ui_views for the 8 external consumers.
- Two exported `formatParamValue` with incompatible signatures
  (`chain_ui_views.mjs:48` vs `param_format.mjs:64`) — deprecate the former in-place.
- Three encoder-accel implementations (input_filter, knob_engine, chain_ui_views) —
  knob feel differs between menu generations; route menu_nav through knob_engine.
- `wrapText` ×3 (scrollable_text canonical, parse_move_manual private copy,
  sound_generator_ui inline).
- Store catalog logic in store/ui.js duplicates store_utils (port the version-cache
  nicety into store_utils, delete the copy).
- `store_utils.CATEGORIES` includes `midi_source` with no subdir mapping and zero
  catalog entries — always-empty store category.

---

## 4. UI/UX

### U-1 Finish the shadow_ui.js modularization (the architecture already exists)
The `ctx`-object extraction (commit 2f4787ad) is proven — slots/patches modules are
complete — but master_fx/tools/store/settings got **draw-only** extraction; their
input handlers live in three giant switches (`handleJog` ~376 lines, `handleSelect`
~1,026, `handleBack` ~456) plus a draw switch and 70 pure-boilerplate delegate
wrappers. Plan (ordered, lowest risk first):
1. Dead-code sweep (§2).
2. Extract wav-position editor → `shadow_ui_wav.mjs` (~850 lines, self-contained).
3. Extract canvas overlay → `shadow_ui_canvas.mjs` (~400 lines; **module-facing** —
   keep `resolveCanvasScriptPath`/globalThis hook semantics byte-identical).
4. Move input handlers + enter fns next to their draws for master_fx (~700), tools
   (~790), store (~540), settings (~300).
5. Replace the switches with a `HANDLERS[view] = {draw, jog, select, back}` registry
   (kills ~2,000 lines of scaffolding + the delegate wrappers). Keep
   `globalThis.tick/onMidiMessage*` and the overtake/co-run fast paths untouched.
6. Unify save-modal + typed-settings engine (C-3).
7. Promote Help to a real view (today it's flag-guarded sub-state duplicated inside
   MASTER_FX **and** GLOBAL_SETTINGS with a `helpReturnView` hack).
8. Consolidate the ×12 `shadow_config.json` accessors into
   `readShadowConfig()/updateShadowConfig(patch)`; one `scanModules()` helper for
   the ×4 module-scan copies.

### U-2 Footer verb soup (cheap, very visible)
`"Click: edit"` ×4, `"Push: edit"` ×2, `"Jog: Select"` vs `"Jog: scroll"`,
`"Back: Up"`/`"Back: up"`/`"Back: cancel"`/`"Back: Cancel"` — across shadow UI,
store, chain, file-browser ("Push:"), song-mode. Pick one verb + capitalization;
add a `FOOTER_VERBS` constants table to `menu_layout.mjs`.

### U-3 file-browser and song-mode hand-roll the menu system
`file-browser/ui.js:521–577` and `song-mode/ui.js:441–545` reimplement
header/footer/list with different scroll indicators, no marquee, and (file-browser
aside) no screen-reader announcements. Migrating to `drawMenuList` fixes marquee +
a11y + visual consistency in one move. Built-in tools only — zero compat risk.

### U-4 Confirmation-flow inconsistency
Save/delete: inline No/Yes, default No. Analytics prompt: own view, default **Yes**.
Warnings: dismissed by any button. Feedback gate: jog=proceed/Back=abort. Unify on
one Yes/No widget at minimum.

### U-5 Back-button semantics
CHAIN_EDIT/TOOLS/GLOBAL_SETTINGS/MASTER_FX Back always exits shadow mode even when
the user navigated in from the SLOTS list — list navigation never round-trips.
Deliberate for shortcut-jump entry, but worth a decision; `shared/menu_stack.mjs`
exists for exactly this and is unused by the shadow layer (two menu frameworks
coexist: host menus use the Gen-2 stack, shadow grew boolean-flag machines).

### U-6 Help & accessibility content
- `help.json` missing for: store, chain, freeverb, arp, chord, linein, rnbo-runner
  (release checklist treats it as required).
- `shared/help_content.json` stale (last touched Apr 21): no Mute+Jog bypass, no
  slot mute/solo, no Latency Comp, no feedback-protection modal; leftover
  `"title": "Loading..."` placeholder. Add to release checklist by name.
- Screen-reader gaps: store detail / help detail scrolling announce nothing
  per-line; `setView` no longer announces (every `enter*` must remember to).
- Singleton state leaks: `menu_layout` lastAnnounced suppression across different
  menus; `text_scroll` marquee keyed by index only.

---

## 5. Docs / build / hygiene

- **MANUAL.md is deleted but load-bearing**: referenced in CLAUDE.md:7, the docs
  index, and Release Checklist step 4 (plus ARCHITECTURE.md ×2). Point to the
  catalog-site manual (which memory says is canonical: `../schwung-catalog-site/manual.html`).
- **docs/DISPLAY.md** exists only on unmerged `feature/generic-display`; CLAUDE.md
  references it as shipped. Also: CLAUDE.md documents chain Recording (CC 118) which
  is dead in v2 (§2), and "Built-in MIDI FX: chord, arp" understates the real modules.
- **CLAUDE.md claims "No automated suite"** — `tests/` has 85 shell scripts, none in
  CI. Document how to run them; consider a CI job; at least one test validates dead
  code (B1 above in scripts).
- **API.md per-runtime annotation**: many documented `host_*` functions
  (load_module, volume, settings, refresh-rate...) exist only in the standalone
  host — a module written against the docs throws ReferenceError under the only
  runtime that ships. Annotate availability (standalone vs shadow vs both). This IS
  the module contract; docs fix only.
- **Hygiene:** `git rm --cached docs/.DS_Store` + fix `.gitignore` to `**/.DS_Store`;
  drop committed `schwung-manager` binaries (13.6 + 9.5 MB; CI rebuilds them) and the
  redundant quickjs tarball-next-to-extracted-tree; consider fetching the 32 MB
  filebrowser binary at build time; two parallel THIRD_PARTY_LICENSES files drifting;
  archive stale top-level docs (code-review-round-2, release-notes-next,
  link-audio-crackle-followup, GAIN_STAGING_ANALYSIS, move-auth-api,
  LINK_AUDIO_WIRE_FORMAT) into docs/plans/ or docs/archive/.
- Dockerfile sanity check still looks for `modules/sf2/dsp.so` (external module,
  old layout) — prints a misleading "not found" every build.
- package.sh legacy `move-anything` symlinks: add a deprecation date.
- linein.c carries `chain_params`/`ui_hierarchy` as giant C string literals
  duplicating module.json — pick one source of truth.
- module-catalog.json itself: structurally clean (89 entries, all required fields,
  versions consistent). Parent-repo table vs catalog ids worth a one-time
  reconciliation (seqomd, talkbox, wavewarp, jp8000, sh101...).

---

## 6. API headers — deprecation path (no breakage)

| Header | Status | Action |
|--------|--------|--------|
| `plugin_api_v1.h` (v2 half) | canonical | none |
| `plugin_api_v1.h` (v1 half) | no first-party loader instantiates v1 in shadow mode; no external module is v1-only | keep typedefs + module_manager fallback (20 lines); add deprecation banner; log a warning when the fallback fires. **Filename must stay** — every external repo includes it by name. |
| `audio_fx_api_v1.h` | **nothing dlsyms `move_audio_fx_init_v1` anywhere** | deletable after removing chain_host's NULL v1 pointers; externals vendor their own copy |
| `audio_fx_api_v2.h`, `midi_fx_api_v1.h` | live, canonical | none ("v1" in the midi fx name is just unfortunate) |
| `param_helper.h` | zero users in schwung; vendored by ~5 external repos | keep; mark "for module authors" |
| `lfo_common.h` | the one good sharing example | extend the pattern (C-4) |

---

## 7. Suggested execution order (each step = one reviewable PR)

1. **Bug fixes**: B1 (THRU bypass — needs on-hardware MPE test), B2 (patch
   truncation), B5 (doSavePreset feedback), B4 (features.json buffer), B6 (schema
   sync), tick() try/catch.
2. **RT pass 1**: SCHED_OTHER in shim_run_command (RT-3); debug-flags poller thread
   + log ring (RT-6); move set-poller off SPI thread (RT-4).
3. **RT pass 2**: sampler start/stop off the SPI thread (RT-1/2); Flite lock-free
   ring (RT-5); /tmp timing-log repoint (RT-7).
4. **Dead-code sweep A**: chain_host v1 half + recording + JS-MIDI-FX (~2,700 lines,
   7.6 MB BSS); decide chain/ui.js fate; shim dead code; shadow_ui.js dead sweep.
5. **Dead-code sweep B**: shared dead modules/exports (deprecation comments first),
   store/ui.js slim-down, dead scripts, shipping fixes (shadow_poc, midi_inject_test,
   start/stop.sh), web_shim/pfx_track_shm, hygiene (.DS_Store, binaries).
6. **B3 stride unification** (`FOREACH_MIDI_IN_EVENT`) — most load-bearing change;
   thorough pad/knob/filter testing on hardware.
7. **Consolidation**: js_host_common.c (C-1), js_display port (C-2), shm_map helper
   (C-5), chain_params_meta.c (C-4), TTS vtable (C-7), schwung-manager packaging (C-8).
8. **shadow_ui.js decomposition** steps 2–8 of U-1, one view per PR.
9. **UI/UX polish**: footer verbs, file-browser/song-mode onto drawMenuList,
   confirmation unification, help content refresh.
10. **chain_host.c file split** (chain_json/chain_patch/chain_mod/chain_midi/
    chain_params) — pure static-fn moves after step 4 shrinks it to ~6,600 lines.
11. **Docs**: MANUAL.md ghosts, DISPLAY.md, Recording section, test-suite docs,
    API.md runtime annotations, BUILD_STANDALONE gating note.

Rough total: ~7,000 lines deleted, ~2,000 lines deduplicated, ~41 MB → ~? resident
chain memory (1f: heap enum options, drop cached patch state blobs), and the two
files everyone fears (shadow_ui.js 16.4k → ~10k, schwung_shim.c 7.9k → ~2.5k via
the extraction table in the shim report) become navigable.

---

### Appendix: memory-footprint note (chain)
`chain_instance_t` ≈ 10.3 MB (×4 slots ≈ 41 MB resident): `options[128][32]` per
param (4 KB even for non-enums), `patches[32]` carries 16 KB synth + 6×8 KB FX state
inline though only name+path are needed for browsing. Heap-allocate enum options /
string pool; re-read state at load time.

---

## Post-review field note (2026-06-11, during step-1 verification)
While verifying B1 on hardware: after an in-place `restart-move.sh`, shadow_ui
and the SHM segments came back but the shim's SPI pre-transfer path never ran
(the spi_midi_log tap could not arm for 3+ minutes). A full `reboot` recovered.
Worth investigating alongside the RT/stability work: the in-place restart may
leave the SPI hook unengaged in the restarted MoveOriginal process.

## Field note 2 (2026-06-12, after RT pass 2)
Sampler/skipback writes are now fully off the audio thread (SCHED_OTHER
writers, cores 0-2), but a small audible blip remains at save time on
hardware. Likely kernel-level: large eMMC writes trigger writeback that can
stall the SPI ioctl itself — not fixable by scheduling. Candidate
mitigations for a dedicated pass: chunked writes with pacing, fdatasync
throttling, ionice/bfq, O_DIRECT for the skipback writer.
