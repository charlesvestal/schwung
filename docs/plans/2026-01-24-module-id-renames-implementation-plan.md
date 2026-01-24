# Module ID and Display Name Renames Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Rename module IDs and user-facing names for DX7, JV-880, and Space Echo to Dexed, Mini-JV, and TapeDelay across the host and module repos without changing GitHub repo names.

**Architecture:** Module IDs drive install folder names, module store entries, and patch references. The host's `module-catalog.json` controls store listings, while each module repo's `src/module.json` controls UI labels/abbrev and `release.json` controls the download URL. Build/install scripts must produce tarballs and install paths matching the new IDs.

**Tech Stack:** C/C++, JavaScript (MJS/JS), JSON, shell scripts, Markdown.

## Naming Map

- `dexed` → `dexed` (display name: "Dexed", abbrev: "DX")
- `minijv` → `minijv` (display name: "Mini-JV", abbrev: "JV")
- `tapedelay` → `tapedelay` (display name: "TapeDelay", abbrev: "TD")

### Task 1: Create worktrees for module repos

**Files:**
- None

**Step 1: Verify worktree directory is ignored (per repo)**

Run in each repo (`move-anything-dx7`, `move-anything-jv880`, `move-anything-space-delay`):

```bash
git check-ignore -q .worktrees
```

Expected: exit status 0.

**Step 2: Create worktrees**

```bash
git worktree add .worktrees/rename-modules -b rename-modules
```

**Step 3: Confirm clean baseline**

```bash
git status -sb
```

Expected: clean working tree.

### Task 2: Update host module store entries and runtime references

**Files:**
- Modify: `module-catalog.json`
- Modify: `scripts/install.sh`
- Modify: `README.md`
- Modify: `BUILDING.md`
- Modify: `src/modules/chain/README.md`
- Modify: `scripts/build.sh`
- Modify: `examples/shadow_poc.c`
- Modify: `src/shadow/shadow_ui.js`
- Modify: `src/shared/sound_generator_ui.mjs`
- Modify: `THIRD_PARTY_LICENSES`

**Step 1: Baseline check for old IDs (non-docs)**

```bash
rg -n "\\bdx7\\b|\\bjv880\\b|\\bspacecho\\b|DX7|JV-880|Space Echo" --glob '!docs/**'
```

Expected: multiple matches.

**Step 2: Update module catalog entries**

Edit `module-catalog.json`:
- Change IDs to `dexed`, `minijv`, `tapedelay`
- Update `name`, `description`, `requires`, and `asset_name` to new names/tarballs

**Step 3: Update host references and install messaging**

Edit the remaining files to:
- Replace module IDs in paths (`/modules/.../dx7` → `/modules/.../dexed`, etc.)
- Replace display names in text with Dexed / Mini-JV / TapeDelay
- Update any examples to use new module IDs

**Step 4: Re-run grep to confirm**

```bash
rg -n "\\bdx7\\b|\\bjv880\\b|\\bspacecho\\b|DX7|JV-880|Space Echo" --glob '!docs/**'
```

Expected: no matches (excluding docs).

**Step 5: Commit**

```bash
git add module-catalog.json scripts/install.sh README.md BUILDING.md \
  src/modules/chain/README.md scripts/build.sh examples/shadow_poc.c \
  src/shadow/shadow_ui.js src/shared/sound_generator_ui.mjs THIRD_PARTY_LICENSES
git commit -m "chore: rename module IDs and names in host"
```

### Task 3: Rename DX7 module to Dexed

**Files:**
- Modify: `../move-anything-dx7/src/module.json`
- Modify: `../move-anything-dx7/src/ui.js`
- Modify: `../move-anything-dx7/src/dsp/dx7_plugin.cpp`
- Modify: `../move-anything-dx7/scripts/build.sh`
- Modify: `../move-anything-dx7/scripts/install.sh`
- Modify: `../move-anything-dx7/release.json`
- Modify: `../move-anything-dx7/README.md`
- Modify: `../move-anything-dx7/CLAUDE.md`

**Step 1: Baseline check**

```bash
rg -n "\\bDX7\\b|\\bdx7\\b" ../move-anything-dx7
```

Expected: matches in module.json, scripts, README, UI, plugin strings.

