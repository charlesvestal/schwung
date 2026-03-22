# Rename Move Everything → Schwung

**Goal:** Rename all references from "move-anything" / "move-everything" to "schwung" across the codebase, device paths, and installer.

**Why:** Unify branding. The project has three names (move-anything, move-everything, schwung) that refer to the same thing. One name is clearer.

---

## Scope

### File and directory renames
- `schwung_shim.c` → `schwung_shim.c`
- `schwung-shim.so` → `schwung-shim.so`
- `schwung_host.c` → `schwung_host.c` (or similar)
- Build output names in `scripts/build.sh`

### Device paths
- `/data/UserData/schwung/` → `/data/UserData/schwung/`
- All hardcoded paths in C and JS (~176 occurrences in C/H files, ~50 in JS files):
  - Module directory: `/data/UserData/schwung/modules/`
  - Patches: `/data/UserData/schwung/patches/`
  - Config: `/data/UserData/schwung/shadow_chain_config.json`
  - Slot state: `/data/UserData/schwung/slot_state/`
  - Debug log: `/data/UserData/schwung/debug.log`
  - Recordings: `/data/UserData/schwung/recordings/`
  - PID files, feature flags, active set, etc.
  - Shadow UI path: `/data/UserData/schwung/shadow/`

#### Path constants are defined in header files (single source of truth for most paths):
- `src/host/unified_log.h` — 2 path defines
- `src/host/settings.h` — 1 path define
- `src/host/shadow_chain_mgmt.h` — 2 path defines
- `src/host/shadow_set_pages.h` — 5 path defines
- `src/host/shadow_state.h` — 1 path define
- `src/host/shadow_sampler.h` — 1 path define
- `src/schwung_host.c` — 3 #define macros + usage string
- `src/modules/chain/dsp/chain_host.c` — 4 path defines

#### Additional C files with hardcoded paths (not via header defines):
- `src/schwung_shim.c` — dozens of path references
- `src/shadow/shadow_ui.c` — many path definitions

#### JS files — all import from `/data/UserData/schwung/shared/`:
- All built-in module UI files (chain, controller, store, file-browser, song-mode, wav-player)
- Shadow UI files (5 files)
- Host menu files
- `src/shared/store_utils.mjs` — GitHub catalog URL

### Shared memory names (14 total)
All defined in `src/host/shadow_constants.h` and `src/host/link_audio.h`:
- `/schwung-audio` → `/schwung-audio`
- `/schwung-midi` → `/schwung-midi`
- `/schwung-ui-midi` → `/schwung-ui-midi`
- `/schwung-display` → `/schwung-display`
- `/schwung-control` → `/schwung-control`
- `/schwung-movein` → `/schwung-movein`
- `/schwung-ui` → `/schwung-ui`
- `/schwung-param` → `/schwung-param`
- `/schwung-midi-out` → `/schwung-midi-out`
- `/schwung-midi-dsp` → `/schwung-midi-dsp`
- `/schwung-midi-inject` → `/schwung-midi-inject`
- `/schwung-screenreader` → `/schwung-screenreader`
- `/schwung-overlay` → `/schwung-overlay`
- `/schwung-pub-audio` → `/schwung-pub-audio`

### Symlink / library paths
- `/usr/lib/schwung-shim.so` → `/usr/lib/schwung-shim.so`
- Wrapper script references

### Installer
- `move-everything-installer` references
- Tarball names, UI strings, paths

### Module catalog and release config
- `module-catalog.json` — 50+ references to `move-everything` repos (host section, asset names, all module entries)
- `release.json` — download URL
- `templates/release-workflow.yml` — Docker image names

### Internal code references
- Variable names with `move_anything` prefix (optional — internal only)
- Log messages, comments
- CLAUDE.md, README.md, MANUAL.md, docs/

### External module repos (35+ repos in parent directory)
Each external module repo has hardcoded `/data/UserData/schwung/` in:
- `scripts/install.sh` (SSH/scp commands)
- `scripts/build.sh`
- `src/ui.js` (import paths)
- `src/dsp/*.c` (some plugin paths)

These need updating but can be done as separate PRs per module.

---

## Migration Strategy

### Backwards compatibility (transition period)
- `postinstall.sh` creates symlink: `/data/UserData/schwung` → `/data/UserData/schwung`
- Modules that hardcode old paths still work via symlink
- After one release cycle, remove symlink

