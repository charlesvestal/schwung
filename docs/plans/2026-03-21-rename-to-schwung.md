# Rename Move Everything → Schwung

**Goal:** Rename all references from "move-anything" / "move-everything" to "schwung" across the codebase, device paths, and installer.

**Why:** Unify branding. The project has three names (move-anything, move-everything, schwung) that refer to the same thing. One name is clearer.

---

## Scope

### File and directory renames
- `move_anything_shim.c` → `schwung_shim.c`
- `move-anything-shim.so` → `schwung-shim.so`
- `move_anything.c` → `schwung_host.c` (or similar)
- Build output names in `scripts/build.sh`

### Device paths
- `/data/UserData/move-anything/` → `/data/UserData/schwung/`
- All hardcoded paths in C and JS:
  - Module directory: `/data/UserData/move-anything/modules/`
  - Patches: `/data/UserData/move-anything/patches/`
  - Config: `/data/UserData/move-anything/shadow_chain_config.json`
  - Slot state: `/data/UserData/move-anything/slot_state/`
  - Debug log: `/data/UserData/move-anything/debug.log`
  - PID files, feature flags, active set, etc.
  - Shadow UI path: `/data/UserData/move-anything/shadow/`

### Shared memory names
- `/move-shadow-display` → `/schwung-display`
- `/move-shadow-control` → `/schwung-control`
- `/move-shadow-ui-midi` → `/schwung-ui-midi`
- All SHM names in `shadow_constants.h`

### Symlink / library paths
- `/usr/lib/move-anything-shim.so` → `/usr/lib/schwung-shim.so`
- Wrapper script references

### Installer
- `move-everything-installer` references
- Tarball names, UI strings, paths

### Internal code references
- Variable names with `move_anything` prefix (optional — internal only)
- Log messages, comments
- CLAUDE.md, README.md, MANUAL.md, docs/

---

## Migration Strategy

### Backwards compatibility (transition period)
- `postinstall.sh` creates symlink: `/data/UserData/move-anything` → `/data/UserData/schwung`
- Modules that hardcode old paths still work via symlink
- SHM names: both old and new names opened (shim tries new first, falls back to old)
- After one release cycle, remove symlink and old SHM fallback

### Module compatibility
- Modules should NOT hardcode `/data/UserData/move-anything/`
- They receive their path from host via `module_config.module_dir` or `module_config.data_dir`
- Audit all modules for hardcoded paths — fix any that have them
- The `module.json` `dsp` field is a relative path (e.g., `braids.so`) — no change needed

### Saved state
- Slot state files, patch files, config files are in the data directory
- Moving the directory moves everything — no file format changes needed
- Symlink handles in-flight transitions

---

## Tasks

### Task 1: Rename source files
- `mv src/move_anything_shim.c src/schwung_shim.c`
- `mv src/move_anything.c src/schwung_host.c`
- Update `scripts/build.sh` for new filenames and output names
- Update Dockerfile if needed

### Task 2: Rename device paths
- Find all `/data/UserData/move-anything` in C and JS files
- Replace with `/data/UserData/schwung`
- Files to check:
  - `src/schwung_shim.c` (was move_anything_shim.c)
  - `src/host/shadow_process.c`
  - `src/host/shadow_chain_mgmt.c`
  - `src/host/shadow_state.c`
  - `src/host/shadow_sampler.c`
  - `src/host/unified_log.c`
  - `src/host/settings.c`
  - `src/shadow/shadow_ui.c`
  - `src/shadow/shadow_ui.js` and all `.mjs` files
  - `scripts/install.sh`, `scripts/build.sh`

### Task 3: Rename SHM segments
- Update `src/host/shadow_constants.h`:
  - `/move-shadow-*` → `/schwung-*`
- Both shim and shadow_ui.c use these constants — single source of truth

### Task 4: Update install/packaging
- `scripts/install.sh` — new paths, new binary names
- Wrapper script — new shim name
- `postinstall.sh` — new paths + backwards compat symlink
- Tarball structure

### Task 5: Update installer app
- `move-everything-installer/` — UI strings, paths, filenames
- This may be a separate PR in that repo

### Task 6: Update docs
- README.md, MANUAL.md, CLAUDE.md
- All docs/ files
- Help content

### Task 7: Audit modules for hardcoded paths
- `grep -rn "move-anything" modules/`
- Fix any that hardcode the old path
- Verify modules use `module_config` paths from host

### Task 8: Test
- Fresh install on clean device
- Upgrade install on device with existing move-anything data (symlink migration)
- All modules load and play
- Shadow UI works
- Patches load from new path
- Saved state persists across rename

---

## What NOT to rename (yet)
- Git repo name on GitHub (can be renamed later without breaking anything)
- Internal variable names like `shadow_control_t` (no user-facing impact)
- The `schwung-spi` library (already correctly named)