**Step 2: Update module metadata**

Edit `../move-anything-dx7/src/module.json`:
- `id`: `dexed`
- `name`: `Dexed`
- `abbrev`: `DX`
- `description`: remove Yamaha/DX7 wording; describe Dexed/MSFA FM synth + .syx support
- `defaults.syx_path`: `/data/UserData/move-anything/modules/sound_generators/dexed/patches.syx`

**Step 3: Update UI and user-facing strings**

Edit `../move-anything-dx7/src/ui.js` and `../move-anything-dx7/src/dsp/dx7_plugin.cpp`:
- Replace "DX7" UI labels and user-visible messages with "Dexed"
- Update any error strings to reference `dexed` paths if present

**Step 4: Update build/install scripts and release URL**

Edit `../move-anything-dx7/scripts/build.sh` and `../move-anything-dx7/scripts/install.sh`:
- `dist/dx7` → `dist/dexed`
- Tarball `dexed-module.tar.gz` → `dexed-module.tar.gz`
- Install path `/modules/sound_generators/dexed` → `/modules/sound_generators/dexed`

Edit `../move-anything-dx7/release.json`:
- `download_url` → `.../dexed-module.tar.gz`

**Step 5: Update README/CLAUDE**

Edit `../move-anything-dx7/README.md` and `../move-anything-dx7/CLAUDE.md`:
- Replace module name and install paths with Dexed/dexed

**Step 6: Verify**

```bash
rg -n "modules/.*/dx7|\\bDX7\\b|\\bdx7\\b" ../move-anything-dx7
```

Expected: no path/label matches; internal file names like `dx7_plugin.cpp` may remain.

**Step 7: Commit**

```bash
git -C ../move-anything-dx7 add src/module.json src/ui.js src/dsp/dx7_plugin.cpp \
  scripts/build.sh scripts/install.sh release.json README.md CLAUDE.md
git -C ../move-anything-dx7 commit -m "chore: rename dx7 module to dexed"
```

### Task 4: Rename JV-880 module to Mini-JV

**Files:**
- Modify: `../move-anything-jv880/src/module.json`
- Modify: `../move-anything-jv880/src/ui.js`
- Modify: `../move-anything-jv880/src/ui_menu.mjs`
- Modify: `../move-anything-jv880/src/ui_browser.mjs`
- Modify: `../move-anything-jv880/src/dsp/jv880_plugin.cpp`
- Modify: `../move-anything-jv880/scripts/build.sh`
- Modify: `../move-anything-jv880/scripts/install.sh`
- Modify: `../move-anything-jv880/release.json`
- Modify: `../move-anything-jv880/README.md`
- Modify: `../move-anything-jv880/CLAUDE.md`

**Step 1: Baseline check**

```bash
rg -n "\\bJV-880\\b|\\bjv880\\b|JV880" ../move-anything-jv880
```

Expected: matches in module.json, scripts, README, UI, plugin strings.

**Step 2: Update module metadata**

Edit `../move-anything-jv880/src/module.json`:
- `id`: `minijv`
- `name`: `Mini-JV`
- `abbrev`: `JV`
- `description`: remove Roland/JV-880 wording; describe Mini-JV rompler + ROM requirement

**Step 3: Update UI and user-facing strings**

Edit `../move-anything-jv880/src/ui.js`, `../move-anything-jv880/src/ui_menu.mjs`, `../move-anything-jv880/src/ui_browser.mjs`,
and `../move-anything-jv880/src/dsp/jv880_plugin.cpp`:
- Replace visible "JV-880" labels with "Mini-JV"
- Update user-facing error text to match new module name

**Step 4: Update build/install scripts and release URL**

Edit `../move-anything-jv880/scripts/build.sh` and `../move-anything-jv880/scripts/install.sh`:
- `dist/jv880` → `dist/minijv`
- Tarball `minijv-module.tar.gz` → `minijv-module.tar.gz`
- Install path `/modules/sound_generators/minijv` → `/modules/sound_generators/minijv`

Edit `../move-anything-jv880/release.json`:
- `download_url` → `.../minijv-module.tar.gz`

**Step 5: Update README/CLAUDE**

