# Integrate JACK Shadow Driver & RNBO Runner into Schwung Core

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bundle the JACK shadow driver (`jack_shadow.so`) and the RNBO Runner overtake module into schwung core, so both are available out-of-the-box without separate installs.

**Architecture:** The JACK shadow driver compiles from 3 source files (already vendored in schwung-jack) into a shared library deployed alongside the host. The RNBO Runner is a pure JS overtake module (no DSP) that gets copied into the built-in modules. A new `requires_path` field in module.json enables runtime visibility gating — the module manager skips modules whose required path doesn't exist on the filesystem.

**Tech Stack:** C (module manager), bash (build script), JavaScript (RNBO Runner UI), ARM64 cross-compilation via Docker

---

## Task 1: Add JACK2 Source Headers to Schwung

The JACK shadow driver (`JackShadowDriver.cpp`) depends on JACK2 internal headers. These need to be vendored into the schwung repo.

**Files:**
- Create: `src/lib/jack2/` — vendored JACK2 headers needed by the shadow driver

**Step 1: Copy required JACK2 headers**

The shadow driver includes these JACK2 headers (from schwung-jack repo):
- `common/jack/*.h` (public JACK API headers)
- `common/JackAudioDriver.h`, `JackThreadedDriver.h`, `JackTime.h`, `JackMidiAsyncQueue.h`
- `common/JackMidiBufferReadQueue.h`, `JackMidiBufferWriteQueue.h`
- `common/JackEngineControl.h`, `JackClientControl.h`, `JackPort.h`
- `common/JackGraphManager.h`, `JackLockedEngine.h`
- `common/JackPosixThread.h`, `JackCompilerDeps.h`, `JackServerGlobals.h`
- `common/jack.h`
- `linux/driver.h`
- `posix/` headers (if referenced transitively)

Copy the necessary subset from `schwung-jack/` into `src/lib/jack2/`:

```bash
mkdir -p src/lib/jack2/common/jack
mkdir -p src/lib/jack2/linux
mkdir -p src/lib/jack2/posix

# Copy common headers (all .h files from common/)
cp ../schwung-jack/common/*.h src/lib/jack2/common/
cp ../schwung-jack/common/jack/*.h src/lib/jack2/common/jack/

# Copy linux driver header
cp ../schwung-jack/linux/driver.h src/lib/jack2/linux/

# Copy posix headers
cp ../schwung-jack/posix/*.h src/lib/jack2/posix/
```

**Step 2: Copy the shadow driver source files**

```bash
mkdir -p src/lib/jack2/shadow
cp ../schwung-jack/linux/shadow/JackShadowDriver.cpp src/lib/jack2/shadow/
cp ../schwung-jack/linux/shadow/JackShadowDriver.h src/lib/jack2/shadow/
```

Note: `schwung_jack_shm.h` already exists at `src/lib/schwung_jack_shm.h` and is identical — no copy needed. The shadow driver's `#include "schwung_jack_shm.h"` will resolve via include path.

**Step 3: Update JackShadowDriver.h include path**

The header currently has `#include "../linux/driver.h"`. Update it to work with the new directory layout:

```cpp
// Change from:
#include "../linux/driver.h"
// To:
#include "../linux/driver.h"  // Same relative path works if jack2/shadow/ → jack2/linux/
```

Verify the relative path `../linux/driver.h` resolves from `src/lib/jack2/shadow/` to `src/lib/jack2/linux/driver.h`. This should work as-is.

**Step 4: Commit**

```bash
git add src/lib/jack2/
git commit -m "vendor: add JACK2 headers for shadow driver build"
```

---

## Task 2: Add JACK Shadow Driver Build to build.sh

**Files:**
- Modify: `scripts/build.sh`

**Step 1: Add build directory creation**

After the existing `mkdir -p` block (around line 136), add:

```bash
mkdir -p ./build/lib/jack
```

**Step 2: Add jack_shadow.so build step**

After the display-server build (around line 535), add:

```bash
# Build JACK shadow driver (loaded by jackd when RNBO/JACK is used)
if needs_rebuild build/lib/jack/jack_shadow.so \
    src/lib/jack2/shadow/JackShadowDriver.cpp \
    src/lib/jack2/shadow/JackShadowDriver.h \
    src/lib/schwung_jack_shm.h; then
    echo "Building JACK shadow driver..."
    "${CROSS_PREFIX}g++" -g -O2 -fPIC -std=c++17 \
        -DSERVER_SIDE \
        -Isrc/lib/jack2 -Isrc/lib/jack2/common -Isrc/lib/jack2/common/jack \
        -Isrc/lib/jack2/linux -Isrc/lib/jack2/shadow -Isrc/lib/jack2/posix \
        -Isrc/lib \
        -c src/lib/jack2/shadow/JackShadowDriver.cpp \
        -o build/jack_shadow_driver.o
    "${CROSS_PREFIX}g++" -shared \
        build/jack_shadow_driver.o \
        -o build/lib/jack/jack_shadow.so \
        -lrt -lpthread
    rm -f build/jack_shadow_driver.o
else
    echo "Skipping JACK shadow driver (up to date)"
fi
```