### SHM names — no fallback needed
The shim and host are always installed together by the same install script. There is no scenario where you'd have a version mismatch between them. Just rename the SHM names directly — no dual-open fallback code needed.

### Module compatibility
- Modules should NOT hardcode `/data/UserData/schwung/`
- They receive their path from host via `module_config.module_dir` or `module_config.data_dir`
- Audit all modules for hardcoded paths — fix any that have them
- The `module.json` `dsp` field is a relative path (e.g., `braids.so`) — no change needed
- The filesystem symlink covers any external modules not yet updated

### Saved state
- Slot state files, patch files, config files are in the data directory
- Moving the directory moves everything — no file format changes needed
- Symlink handles in-flight transitions

---

## Tasks

### Task 1: Audit modules for hardcoded paths
Do this first to understand the full scope before committing to changes.
- `grep -rn "move-anything" src/modules/`
- `grep -rn "move-anything" ../move-anything-*/` (external modules)
- Fix any built-in modules that hardcode the old path
- Verify modules use `module_config` paths from host
- External module fixes can be separate PRs

### Task 2: Rename source files
- `mv src/schwung_shim.c src/schwung_shim.c`
- `mv src/schwung_host.c src/schwung_host.c`
- Update `scripts/build.sh` for new filenames and output names
- Update Dockerfile if needed

### Task 3: Rename SHM segments
- Update `src/host/shadow_constants.h`: `/schwung-*` → `/schwung-*`
- Update `src/host/link_audio.h`: `/schwung-pub-audio` → `/schwung-pub-audio`
- Both shim and shadow_ui.c use these constants — single source of truth
- No fallback needed (shim and host always deploy together)

### Task 4: Rename device paths
- Start with header files (these are the source of truth for most paths):
  - `src/host/unified_log.h`
  - `src/host/settings.h`
  - `src/host/shadow_chain_mgmt.h`
  - `src/host/shadow_set_pages.h`
  - `src/host/shadow_state.h`
  - `src/host/shadow_sampler.h`
- Then update remaining hardcoded paths in:
  - `src/schwung_shim.c` (was schwung_shim.c) — dozens of references
  - `src/schwung_host.c` (was schwung_host.c) — 3 #define macros + usage string
  - `src/shadow/shadow_ui.c` — many path definitions
  - `src/modules/chain/dsp/chain_host.c` — 4 path defines
  - All other `src/host/*.c` files
- Update JS import paths in all `.js` and `.mjs` files:
  - All built-in module UIs
  - Shadow UI files
  - Host menu files
  - `src/shared/store_utils.mjs` (catalog URL)

### Task 5: Update install/packaging
- `scripts/install.sh` — new paths, new binary names, GitHub API URLs
- Wrapper script — new shim name
- `postinstall.sh` — new paths + backwards compat symlink for `/data/UserData/schwung`
- Tarball structure and output names
- `release.json` — update download URL

### Task 6: Update module catalog
- `module-catalog.json` — host section (name, asset_name, download_url)
- Note: individual module `github_repo` fields reference repo names that aren't changing yet, so leave those as-is
- `templates/release-workflow.yml` — Docker image names if applicable

### Task 7: Update docs
- CLAUDE.md (extensive — nearly every path and command references move-anything)
- README.md
- MANUAL.md
- All docs/ files
- Help content (help.json in tool modules)
- Parent directory CLAUDE.md

### Task 8: Update external modules (separate PRs)
- Each of the 35+ external module repos needs:
  - `scripts/install.sh` — SSH paths
  - `src/ui.js` — import paths
  - Any hardcoded paths in DSP code
- These can be batched or scripted since the changes are mechanical
- The filesystem symlink covers these during the transition

### Task 9: Update installer app (separate PR)
- `move-everything-installer/` — UI strings, paths, filenames
- Separate PR in that repo

### Task 10: Test
- Fresh install on clean device
- Upgrade install on device with existing move-anything data (symlink migration)
- All built-in modules load and play
- Shadow UI works
- SHM segments connect correctly
- Patches load from new path
- Saved state persists across rename
- External modules still work via symlink (before they're updated)
- Module Store can still fetch and install modules

---

## What NOT to rename (yet)
- Git repo names on GitHub (can be renamed later without breaking anything)
- Internal variable names like `shadow_control_t` (no user-facing impact)
- The `schwung-spi` library (already correctly named)
- External module repo names (`move-anything-braids` etc. — rename later if desired)
- Individual module `github_repo` fields in catalog (they still point to current repo names)
