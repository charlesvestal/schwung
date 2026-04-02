# Standalone Tool Modules Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Allow tool modules to declare themselves as standalone binaries that get exclusive SPI access, with the host managing the kill/relaunch lifecycle automatically.

**Architecture:** A module declares `"standalone": true` in module.json. The host menu detects this and instead of loading JS/DSP, runs a launch script that kills Move, executes the standalone binary, and restarts Move when it exits. The standalone binary opens `/dev/ablspi0.0` directly for SPI access.

**Tech Stack:** C (standalone binary, module manager), JavaScript (menu changes), shell (launcher script)

---

### Task 1: Create the standalone example module directory and module.json

**Files:**
- Create: `src/modules/tools/standalone-example/module.json`

**Step 1: Create module.json**

```json
{
    "id": "standalone-example",
    "name": "Standalone Example",
    "version": "0.1.0",
    "description": "Example standalone tool with direct SPI access",
    "author": "Schwung",
    "component_type": "tool",
    "standalone": true
}
```

**Step 2: Commit**

```bash
git add src/modules/tools/standalone-example/module.json
git commit -m "feat: add standalone-example module.json"
```

---

### Task 2: Write the standalone binary source

**Files:**
- Create: `src/standalone/standalone_example.c`

This is a self-contained C program that:
- Opens `/dev/ablspi0.0` directly via `open()` + `mmap()`
- Runs a main loop driven by `ioctl(fd, SCHWUNG_IOCTL_WAIT_SEND_SIZE, 0x300)`
- Parses MIDI input from offset 2048 (8-byte events: 4-byte USB-MIDI + 4-byte timestamp)
- On pad press (note on, notes 68-99): starts playing a sine wave at a frequency based on the pad note, sets that pad's LED to a color
- On pad release (note off): stops the sine wave, turns off the LED
- Pushes 1-bit display via 6-slice progressive scheme (offset 80-255)
- Shows "Standalone Example" text and current pad state on screen
- Exits on Back button (CC 51, value 127)

**Key constants (from schwung_spi_lib.h):**
- SPI device: `/dev/ablspi0.0`
- Page size: 4096
- Audio out offset: 256 (512 bytes = 128 frames stereo int16)
- MIDI in offset: 2048 (8-byte events, up to 31)
- Display: offset 80 = slice number (1-6), offset 84 = slice data (172 bytes)
- ioctl trigger: `_IOC(_IOC_NONE, 0, 0xa, 0)` with arg `0x300`

**LED control via MIDI OUT (offset 0, 4-byte USB-MIDI packets):**
- Pad LED: `[0x09, 0x90, note, color]` (CIN=9/NoteOn, cable=0, status=0x90)
- Color palette: 0=off, 1=red, 3=orange, 7=yellow, 17=green, 45=blue, 120=white

**Display format:**
- 128x64 pixels, 1-bit packed (128/8 = 16 bytes per row)
- 6 slices: slices 1-5 are 11 rows each (172 bytes = 16*10 + 12), slice 6 is 9 rows (144 bytes but buffer is 172)
- Total: 5*11 + 9 = 64 rows
- Write slice number to offset 80, slice data to offset 84

**Sine wave generation:**
- 44100 Hz sample rate, 128 frames per block
- Phase accumulator, frequency derived from MIDI note: `freq = 440 * pow(2, (note - 69) / 12.0)`
- Mono sine duplicated to stereo, output as int16 interleaved at offset 256
- Simple polyphony: track up to 8 active notes, sum their sine waves

**Step 1: Write standalone_example.c**

The file should include:
- `schwung_spi_lib.h` for constants and types only (no LD_PRELOAD usage)
- Direct `open("/dev/ablspi0.0", O_RDWR)` and `mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)`
- A simple 5x7 bitmap font for text rendering (just uppercase + digits, embedded in source)
- Signal handler for SIGTERM to exit cleanly

**Step 2: Commit**

```bash
git add src/standalone/standalone_example.c
git commit -m "feat: standalone example binary with sine synth and display"
```

---

### Task 3: Parse `standalone` capability in module_manager

**Files:**
- Modify: `src/host/module_manager.h` (add `cap_standalone` field to `module_info_t`)
- Modify: `src/host/module_manager.c:178` (parse `"standalone"` bool from module.json)

**Step 1: Add field to module_info_t**

In `src/host/module_manager.h`, after `cap_raw_ui` (line 34):
```c
int cap_standalone;              /* If true, module is a standalone binary */
```

**Step 2: Parse it in parse_module_json**

In `src/host/module_manager.c`, after line 175 (`json_get_bool(json, "raw_ui", ...)`):
```c
json_get_bool(json, "standalone", &info->cap_standalone);
```

**Step 3: Commit**

```bash
git add src/host/module_manager.h src/host/module_manager.c
git commit -m "feat: parse standalone capability from module.json"
```

---

### Task 4: Expose standalone info to JavaScript menu

**Files:**
- Modify: `src/schwung_host.c:1297-1300` (expose `standalone` and `standalone_path` in `js_host_list_modules`)