**Step 3: Add display_ctl build step**

After the jack_shadow.so build, add:

```bash
# Build display_ctl (toggles RNBO display override via shared memory)
if needs_rebuild build/bin/display_ctl \
    src/tools/display_ctl.c src/lib/schwung_jack_shm.h; then
    echo "Building display_ctl..."
    "${CROSS_PREFIX}gcc" -g -O2 \
        src/tools/display_ctl.c \
        -o build/bin/display_ctl \
        -Isrc \
        -lrt
else
    echo "Skipping display_ctl (up to date)"
fi
```

**Step 4: Commit**

```bash
git add scripts/build.sh
git commit -m "build: add JACK shadow driver and display_ctl to build"
```

---

## Task 3: Copy display_ctl Source into Schwung

**Files:**
- Create: `src/tools/display_ctl.c`

**Step 1: Copy display_ctl.c from schwung-rnbo**

```bash
mkdir -p src/tools
cp ../schwung-rnbo/src/tools/display_ctl.c src/tools/
```

**Step 2: Fix the include path**

The file currently has `#include "lib/schwung_jack_shm.h"`. Update to:

```c
#include "schwung_jack_shm.h"
```

Since the build step uses `-Isrc/lib`, this will resolve correctly.

Wait — the build command uses `-Isrc`, so `lib/schwung_jack_shm.h` would also work. Check the original and keep whichever resolves with the `-Isrc` include path. The original `"lib/schwung_jack_shm.h"` works with `-Isrc`. Keep it as-is.

**Step 3: Commit**

```bash
git add src/tools/display_ctl.c
git commit -m "add display_ctl tool for RNBO display override"
```

---

## Task 4: Add RNBO Runner Module to Built-in Modules

**Files:**
- Create: `src/modules/overtake/rnbo-runner/module.json`
- Create: `src/modules/overtake/rnbo-runner/ui.js`
- Create: `src/modules/overtake/rnbo-runner/exit-hook.sh`
- Create: `src/modules/overtake/rnbo-runner/control-startup-shadow-nojack.json`
- Create: `src/modules/overtake/rnbo-runner/restart-control.sh`

**Step 1: Copy rnbo-runner module files**

```bash
mkdir -p src/modules/overtake/rnbo-runner
cp ../schwung-rnbo/src/modules/rnbo-runner/module.json src/modules/overtake/rnbo-runner/
cp ../schwung-rnbo/src/modules/rnbo-runner/ui.js src/modules/overtake/rnbo-runner/
cp ../schwung-rnbo/src/modules/rnbo-runner/exit-hook.sh src/modules/overtake/rnbo-runner/
cp ../schwung-rnbo/src/modules/rnbo-runner/control-startup-shadow-nojack.json src/modules/overtake/rnbo-runner/
cp ../schwung-rnbo/src/modules/rnbo-runner/restart-control.sh src/modules/overtake/rnbo-runner/
chmod +x src/modules/overtake/rnbo-runner/exit-hook.sh
chmod +x src/modules/overtake/rnbo-runner/restart-control.sh
```

**Step 2: Add `requires_path` to module.json**

Edit `src/modules/overtake/rnbo-runner/module.json` to add runtime visibility gating:

```json
{
    "id": "rnbo-runner",
    "name": "RNBO Runner",
    "version": "0.1.0",
    "description": "Run the official RNBO graph runner with full UI",
    "author": "charlesvestal",
    "license": "MIT",
    "ui": "ui.js",
    "api_version": 2,
    "component_type": "overtake",
    "requires_path": "/data/UserData/rnbo/bin/rnbomovecontrol",
    "capabilities": {
        "audio_out": true,
        "midi_out": true
    }
}
```

**Step 3: Update ui.js paths**

The module's `ui.js` references its own install path for the exit hook copy. Since the module will now be built into a different location, update the exit-hook copy path in `ui.js`.

Currently (line 56 and 102):
```javascript
host_system_cmd('cp /data/UserData/schwung/modules/overtake/rnbo-runner/exit-hook.sh ' + HOOKS_DIR + '/overtake-exit.sh');
```

This path is actually correct — on device, built-in modules still live under `/data/UserData/schwung/modules/`. The build copies module files preserving directory structure (`src/modules/overtake/rnbo-runner/` → `build/modules/overtake/rnbo-runner/` → deployed to `schwung/modules/overtake/rnbo-runner/`). So **no path changes needed** in ui.js.

**Step 4: Commit**

```bash
git add src/modules/overtake/rnbo-runner/
git commit -m "feat: bundle RNBO Runner overtake module into core"
```

---

## Task 5: Add `requires_path` Support to Module Manager

**Files:**
- Modify: `src/host/module_manager.h` — add field to `module_info_t`
- Modify: `src/host/module_manager.c` — parse field and check at scan time

**Step 1: Add field to module_info_t**

In `module_manager.h`, add to the `module_info_t` struct (after `scan_packs`):

```c
    /* Runtime visibility: if set, module is only registered when this path exists */
    char requires_path[MAX_PATH_LEN];
```

**Step 2: Parse requires_path in parse_module_json()**