Edit `../move-anything-jv880/README.md` and `../move-anything-jv880/CLAUDE.md`:
- Replace module name and install paths with Mini-JV/minijv
- Keep ROM filenames unchanged (they reflect hardware dumps)

**Step 6: Verify**

```bash
rg -n "modules/.*/jv880|\\bJV-880\\b|\\bjv880\\b" ../move-anything-jv880
```

Expected: no path/label matches; internal file names like `jv880_plugin.cpp` may remain.

**Step 7: Commit**

```bash
git -C ../move-anything-jv880 add src/module.json src/ui.js src/ui_menu.mjs src/ui_browser.mjs \
  src/dsp/jv880_plugin.cpp scripts/build.sh scripts/install.sh release.json README.md CLAUDE.md
git -C ../move-anything-jv880 commit -m "chore: rename jv880 module to minijv"
```

### Task 5: Rename Space Echo module to TapeDelay

**Files:**
- Modify: `../move-anything-space-delay/src/module.json`
- Modify: `../move-anything-space-delay/src/dsp/spacecho.c`
- Modify: `../move-anything-space-delay/scripts/build.sh`
- Modify: `../move-anything-space-delay/scripts/install.sh`
- Modify: `../move-anything-space-delay/release.json`
- Modify: `../move-anything-space-delay/README.md`
- Modify: `../move-anything-space-delay/CLAUDE.md`

**Step 1: Baseline check**

```bash
rg -n "\\bspacecho\\b|Space Echo|RE-201" ../move-anything-space-delay
```

Expected: matches in module.json, scripts, README, DSP strings.

**Step 2: Update module metadata**

Edit `../move-anything-space-delay/src/module.json`:
- `id`: `tapedelay`
- `name`: `TapeDelay`
- `abbrev`: `TD`
- `description`: remove RE-201 wording; describe tape delay with flutter/saturation
- `dsp`: rename to `tapedelay.so`
- `ui_hierarchy.levels.root.name`: `TapeDelay`

**Step 3: Update DSP name and log prefix**

Edit `../move-anything-space-delay/src/dsp/spacecho.c`:
- Update user-facing name string to "TapeDelay"
- Update log prefix from `[spacecho]` to `[tapedelay]`

**Step 4: Update build/install scripts and release URL**

Edit `../move-anything-space-delay/scripts/build.sh` and `../move-anything-space-delay/scripts/install.sh`:
- `dist/spacecho` → `dist/tapedelay`
- `spacecho.so` → `tapedelay.so`
- Tarball `tapedelay-module.tar.gz` → `tapedelay-module.tar.gz`
- Install path `/modules/audio_fx/tapedelay` → `/modules/audio_fx/tapedelay`

Edit `../move-anything-space-delay/release.json`:
- `download_url` → `.../tapedelay-module.tar.gz`

**Step 5: Update README/CLAUDE**

Edit `../move-anything-space-delay/README.md` and `../move-anything-space-delay/CLAUDE.md`:
- Replace module name and install paths with TapeDelay/tapedelay

**Step 6: Verify**

```bash
rg -n "modules/.*/spacecho|\\bspacecho\\b|Space Echo" ../move-anything-space-delay
```

Expected: no path/label matches; internal file name `spacecho.c` may remain.

**Step 7: Commit**

```bash
git -C ../move-anything-space-delay add src/module.json src/dsp/spacecho.c scripts/build.sh \
  scripts/install.sh release.json README.md CLAUDE.md
git -C ../move-anything-space-delay commit -m "chore: rename spacecho module to tapedelay"
```

### Task 6: Cross-repo verification

**Files:**
- None

**Step 1: Verify host catalog IDs**

```bash
rg -n "\"id\": \"(dexed|minijv|tapedelay)\"" module-catalog.json
```

Expected: matches for the three updated modules.

**Step 2: Ensure no old module IDs remain in non-docs**

```bash
rg -n "\\bdx7\\b|\\bjv880\\b|\\bspacecho\\b|DX7|JV-880|Space Echo" \
  --glob '!docs/**' --glob '!docs/plans/**' \
  ../move-anything ../move-anything-dx7 ../move-anything-jv880 ../move-anything-space-delay
```

Expected: no matches.

**Step 3: Commit verification notes (optional)**

No commit needed unless you add a verification script.