**Step 1: Add standalone properties to module list**

After line 1300 (`JS_SetPropertyStr(ctx, obj, "has_ui", ...)`), add:
```c
JS_SetPropertyStr(ctx, obj, "standalone", JS_NewBool(ctx, info->cap_standalone));
if (info->cap_standalone) {
    char standalone_path[MAX_PATH_LEN];
    snprintf(standalone_path, sizeof(standalone_path), "%s/standalone", info->module_dir);
    JS_SetPropertyStr(ctx, obj, "standalone_path", JS_NewString(ctx, standalone_path));
}
```

**Step 2: Commit**

```bash
git add src/schwung_host.c
git commit -m "feat: expose standalone module info to JS menu"
```

---

### Task 5: Create the launch-standalone.sh script

**Files:**
- Create: `src/launch-standalone.sh`

This script follows the same pattern as `src/restart-move.sh`:
- `setsid` to detach from parent process
- Close all inherited file descriptors (3+)
- Kill Move, MoveLauncher, MoveMessageDisplay, schwung, shadow_ui
- Free SPI device via fuser
- Run the standalone binary (blocking)
- When it exits, restart Move

```bash
#!/bin/sh
# Launch a standalone module, then restart Move when it exits.
# Usage: launch-standalone.sh /path/to/standalone/binary
#
# Called via host_system_cmd() from shadow_ui, so this process inherits
# Move's file descriptors. We MUST close them before killing Move.

BINARY="$1"
if [ -z "$BINARY" ] || [ ! -x "$BINARY" ]; then
    echo "launch-standalone: invalid binary: $BINARY" >&2
    exit 1
fi

setsid sh -c '
    BINARY="$1"
    LOG_HELPER=/data/UserData/schwung/unified-log

    log() {
        if [ -x "$LOG_HELPER" ]; then
            "$LOG_HELPER" standalone "$*"
        elif [ -f /data/UserData/schwung/debug_log_on ]; then
            printf "%s\n" "$*" >> /data/UserData/schwung/debug.log
        fi
    }

    # Close ALL inherited file descriptors (3+)
    i=3; while [ $i -lt 1024 ]; do eval "exec ${i}>&-" 2>/dev/null; i=$((i+1)); done

    exec >/dev/null 2>&1
    log "=== launch-standalone.sh started at $(date) ==="
    log "Binary: $BINARY"
    sleep 1

    # Two-phase kill
    for name in MoveMessageDisplay MoveLauncher Move MoveOriginal schwung shadow_ui; do
        pids=$(pidof $name 2>/dev/null || true)
        if [ -n "$pids" ]; then
            log "SIGTERM $name: $pids"
            kill $pids 2>/dev/null || true
        fi
    done
    sleep 0.5

    for name in MoveMessageDisplay MoveLauncher Move MoveOriginal schwung shadow_ui; do
        pids=$(pidof $name 2>/dev/null || true)
        if [ -n "$pids" ]; then
            log "SIGKILL $name: $pids"
            kill -9 $pids 2>/dev/null || true
        fi
    done
    sleep 0.2

    # Free SPI device
    pids=$(fuser /dev/ablspi0.0 2>/dev/null || true)
    if [ -n "$pids" ]; then
        log "Killing SPI holders: $pids"
        kill -9 $pids 2>/dev/null || true
        sleep 0.5
    fi

    # Run standalone binary (blocks until exit)
    log "Launching: $BINARY"
    "$BINARY"
    EXIT_CODE=$?
    log "Standalone exited with code $EXIT_CODE"

    # Restart Move
    log "Restarting Move..."
    sleep 0.5
    if [ -x "$LOG_HELPER" ]; then
        nohup sh -c "/opt/move/Move 2>&1 | /data/UserData/schwung/unified-log move-shim" >/dev/null 2>&1 &
    else
        nohup /opt/move/Move >/dev/null 2>&1 &
    fi
    log "Move restarted with PID $!"
' _ "$BINARY" &
```

**Step 2: Commit**

```bash
git add src/launch-standalone.sh
git commit -m "feat: add launch-standalone.sh for standalone module lifecycle"
```

---

### Task 6: Handle standalone modules in the menu UI

**Files:**
- Modify: `src/host/menu_ui.js:179-215` (update `loadModule` to handle standalone)

**Step 1: Update loadModule function**

Replace the `has_ui` check at line 187-191 with logic that also handles standalone:

```javascript
function loadModule(mod) {
    if (!mod) {
        showStatus("No module selected");
        return;
    }

    /* Standalone modules: launch via script, kills Move */
    if (mod.standalone) {
        console.log(`Launching standalone: ${mod.id} at ${mod.standalone_path}`);
        showStatus(`Launching ${mod.name}...`);
        host_system_cmd("sh /data/UserData/schwung/launch-standalone.sh " + mod.standalone_path);
        return;
    }

    /* Check if module has its own UI */
    if (!mod.has_ui) {
        showStatus("Chain-only module");
        console.log(`Module ${mod.id} has no UI`);
        return;
    }

    // ... rest of existing loadModule unchanged
```