In `module_manager.c`, after the `scan_packs` parsing (line 184), add:

```c
    /* Runtime visibility gate */
    json_get_string(json, "requires_path", info->requires_path, sizeof(info->requires_path));
```

**Step 3: Add visibility check in scan_directory()**

In `scan_directory()`, after the `parse_module_json()` call succeeds (line 363), before the `scan_packs` check, add:

```c
            /* Runtime visibility: skip if requires_path doesn't exist */
            if (parsed->requires_path[0]) {
                struct stat rp_st;
                if (stat(parsed->requires_path, &rp_st) != 0) {
                    printf("mm: skipping '%s' (requires_path not found: %s)\n",
                           parsed->id, parsed->requires_path);
                    memset(parsed, 0, sizeof(*parsed));
                    continue;
                }
            }
```

This goes right after:
```c
        if (parse_module_json(module_path, &mm->modules[mm->module_count]) == 0) {
            module_info_t *parsed = &mm->modules[mm->module_count];
```

And before:
```c
            if (parsed->scan_packs[0]) {
```

**Step 4: Verify the check runs on boot**

`mm_scan_modules()` is called at host startup and on `host_rescan_modules()`. The `stat()` check runs every scan, so if RNBO is installed after boot, a rescan will pick it up. If RNBO is uninstalled, it disappears on next scan. No caching needed.

**Step 5: Commit**

```bash
git add src/host/module_manager.c src/host/module_manager.h
git commit -m "feat: add requires_path for runtime module visibility gating"
```

---

## Task 6: Update Install Script for JACK Driver and display_ctl

**Files:**
- Modify: `scripts/install.sh` — deploy `jack_shadow.so` and `display_ctl` to device

**Step 1: Identify where binaries are deployed**

The install script deploys the schwung tarball. Check how `scripts/package.sh` works to ensure the new files are included.

Look at `scripts/package.sh` — it likely tars up the `build/` directory. Since `jack_shadow.so` is at `build/lib/jack/jack_shadow.so` and `display_ctl` is at `build/bin/display_ctl`, they should be included automatically.

**Step 2: Add RNBO directory setup to install script**

The JACK shadow driver needs to be installed to `/data/UserData/rnbo/lib/jack/` (where JACK looks for drivers). Add a post-install step that symlinks or copies it:

In the install script, after the main deployment, add:

```bash
# Install JACK shadow driver to RNBO lib path (where jackd looks for drivers)
ssh_ableton_with_retry "mkdir -p /data/UserData/rnbo/lib/jack && \
    ln -sf /data/UserData/schwung/lib/jack/jack_shadow.so /data/UserData/rnbo/lib/jack/jack_shadow.so"

# Install display_ctl to RNBO scripts path (used by RNBO Runner module)
ssh_ableton_with_retry "mkdir -p /data/UserData/rnbo/scripts && \
    ln -sf /data/UserData/schwung/bin/display_ctl /data/UserData/rnbo/scripts/display_ctl"
```

Using symlinks means the schwung install is the single source of truth, and the RNBO paths still resolve.

**Step 3: Commit**

```bash
git add scripts/install.sh
git commit -m "deploy: symlink JACK shadow driver and display_ctl to RNBO paths"
```

---

## Task 7: Build and Test

**Step 1: Build**

```bash
cd schwung && ./scripts/build.sh
```

Verify:
- `build/lib/jack/jack_shadow.so` exists and is ARM64
- `build/bin/display_ctl` exists and is ARM64
- `build/modules/overtake/rnbo-runner/module.json` exists and contains `requires_path`

**Step 2: Deploy and test**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

Verify on device:
- **Without RNBO installed**: RNBO Runner should NOT appear in the overtake menu
- **With RNBO installed**: RNBO Runner should appear and launch correctly
- **JACK driver**: `ls /data/UserData/rnbo/lib/jack/jack_shadow.so` — symlink resolves
- **display_ctl**: `ls /data/UserData/rnbo/scripts/display_ctl` — symlink resolves

**Step 3: Commit any fixes and final commit**

```bash
git add -A
git commit -m "feat: integrate JACK shadow driver and RNBO Runner into schwung core"
```

---

## Summary of Changes

| Component | Source | Destination in schwung |
|-----------|--------|----------------------|
| JACK2 headers | `schwung-jack/common/`, `linux/`, `posix/` | `src/lib/jack2/` |
| Shadow driver | `schwung-jack/linux/shadow/` | `src/lib/jack2/shadow/` |
| display_ctl | `schwung-rnbo/src/tools/display_ctl.c` | `src/tools/display_ctl.c` |
| RNBO Runner | `schwung-rnbo/src/modules/rnbo-runner/` | `src/modules/overtake/rnbo-runner/` |
| module_manager | (existing) | Add `requires_path` field + check |
| build.sh | (existing) | Add jack_shadow.so + display_ctl builds |
| install.sh | (existing) | Add symlinks to RNBO paths |

**What stays in separate repos:**
- `schwung-jack` — JACK2 fork (reference, not needed for build)
- `schwung-rnbo` — rnbo-synth + rnbo-fx modules (not ready to publish)