Note: `host_system_cmd` is available in shadow_ui context. For the schwung host menu (`menu_ui.js`), we need to check if it's available there too. If `host_system_cmd` is only in shadow_ui, we need to add it to the host or use an alternative approach.

**Important:** The menu_ui.js runs in the schwung host process (not shadow_ui). Check whether `host_system_cmd` is exposed there. If not, we need to add a `host_launch_standalone(path)` binding to `schwung_host.c` instead.

Looking at the code: `host_system_cmd` is defined in `shadow_ui.c` only. So for `menu_ui.js` (which runs in `schwung_host.c`), we need a new JS binding.

**Step 2: Add host_launch_standalone binding to schwung_host.c**

In `schwung_host.c`, add a new JS function that:
1. Takes a path argument
2. Validates it starts with `/data/UserData/schwung/` and ends with `/standalone`
3. Calls the launch script via fork/exec (same setsid+detach pattern)

```c
static JSValue js_host_launch_standalone(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_FALSE;

    /* Safety: only allow paths under schwung modules dir */
    if (strncmp(path, "/data/UserData/schwung/modules/", 30) != 0) {
        fprintf(stderr, "host_launch_standalone: path not allowed: %s\n", path);
        JS_FreeCString(ctx, path);
        return JS_FALSE;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "/data/UserData/schwung/launch-standalone.sh '%s'", path);

    /* Fork+exec detached — the script handles the rest */
    pid_t pid = fork();
    if (pid == 0) {
        struct sched_param sp = { .sched_priority = 0 };
        sched_setscheduler(0, SCHED_OTHER, &sp);
        execl("/bin/sh", "sh", "/data/UserData/schwung/launch-standalone.sh", path, (char *)NULL);
        _exit(127);
    }

    JS_FreeCString(ctx, path);
    return (pid > 0) ? JS_TRUE : JS_FALSE;
}
```

Register it alongside other host functions in the JS context setup.

Then in `menu_ui.js`, use:
```javascript
host_launch_standalone(mod.standalone_path);
```

**Step 3: Commit**

```bash
git add src/schwung_host.c src/host/menu_ui.js
git commit -m "feat: handle standalone module launch from host menu"
```

---

### Task 7: Add standalone binary to the build

**Files:**
- Modify: `scripts/build.sh` (add compilation step for standalone_example)

**Step 1: Add build step**

After the Shadow POC build block (around line 263), add:

```bash
# Build standalone example tool
if needs_rebuild build/modules/tools/standalone-example/standalone \
    src/standalone/standalone_example.c src/lib/schwung_spi_lib.h; then
    echo "Building standalone example..."
    mkdir -p build/modules/tools/standalone-example
    "${CROSS_PREFIX}gcc" -g -O3 \
        src/standalone/standalone_example.c \
        -o build/modules/tools/standalone-example/standalone \
        -Isrc -Isrc/lib \
        -lm
    cp src/modules/tools/standalone-example/module.json build/modules/tools/standalone-example/
else
    echo "Skipping standalone example (up to date)"
fi
```

**Step 2: Add launch script to package**

In `scripts/package.sh` or the build script's copy section, ensure `launch-standalone.sh` is copied to `build/` and included in the tarball.

**Step 3: Commit**

```bash
git add scripts/build.sh
git commit -m "feat: add standalone example to build system"
```

---

### Task 8: Test on hardware

**Step 1: Build**
```bash
./scripts/build.sh
```

**Step 2: Deploy**
```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Test**
- Open Tools menu on Move
- Select "Standalone Example"
- Verify Move kills and standalone binary takes over
- Verify display shows text
- Press pads — verify LED colors change and sine wave plays
- Press Back button — verify standalone exits and Move restarts
- Verify schwung shadow mode works normally after restart

---

## Notes

### Display rendering in standalone

The display is 128x64, 1-bit, pushed in 6 slices per frame. Each ioctl cycle, set:
- `spi[80]` = slice number (1-6, cycle through one per ioctl)
- `spi[84..255]` = slice pixel data (172 bytes max)

Slice layout:
- Slices 1-5: 11 rows each, 16 bytes/row = 176 bytes (only 172 used, last 4 bytes ignored for slices 1-4; for slice 5 it's 172 bytes = 10.75 rows)
- Slice 6: remaining rows

In practice, simplest approach: maintain a 128x64 framebuffer, pack it to 1-bit, send one slice per ioctl tick (full refresh every 6 ticks = ~136ms at 44.1kHz/128 frames).

### Font

Embed a minimal 5x7 bitmap font in the C source. Only need uppercase, lowercase, digits, and basic punctuation. This keeps the standalone binary fully self-contained with zero external dependencies.

### Audio

Sine wave at pad frequency, 128 frames per tick, stereo int16 at offset 256. Simple additive: sum active voices, clip to int16 range. No envelope — just on/off for the example (or a simple 10ms attack/release to avoid clicks).
