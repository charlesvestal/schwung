# Boot Selector Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `/opt/move/Move` becomes a thin selector that boots a chosen target (Schwung, stock MoveOriginal, or a registered third party), with a 2 s interruptible boot window, a jog picker, and a two-strike watchdog whose terminal fallback is always stock Move.

**Architecture:** The existing `shim-entrypoint.sh` keeps its name and install/heal plumbing but its content becomes the selector: watchdog + target resolution in a sourceable shell lib, an interactive window/picker in a new C binary (`boot-select`, reusing `js_display.c` for drawing), then `exec` of the target's entry script. Today's entrypoint tail (services + LD_PRELOAD exec) moves to `schwung-entry.sh`, the Schwung target.

**Tech Stack:** POSIX shell (BusyBox-compatible), C (cross-compiled `${CROSS_PREFIX}gcc` in Docker), existing `js_display.c` + QuickJS static link (same as shadow_ui), tests/host shell + C harness.

**Spec:** `docs/superpowers/specs/2026-09-05-boot-selector-design.md` (approved 2026-09-05).

**User decisions (already made):**
- Interrupt window over hidden key combo; ~2 s; **Back** is the interrupt button; window shows on **every** boot.
- Back must be a **press edge inside the window** (buttons unreadable until SPI frames are clocked).
- Picker selection **sets the default** — no boot-once mode.
- Watchdog: opt-in `healthy` touch-file first, ~30 s liveness fallback.
- Stock is truly stock: `exec /opt/move/MoveOriginal`, no sidecars, no manager; switching back is reboot+Back.
- Selector ships in Schwung; Schwung's installer is the **sole writer** of `/opt/move/Move`.
- Registry at neutral `/data/UserData/boot-targets/`; `default` is its own file, never a `features.json` key.
- Stock Move is a built-in picker row, not a file.
- Manager feature-gating for V-launched schwung-manager: **separate work item, out of scope here.**

---

## File structure

```
src/host/boot_target_lib.sh      NEW  sourceable: registry resolution + watchdog (no side effects at source time)
src/shim-entrypoint.sh           REWRITE  thin selector (keeps name: heal/manager/post-update paths unchanged)
src/schwung-entry.sh             NEW  Schwung target entry = today's entrypoint tail (services + LD_PRELOAD exec)
src/boot-select.c                NEW  main: SPI open, window, picker, default write
src/host/boot_select_core.c/.h   NEW  pure logic: Back edge detect, jog delta decode, boot.json field scan, row model
src/schwung_shim.c               MODIFY  post SHIM_EVT_BOOT_HEALTHY from first-frame branch
src/host/shim_worker.h/.c        MODIFY  new event + hook
scripts/build.sh                 MODIFY  build boot-select, cp new .sh files
scripts/package.sh               MODIFY  add ./schwung-entry.sh + ./host/boot_target_lib.sh to ITEMS
scripts/install.sh               MODIFY  chmod new scripts (both paths)
scripts/post-update.sh           MODIFY  chmod schwung-entry.sh
scripts/uninstall.sh             MODIFY  remove /data/UserData/boot-targets
tests/host/test_boot_target_resolution.sh   NEW
tests/host/test_boot_watchdog.sh            NEW
tests/host/test_boot_select_core.c          NEW
tests/host/test_boot_healthy_touch.sh       NEW  source-pin: touch goes through shim_worker, not RT path
tests/host/test_manager_pidfile_guard.sh    MODIFY  SRC → src/schwung-entry.sh
tests/host/test_runtime_logs_not_in_root_tmp.sh  MODIFY  add schwung-entry.sh to file list
docs/BOOT_TARGETS.md             MODIFY  design-stage → as-built paths
CLAUDE.md                        MODIFY  one hook bullet
```

Device layout after install:

```
/data/UserData/boot-targets/
  default                        # bare id, e.g. "schwung"
  .boot-attempt                  # "<id> <count>"
  schwung/boot.json              # self-registered by the selector each boot
  schwung/healthy                # touched by the shim after first SPI frame
/data/UserData/schwung/
  shim-entrypoint.sh             # the selector (mirrored to /opt/move/Move by heal — unchanged plumbing)
  schwung-entry.sh               # Schwung target entry
  host/boot_target_lib.sh
  bin/boot-select
```

Key hardware facts (from `docs/SPI_PROTOCOL.md`, `src/lib/schwung_spi_lib.h`):
Back = CC 51, jog turn = CC 14 (relative: 1..63 CW, 65..127 CCW), jog click = CC 3, all on cable 0, status `0xB0`. MIDI_IN at offset 2048, **31 × 8-byte events** (4-byte USB-MIDI + 4-byte timestamp), first all-zero slot terminates. Display: status byte at TX offset 80, data at 84, 6 slices × 172 bytes from the 1024-byte packed buffer. ioctl `SCHWUNG_IOCTL_WAIT_SEND_SIZE` (10) with arg 0x300; speed ioctl 11 with 0x1312d00.

---

### Task 1: Shell library — target resolution + watchdog

**Goal:** `src/host/boot_target_lib.sh`, a sourceable library with all selector decision logic, covered by two host tests. No side effects at source time; every path comes from `BOOT_TARGETS_DIR` so tests point it at a fixture.

**Files:**
- Create: `src/host/boot_target_lib.sh`
- Test: `tests/host/test_boot_target_resolution.sh`, `tests/host/test_boot_watchdog.sh`

**Acceptance Criteria:**
- [ ] `bt_resolve_default` echoes: the id in `$BOOT_TARGETS_DIR/default` when that target's `boot.json` exists; `schwung` when the file is missing/dangling and schwung is registered; `stock` otherwise.
- [ ] `bt_exec_path <id>` echoes the `exec` value from `<id>/boot.json`; echoes nothing (rc 1) for `stock` or a missing/exec-less boot.json.
- [ ] Watchdog: `bt_watchdog_enter <id>` clears the stamp when `<id>/healthy` exists (and removes `healthy`), otherwise increments; echoes the post-increment count. `bt_watchdog_forced <id>` rc 0 iff stamp names `<id>` with count ≥ 2. `bt_watchdog_clear` removes the stamp. `stock` is never stamped (`bt_watchdog_enter stock` echoes 0, writes nothing).
- [ ] Both tests pass; existing `tests/host/*.sh` still pass.

**Verify:** `bash tests/host/test_boot_target_resolution.sh && bash tests/host/test_boot_watchdog.sh` → both exit 0 with per-case PASS lines.

**Steps:**

- [ ] **Step 1: Write the library**

```sh
#!/bin/sh
# boot_target_lib.sh — selector decision logic. Sourced by shim-entrypoint.sh
# and by tests/host/test_boot_*.sh. BusyBox sh compatible: no arrays, no
# [[ ]], no local outside functions. Every path derives from
# BOOT_TARGETS_DIR so tests can aim it at a fixture directory.

BOOT_TARGETS_DIR="${BOOT_TARGETS_DIR:-/data/UserData/boot-targets}"

# Extract a string field from a boot.json. Deliberately dumb: first
# occurrence of "key" : "value" on any line. BOOT_TARGETS.md documents that
# boot.json must be flat JSON with one field per line.
bt_json_field() { # $1=file $2=key
    [ -f "$1" ] || return 1
    sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | head -n 1
}

bt_is_registered() { # $1=id
    [ -f "$BOOT_TARGETS_DIR/$1/boot.json" ]
}

bt_resolve_default() {
    d=""
    [ -f "$BOOT_TARGETS_DIR/default" ] && d=$(head -n 1 "$BOOT_TARGETS_DIR/default" | tr -d '[:space:]')
    if [ -n "$d" ] && { [ "$d" = "stock" ] || bt_is_registered "$d"; }; then
        echo "$d"; return 0
    fi
    if bt_is_registered schwung; then echo schwung; else echo stock; fi
}

bt_exec_path() { # $1=id -> exec path on stdout, rc 1 if none
    [ "$1" = "stock" ] && return 1
    p=$(bt_json_field "$BOOT_TARGETS_DIR/$1/boot.json" exec)
    [ -n "$p" ] || return 1
    echo "$p"
}

# ── Watchdog ──────────────────────────────────────────────────────────────
# Stamp file: "<id> <count>". Protocol per boot:
#   count=$(bt_watchdog_enter "$id")   # healthy-file clear, then increment
#   bt_watchdog_forced "$id" && ...    # >=2 un-cleared attempts -> picker
# Cleared by: the target's healthy touch-file (seen at the NEXT entry), or
# the 30 s liveness watcher calling bt_watchdog_clear.

bt_stamp_file() { echo "$BOOT_TARGETS_DIR/.boot-attempt"; }

bt_watchdog_clear() { rm -f "$(bt_stamp_file)"; }

bt_watchdog_enter() { # $1=id -> echoes post-increment count
    id="$1"
    if [ "$id" = "stock" ]; then echo 0; return 0; fi
    if [ -f "$BOOT_TARGETS_DIR/$id/healthy" ]; then
        rm -f "$BOOT_TARGETS_DIR/$id/healthy"
        bt_watchdog_clear
    fi
    n=0
    if [ -f "$(bt_stamp_file)" ]; then
        # Stamp is "<id> <count>". `set --` clobbers positionals, so id was
        # captured above before this line.
        set -- $(cat "$(bt_stamp_file)")
        prev_id="${1:-}"; prev_n="${2:-0}"
        # A different target resets the count: strikes are per-target.
        [ "$prev_id" = "$id" ] && n="$prev_n"
    fi
    n=$((n + 1))
    printf '%s %s\n' "$id" "$n" > "$(bt_stamp_file)"
    echo "$n"
}

bt_watchdog_forced() { # $1=id -> rc 0 if this target has >=2 uncleared attempts
    want="$1"
    [ -f "$(bt_stamp_file)" ] || return 1
    set -- $(cat "$(bt_stamp_file)")
    [ "${1:-}" = "$want" ] && [ "${2:-0}" -ge 2 ]
}
```

- [ ] **Step 2: Write the resolution test**

`tests/host/test_boot_target_resolution.sh` — follow the house pattern (`set -uo pipefail`, `cd "$(dirname "$0")/../.."`, `fails` counter, no `grep|head` pipes):

```sh
#!/usr/bin/env bash
# The selector's target resolution, run against fixtures on the host.
set -uo pipefail
cd "$(dirname "$0")/../.."
fails=0
fail() { echo "FAIL: $1"; fails=$((fails+1)); }
pass() { echo "PASS: $1"; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export BOOT_TARGETS_DIR="$tmp/boot-targets"
mkdir -p "$BOOT_TARGETS_DIR/schwung" "$BOOT_TARGETS_DIR/v"
printf '{\n  "name": "Schwung",\n  "exec": "/data/UserData/schwung/schwung-entry.sh"\n}\n' \
    > "$BOOT_TARGETS_DIR/schwung/boot.json"
printf '{\n  "name": "V",\n  "exec": "/data/UserData/boot-targets/v/entry.sh"\n}\n' \
    > "$BOOT_TARGETS_DIR/v/boot.json"

. src/host/boot_target_lib.sh

[ "$(bt_resolve_default)" = "schwung" ] && pass "no default file -> schwung" || fail "no default file"
echo v > "$BOOT_TARGETS_DIR/default"
[ "$(bt_resolve_default)" = "v" ] && pass "default honored" || fail "default honored"
echo ghost > "$BOOT_TARGETS_DIR/default"
[ "$(bt_resolve_default)" = "schwung" ] && pass "dangling default -> schwung" || fail "dangling default"
rm -rf "$BOOT_TARGETS_DIR/schwung"
[ "$(bt_resolve_default)" = "stock" ] && pass "no schwung -> stock" || fail "no schwung -> stock"
echo stock > "$BOOT_TARGETS_DIR/default"
[ "$(bt_resolve_default)" = "stock" ] && pass "explicit stock" || fail "explicit stock"

[ "$(bt_exec_path v)" = "/data/UserData/boot-targets/v/entry.sh" ] && pass "exec path" || fail "exec path"
bt_exec_path stock >/dev/null && fail "stock has no exec path" || pass "stock has no exec path"
bt_exec_path ghost >/dev/null && fail "missing target rc" || pass "missing target rc"

exit $((fails > 0))
```

- [ ] **Step 3: Write the watchdog test**

`tests/host/test_boot_watchdog.sh`, same skeleton and fixture setup (schwung registered), then:

```sh
[ "$(bt_watchdog_enter schwung)" = "1" ] && pass "first attempt = 1" || fail "first attempt"
bt_watchdog_forced schwung && fail "not forced at 1" || pass "not forced at 1"
[ "$(bt_watchdog_enter schwung)" = "2" ] && pass "second attempt = 2" || fail "second attempt"
bt_watchdog_forced schwung && pass "forced at 2" || fail "forced at 2"

# healthy file clears the stamp at the next entry
touch "$BOOT_TARGETS_DIR/schwung/healthy"
[ "$(bt_watchdog_enter schwung)" = "1" ] && pass "healthy resets to 1" || fail "healthy resets"
[ ! -f "$BOOT_TARGETS_DIR/schwung/healthy" ] && pass "healthy consumed" || fail "healthy consumed"

# a different target resets the strike count
bt_watchdog_clear
bt_watchdog_enter schwung >/dev/null
[ "$(bt_watchdog_enter v)" = "1" ] && pass "target switch resets count" || fail "target switch"

# stock is never stamped
bt_watchdog_clear
[ "$(bt_watchdog_enter stock)" = "0" ] && pass "stock not stamped" || fail "stock stamped"
[ ! -f "$BOOT_TARGETS_DIR/.boot-attempt" ] && pass "no stamp file for stock" || fail "stamp for stock"
```

- [ ] **Step 4: Run, fix, run again**

Run: `bash tests/host/test_boot_target_resolution.sh && bash tests/host/test_boot_watchdog.sh`
Expected: all PASS, exit 0. (First run will catch the `set --` clobber noted in Step 1.)

- [ ] **Step 5: Commit**

```bash
git add src/host/boot_target_lib.sh tests/host/test_boot_target_resolution.sh tests/host/test_boot_watchdog.sh
git commit -m "feat: boot target resolution + watchdog shell library"
```

---

### Task 2: Entrypoint split

**Goal:** `shim-entrypoint.sh` becomes the thin selector; today's tail moves verbatim to `schwung-entry.sh`. All heal/manager/install path references stay valid because the filename doesn't change.

**Files:**
- Create: `src/schwung-entry.sh`
- Modify: `src/shim-entrypoint.sh` (rewrite), `scripts/build.sh:848` (cp new files), `scripts/package.sh:17` (ITEMS)
- Test: modify `tests/host/test_manager_pidfile_guard.sh:24` (`SRC="src/schwung-entry.sh"`), `tests/host/test_runtime_logs_not_in_root_tmp.sh:12` (add `src/schwung-entry.sh`)

**Acceptance Criteria:**
- [ ] `src/schwung-entry.sh` contains, unchanged in behavior: the LD_LIBRARY_PATH export, display-server launch, schwung-manager launch (with the pid-file cmdline guard intact), filebrowser launch, and the final `exec env LD_PRELOAD=schwung-shim.so /opt/move/MoveOriginal`.
- [ ] `src/shim-entrypoint.sh` contains, in order: migration block (unchanged), factory-reset safety net (unchanged), `/usr/lib` shim symlink fix + stale-entrypoint blocks (unchanged), schwung-heal (unchanged), then the new selector flow below. No service launches remain in it.
- [ ] The selector self-registers `boot-targets/schwung/boot.json` every boot (Schwung-owned, self-healing registration; `install.sh` needn't create it).
- [ ] Every fallback path terminates in `exec /opt/move/MoveOriginal`.
- [ ] The liveness watcher backgrounds *before* the exec and clears the stamp iff the pid (preserved across `exec`) is alive after 30 s.
- [ ] `bash tests/host/test_manager_pidfile_guard.sh` and `bash tests/host/test_runtime_logs_not_in_root_tmp.sh` pass against the new layout.

**Verify:** `bash tests/host/test_manager_pidfile_guard.sh && bash tests/host/test_runtime_logs_not_in_root_tmp.sh && sh -n src/shim-entrypoint.sh && sh -n src/schwung-entry.sh` → exit 0.

**Steps:**

- [ ] **Step 1: Create `src/schwung-entry.sh`**

Move everything from the current `src/shim-entrypoint.sh` **after** the heal block (line 78, `export LD_LIBRARY_PATH...`) through the final `exec` into the new file, prefixed with:

```sh
#!/bin/sh
# schwung-entry.sh — the Schwung boot target. Exec'd by the selector
# (/opt/move/Move, i.e. shim-entrypoint.sh). Launches Schwung's sidecar
# services, then execs MoveOriginal under the shim. Registered at
# /data/UserData/boot-targets/schwung/boot.json.
SCHWUNG_DIR="/data/UserData/schwung"
```

Delete the stale "link-subscriber is launched by the shim" comment while moving (the shim doesn't launch it; keep the line out rather than propagating the error).

- [ ] **Step 2: Rewrite the tail of `src/shim-entrypoint.sh`**

After the existing heal block, replace everything with:

```sh
# ── Boot selector ─────────────────────────────────────────────────────────
# Resolve a target, offer the 2 s Back window, watchdog-count the attempt,
# then exec the target's entry script. Every failure path lands on stock
# MoveOriginal: the selector's own failure mode is always "Move boots".
BT_LIB="$SCHWUNG_DIR/host/boot_target_lib.sh"
BOOT_SELECT="$SCHWUNG_DIR/bin/boot-select"

if [ ! -f "$BT_LIB" ]; then
    exec env LD_PRELOAD=schwung-shim.so /opt/move/MoveOriginal
fi
. "$BT_LIB"

# Self-registration: Schwung owns its own registry entry, refreshed every
# boot so a wiped or hand-edited registry heals itself.
mkdir -p "$BOOT_TARGETS_DIR/schwung"
printf '{\n  "name": "Schwung",\n  "exec": "%s"\n}\n' \
    "$SCHWUNG_DIR/schwung-entry.sh" > "$BOOT_TARGETS_DIR/schwung/boot.json"

target=$(bt_resolve_default)
attempt=$(bt_watchdog_enter "$target")

# boot-select: window + picker. Prints the chosen id on stdout; a selection
# rewrites $BOOT_TARGETS_DIR/default itself. Missing or failing binary ->
# silent fallthrough to the resolved default.
if [ -x "$BOOT_SELECT" ]; then
    if bt_watchdog_forced "$target"; then
        chosen=$("$BOOT_SELECT" --forced "$target" 2>/dev/null) || chosen=""
    else
        chosen=$("$BOOT_SELECT" --window "$target" 2>/dev/null) || chosen=""
    fi
    if [ -n "$chosen" ] && [ "$chosen" != "$target" ]; then
        target="$chosen"
        attempt=$(bt_watchdog_enter "$target")
    fi
fi

if [ "$target" = "stock" ]; then
    bt_watchdog_clear
    exec /opt/move/MoveOriginal
fi

entry=$(bt_exec_path "$target") || entry=""
if [ -z "$entry" ] || [ ! -x "$entry" ]; then
    exec /opt/move/MoveOriginal
fi

# Liveness watcher: exec preserves the pid, so checking our own pid after
# 30 s checks the target. A target that forks-and-exits is not covered —
# that's what the healthy touch-file is for (see docs/BOOT_TARGETS.md).
self=$$
(
    sleep 30
    if kill -0 "$self" 2>/dev/null; then
        BOOT_TARGETS_DIR="$BOOT_TARGETS_DIR" . "$BT_LIB" && bt_watchdog_clear
    fi
) &

exec "$entry"
```

Note `BOOT_TARGETS_DIR` comes from the lib's default; the watcher subshell re-sources the lib because the environment (not shell functions) survives into `( … ) &` only until the parent execs — functions defined pre-fork DO persist in the subshell, so the re-source is belt-and-braces; keep it, it also survives future refactors that move the watcher.

- [ ] **Step 3: Ship both files**

`scripts/build.sh` line 848 area: after `cp ./src/shim-entrypoint.sh ./build/`, add:

```sh
cp ./src/schwung-entry.sh ./build/
mkdir -p ./build/host && cp ./src/host/boot_target_lib.sh ./build/host/
```

(`./host` is already in package ITEMS if present — check `scripts/package.sh:17`; add `./schwung-entry.sh` to ITEMS, and `./host/boot_target_lib.sh` rides along if `./host` is an ITEMS entry, else add it explicitly.)

- [ ] **Step 4: Repoint the two tests**

`tests/host/test_manager_pidfile_guard.sh:24`: `SRC="src/schwung-entry.sh"`. `tests/host/test_runtime_logs_not_in_root_tmp.sh:12`: add `src/schwung-entry.sh` to the scanned list (keep `src/shim-entrypoint.sh` — it's still shipped).

- [ ] **Step 5: Verify + commit**

Run: `bash tests/host/test_manager_pidfile_guard.sh && bash tests/host/test_runtime_logs_not_in_root_tmp.sh && sh -n src/shim-entrypoint.sh && sh -n src/schwung-entry.sh`
Expected: exit 0.

```bash
git add src/shim-entrypoint.sh src/schwung-entry.sh scripts/build.sh scripts/package.sh tests/host/test_manager_pidfile_guard.sh tests/host/test_runtime_logs_not_in_root_tmp.sh
git commit -m "feat: split entrypoint into boot selector + schwung target entry"
```

---

### Task 3: boot-select core logic (host-testable C)

**Goal:** `src/host/boot_select_core.c/.h` — the pure-logic half of the picker: MIDI_IN event walk with Back edge detection, jog delta decoding, boot.json field extraction, and the row model (registered targets + built-in Stock). Unit-tested on the host.

**Files:**
- Create: `src/host/boot_select_core.h`, `src/host/boot_select_core.c`
- Test: `tests/host/test_boot_select_core.c`; modify `tests/host/Makefile` (add to `TARGETS`, add an explicit rule linking the core .c)

**Acceptance Criteria:**
- [ ] `bs_input_scan` walks MIDI_IN at **stride 8**, stops at the first all-zero-header slot, considers **cable 0 only**, and reports: Back press edge (CC 51, d2>0 after a previously-seen release or ≥1 clean frame), jog click press edge (CC 3), and accumulated jog delta (CC 14: 1..63 → +v, 65..127 → v−128).
- [ ] Edge detection is stateful across frames via a caller-owned `bs_input_state_t`; the **first frame never fires** an edge (stale mailbox junk is ignored).
- [ ] `bs_json_field(buf, "exec", out, outlen)` extracts flat-JSON string fields; tolerates whitespace; returns 0 on absence.
- [ ] `bs_build_rows(dir, rows, max)` fills rows from `<dir>/*/boot.json` (id + name), sorts `schwung` first then alphabetical, appends the built-in `{id:"stock", name:"Stock Move"}` last; never overflows `max`.
- [ ] `make -C tests/host test` passes.

**Verify:** `make -C tests/host test` → all targets build and run green, including `test_boot_select_core`.

**Steps:**

- [ ] **Step 1: Header**

```c
// boot_select_core.h — pure logic for boot-select, unit-tested on the host.
#ifndef BOOT_SELECT_CORE_H
#define BOOT_SELECT_CORE_H
#include <stdint.h>
#include <stddef.h>

#define BS_MIDI_IN_BYTES (31 * 8)   // 31 events x 8 bytes; NEVER 256

typedef struct {
    int frames_seen;     // 0 until first scan completes; edges suppressed at 0
    int back_down;       // last observed Back state
    int click_down;      // last observed jog-click state
} bs_input_state_t;

typedef struct {
    int back_pressed;    // press edge this frame
    int click_pressed;   // press edge this frame
    int jog_delta;       // summed signed detents this frame
} bs_input_events_t;

// src points at the 248-byte MIDI_IN region (offset 2048 in the RX page).
void bs_input_scan(bs_input_state_t *st, const uint8_t *src,
                   bs_input_events_t *out);

int bs_json_field(const char *buf, const char *key, char *out, size_t outlen);

typedef struct { char id[64]; char name[64]; } bs_row_t;
// Returns row count. Last row is always the built-in stock entry.
int bs_build_rows(const char *dir, bs_row_t *rows, int max);

#endif
```

- [ ] **Step 2: Implementation**

`bs_input_scan` core loop (the part that must be exactly right):

```c
void bs_input_scan(bs_input_state_t *st, const uint8_t *src,
                   bs_input_events_t *out)
{
    memset(out, 0, sizeof(*out));
    int back_now = st->back_down, click_now = st->click_down;
    for (int i = 0; i < BS_MIDI_IN_BYTES; i += 8) {
        const uint8_t *e = src + i;
        if (!e[0] && !e[1] && !e[2] && !e[3]) break;  // terminator slot
        int cable = e[0] >> 4;
        if (cable != 0) continue;                      // Move hw only
        if ((e[1] & 0xF0) != 0xB0) continue;           // CC only
        uint8_t cc = e[2], val = e[3];
        if (cc == 51) {
            if (val > 0 && !back_now && st->frames_seen > 0) out->back_pressed = 1;
            back_now = val > 0;
        } else if (cc == 3) {
            if (val > 0 && !click_now && st->frames_seen > 0) out->click_pressed = 1;
            click_now = val > 0;
        } else if (cc == 14 && st->frames_seen > 0) {
            out->jog_delta += (val < 64) ? (int)val : (int)val - 128;
        }
    }
    st->back_down = back_now;
    st->click_down = click_now;
    st->frames_seen++;
}
```

`bs_json_field`: `strstr` for `"key"`, skip to `:`, skip whitespace, require `"`, copy until closing `"` with bounds check. `bs_build_rows`: `opendir`/`readdir`, for each subdir read `boot.json` (bounded `fread` into a 4 KB buffer), extract `name` (fall back to the dir name), copy id from dir name; skip an entry named `stock` or `schwung`-duplicates; simple insertion keeping `schwung` at index 0; append stock row last.

- [ ] **Step 3: Test**

`tests/host/test_boot_select_core.c` — follow the house C-test style (assert macro counting failures, `main` returns fail count). Cases, each building a synthetic 248-byte buffer:

```c
// helper: put a CC event in slot n
static void put_cc(uint8_t *buf, int slot, uint8_t cc, uint8_t val) {
    uint8_t *e = buf + slot * 8;
    e[0] = 0x0B; e[1] = 0xB0; e[2] = cc; e[3] = val;   // cable 0, CIN B
    e[4] = 1;                                           // nonzero timestamp
}
```

1. Back press in frame 0 → **no** edge (frames_seen guard). Same event in frame 1 after a clean frame → edge fires.
2. Back held across frames → exactly one edge; release then press → second edge.
3. Event behind a terminator slot is invisible (fill slot 0 with zeros, slot 1 with Back press → no edge ever).
4. Cable-2 copy of CC 51 → ignored (`e[0] = 0x2B`).
5. Jog: val 1 ×3 events → delta +3; val 127 → −1; mixed sums.
6. A buffer with all 31 slots full of jog events → walk stays inside 248 bytes (no ASAN needed; assert delta == 31·1).
7. `bs_json_field`: extracts `exec` from the Task 1 fixture JSON shape; absent key → 0; value longer than `outlen` → truncated + NUL-terminated.
8. `bs_build_rows` against a mktemp fixture dir (create `schwung/`, `v/` with boot.jsons, plus a junk file): rows = schwung, v, stock; count 3.

`tests/host/Makefile`: add `test_boot_select_core` to `TARGETS` (~line 56) and an explicit rule (pattern of `test_shadow_midi_filter`, line 92):

```make
$(BUILD_DIR)/test_boot_select_core: test_boot_select_core.c ../../src/host/boot_select_core.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ $(LDFLAGS)
```

- [ ] **Step 4: Run + commit**

Run: `make -C tests/host test`
Expected: all green including `test_boot_select_core`.

```bash
git add src/host/boot_select_core.h src/host/boot_select_core.c tests/host/test_boot_select_core.c tests/host/Makefile
git commit -m "feat: boot-select core logic with host unit tests"
```

---

### Task 4: boot-select binary (SPI window + picker)

**Goal:** `src/boot-select.c` — opens the SPI device, paints the window/picker via `js_display.c`, drives frames, writes `default` on selection, prints the chosen id. Built into `build/bin/boot-select` (rides into the tarball via the existing `./bin` packaging — **do not** add it to package.sh's line-24 scrub list).

**Files:**
- Create: `src/boot-select.c`
- Modify: `scripts/build.sh` (new build block after schwung-heal's, ~line 836)

**Acceptance Criteria:**
- [ ] `boot-select --window <id>`: paints `Loading <name>` / `press Back to change`, runs ~2 s of frames; no Back edge → prints `<id>`, exit 0. Back edge → picker.
- [ ] `boot-select --forced <id>`: skips the window, opens the picker with banner `<name> failed to start`, cursor on the stock row.
- [ ] Picker: jog scrolls (delta accumulates, one row per detent), jog-click selects → writes the id to `$BOOT_TARGETS_DIR/default` (atomic: write `.default.tmp`, `rename`) and prints it; Back in the picker cancels → prints the incoming default id.
- [ ] SPI fd is closed and the mmap unmapped before exit (the target must find the device free).
- [ ] Any failure (device open, mmap, registry read) → exit 1 with nothing on stdout; stderr only. Total runtime hard-capped at 60 s (alarm) so a wedged picker can't hold boot forever — on SIGALRM print the incoming default and exit 0.
- [ ] `./scripts/build.sh` produces `build/bin/boot-select` (ARM64); CI `cross-compile` job stays green.

**Verify:** `./scripts/build.sh` → `file build/bin/boot-select` reports ARM aarch64; `make -C tests/host test` still green.

**Steps:**

- [ ] **Step 1: Main structure**

```c
// boot-select — the boot window + target picker. Runs before anything owns
// /dev/ablspi0.0. NOT realtime: plain SCHED_OTHER, may fopen freely.
// stdout carries exactly one line: the chosen target id. All diagnostics
// to stderr. Exit 1 = caller falls through to its resolved default.
#include "lib/schwung_spi_lib.h"     // constants only, not the interposer
#include "host/js_display.h"
#include "host/boot_select_core.h"
```

Flow, all in `main`:
1. Parse `--window <id>` | `--forced <id>`; `BOOT_TARGETS_DIR` env override honored (default `/data/UserData/boot-targets`), `alarm(60)` + SIGALRM handler that prints the default id and `_exit(0)`.
2. `bs_build_rows()`; find the incoming id's row for its display name.
3. `open(SCHWUNG_SPI_DEVICE, O_RDWR)`, `mmap(NULL, SCHWUNG_PAGE_SIZE, ...)`, `memset(map, 0, SCHWUNG_PAGE_SIZE)`, speed ioctl (`SCHWUNG_IOCTL_SET_SPEED`, `0x1312d00`) — the exact sequence from `schwung_host.c:2216-2371`.
4. Frame loop helper:

```c
static void frame(int fd) {                    // one 768-byte SPI transfer
    ioctl(fd, _IOC(_IOC_NONE, 0, SCHWUNG_IOCTL_WAIT_SEND_SIZE, 0), 0x300);
}
static void flush_display(int fd, uint8_t *map) {
    static uint8_t packed[SCHWUNG_DISPLAY_SIZE];
    js_display_pack(packed);
    for (int s = 0; s < 6; s++) {
        int len = (s == 5) ? 164 : SCHWUNG_OUT_DISP_CHUNK_LEN;
        map[SCHWUNG_OFF_OUT_DISP_STAT] = (uint8_t)(s + 1);
        memcpy(map + SCHWUNG_OFF_OUT_DISP_DATA, packed + s * SCHWUNG_OUT_DISP_CHUNK_LEN, len);
        frame(fd);
        struct timespec ts = {0, 3000000}; nanosleep(&ts, NULL);  // 3 ms, as js_host_flush_display
    }
    map[SCHWUNG_OFF_OUT_DISP_STAT] = 0;
}
```

5. Window phase (`--window` only): draw via `js_display_clear()` / `js_display_print()` (name centered with `js_display_text_width`), flush, then loop ~700 frames (~2 s at ~2.8 ms/frame): `frame(fd)` then `bs_input_scan(&st, map + SCHWUNG_OFF_IN_MIDI, &ev)`; `ev.back_pressed` → picker phase; timeout → print incoming id, cleanup, exit 0.
6. Picker phase: cursor = incoming id's row (`--window`) or the stock row (`--forced`); redraw on change only (banner line, up to 5 rows, `>` cursor glyph, footer `Click: boot  Back: cancel`); same frame+scan loop; `jog_delta` moves cursor (clamp), `click_pressed` → write default atomically + print row id; `back_pressed` → print incoming id.
7. Cleanup path used by every exit: `munmap`, `close(fd)`, `fflush(stdout)`.

Font note: `js_display_print` lazy-loads `/data/UserData/schwung/host/font.png`. boot-select only runs when the payload exists (the factory-reset net execs MoveOriginal earlier), but if the font load fails the window is blank and the timeout path still boots the default — degraded, never stuck. Log the failure to stderr.

- [ ] **Step 2: Build block**

In `scripts/build.sh` after the schwung-heal block (~line 836), modeled on the shadow_ui link line (343-352):

```sh
if needs_rebuild build/bin/boot-select src/boot-select.c src/host/boot_select_core.c src/host/js_display.c; then
    echo "Building boot-select..."
    "${CROSS_PREFIX}gcc" -g -O2 -Isrc -Isrc/host -Ilibs/quickjs/quickjs-2025-04-26 \
        src/boot-select.c src/host/boot_select_core.c src/host/js_display.c \
        -Llibs/quickjs/quickjs-2025-04-26 -lquickjs -lm -lpthread \
        -o build/bin/boot-select
fi
```

(js_display.c drags in QuickJS for its JS bindings; the link is the same one shadow_ui already pays. Match the exact include/lib paths used at build.sh:343-352 — copy them, don't retype.)

- [ ] **Step 3: Build + verify + commit**

Run: `./scripts/build.sh` then `file build/bin/boot-select`
Expected: `ELF 64-bit LSB executable, ARM aarch64`.

```bash
git add src/boot-select.c scripts/build.sh
git commit -m "feat: boot-select SPI window + picker binary"
```

---

### Task 5: Installer surfaces

**Goal:** install.sh (both paths), post-update.sh, and uninstall.sh know about the new files; uninstall removes the registry.

**Files:**
- Modify: `scripts/install.sh` (full path ~1202, re-enable path ~1005), `scripts/post-update.sh` (~line 68), `scripts/uninstall.sh` (main(), ~line 202)

**Acceptance Criteria:**
- [ ] Both install.sh paths `chmod +x` `schwung-entry.sh` alongside the existing `shim-entrypoint.sh` chmod, and assert `bin/boot-select` + `host/boot_target_lib.sh` exist in the payload (fail loudly, same style as the 1117-1118 payload assertions).
- [ ] post-update.sh chmods `schwung-entry.sh` next to its existing shim-entrypoint chmod (line 68).
- [ ] uninstall.sh removes `/data/UserData/boot-targets` (it already restores `/opt/move/Move` from MoveOriginal at line 195 — the selector disappears with the payload).
- [ ] No change to heal (`src/schwung-heal.c` mirrors the same two files it always did), no change to features.json handling, no new `/etc/ld.so.preload` entries.
- [ ] **Web-update activation works with zero manager/heal changes** — this is why the selector keeps the `shim-entrypoint.sh` name: post-update.sh already chmods the payload and invokes heal (lines 40-45), and heal content-mirrors it to `/opt/move/Move`. Verify in Task 7's hardware pass by doing one upgrade through schwung-manager, not only install.sh. (Never-blessed devices keep the existing shim-bootstrap gap — one desktop/GUI install required, repair banner already covers it; no new work.)
- [ ] **Never-blessed devices must not regress — hard requirement (Charles: "i don't want to brick never blessed devices").** On such a device the web update writes the new payload but `/opt/move/Move` keeps the OLD entrypoint content, which launches services inline and execs MoveOriginal under the old shim. Therefore the new payload must keep every file the old entrypoint references: `schwung-shim.so` (also the payload-presence sentinel), `bin/schwung-heal`, `lib/`, `display-server`, `schwung-manager`, `bin/filebrowser` path, `host/font.png`. None may move or be renamed in this change. Add `tests/host/test_old_entrypoint_payload_compat.sh`: extract every `$SCHWUNG_DIR/`-relative path referenced by the **previous release's** entrypoint (vendor a copy of the old script into the test as a heredoc fixture) and assert each is still produced into `build/` by build.sh (check the cp/build lines, or `build/` contents when present). The selector never activating IS the correct never-blessed behavior — old boot flow, new payload, existing repair banner.
- [ ] `bash tests/host/test_features_json_preserved.sh` still passes (the merge block markers must not have moved incompatibly).

**Verify:** `bash -n scripts/install.sh scripts/post-update.sh scripts/uninstall.sh && bash tests/host/test_features_json_preserved.sh` → exit 0.

**Steps:**

- [ ] **Step 1:** install.sh full path: after line 1202's chmod add `ssh_root_with_retry "chmod +x /data/UserData/schwung/schwung-entry.sh" || fail "Failed to set entry permissions"`; extend the payload assertions at 1117-1118 with `schwung-entry.sh`, `host/boot_target_lib.sh`, `bin/boot-select`. Mirror both edits in the re-enable path (checks at 975-981, chmod at 1005).
- [ ] **Step 2:** post-update.sh line 68: add `chmod +x "$BASE/schwung-entry.sh"`.
- [ ] **Step 3:** uninstall.sh: next to the payload removal (line 202) add `$ssh_root "rm -rf /data/UserData/boot-targets"`.
- [ ] **Step 4:** Verify + commit.

Run: `bash -n scripts/install.sh scripts/post-update.sh scripts/uninstall.sh && bash tests/host/test_features_json_preserved.sh`
Expected: exit 0.

```bash
git add scripts/install.sh scripts/post-update.sh scripts/uninstall.sh
git commit -m "feat: installer surfaces for the boot selector"
```

---

### Task 6: Shim healthy touch

**Goal:** the shim touches `/data/UserData/boot-targets/schwung/healthy` once, shortly after the first SPI frame — via the shim_worker, never from the RT callback.

**Files:**
- Modify: `src/host/shim_worker.h` (new `SHIM_EVT_BOOT_HEALTHY` + hook member), `src/host/shim_worker.c` (dispatch), `src/schwung_shim.c` (hook impl + one-time post from the first-frame branch at ~5732)
- Test: `tests/host/test_boot_healthy_touch.sh`

**Acceptance Criteria:**
- [ ] New event follows the existing `SHIM_EVT_*` pattern (`shim_worker.h:88-97`) and a `boot_healthy` member in `shim_worker_hooks_t` (`:122-133`), dispatched in shim_worker.c like its siblings.
- [ ] In `schwung_shim.c`, the hook impl is a plain function doing `mkdir -p`-equivalent (`mkdir()` ignoring EEXIST) + `open(O_CREAT|O_WRONLY, 0644)`/`close` of `/data/UserData/boot-targets/schwung/healthy`, and the post happens exactly once, right after the `shim_init_subsystems()` call in `shim_pre_transfer` (lines 5732-5735), guarded by its own static flag: `shim_worker_post(SHIM_EVT_BOOT_HEALTHY)`.
- [ ] Source-pin test: `test_boot_healthy_touch.sh` asserts (a) the string `boot-targets/schwung/healthy` appears in schwung_shim.c only inside the hook function, (b) the RT-side call is `shim_worker_post`, not `open`/`fopen` (grep the ±5 lines around the post site for file-I/O calls and fail if found), (c) the event is registered in the hooks struct passed to `shim_worker_set_hooks`.
- [ ] `make -C tests/host test` and all `tests/host/*.sh` pass.

**Verify:** `bash tests/host/test_boot_healthy_touch.sh && make -C tests/host test` → exit 0.

**Steps:**

- [ ] **Step 1:** Add the event + hook (header), dispatch (worker .c), hook impl + registration + one-shot post (shim). The post site:

```c
    if (!shim_subsystems_initialized) {
        shim_init_subsystems();
    }
    static int boot_healthy_posted = 0;   /* RT path: post only, no file I/O */
    if (!boot_healthy_posted) {
        boot_healthy_posted = 1;
        shim_worker_post(SHIM_EVT_BOOT_HEALTHY);
    }
```

- [ ] **Step 2:** Write the source-pin test (house style: `set -uo pipefail`, awk-based single-pass scans, no `grep|head`).
- [ ] **Step 3:** `./scripts/build.sh` (shim must still compile), run tests, commit.

```bash
git add src/host/shim_worker.h src/host/shim_worker.c src/schwung_shim.c tests/host/test_boot_healthy_touch.sh
git commit -m "feat: shim posts boot-healthy touch via worker after first SPI frame"
```

---

### Task 7: Docs + hardware verification

**Goal:** docs match the as-built system; the feature is verified on the device.

**Files:**
- Modify: `docs/BOOT_TARGETS.md` (drop the "design-stage" banner once verified; confirm paths: `entry.sh` example exec path, `host/boot_target_lib.sh` mention if referenced), `CLAUDE.md` (one bullet in the Architecture/Deployment area pointing at `docs/BOOT_TARGETS.md`), `docs/ARCHITECTURE.md:63` (entrypoint description), `../schwung-catalog-site/manual.html` (short "Boot menu" note: the 2 s window, Back opens the picker — user-visible gesture, so the manual sync rule applies)

**Acceptance Criteria:**
- [ ] CLAUDE.md gains exactly one bullet (index style, not prose): the selector exists, shim-entrypoint.sh is the selector not the launcher, targets live in `/data/UserData/boot-targets/`, read `docs/BOOT_TARGETS.md` before touching boot.
- [ ] BOOT_TARGETS.md's status paragraph updated; every path in it matches the shipped layout.
- [ ] On-hardware checklist (deploy with `./scripts/install.sh local --skip-modules --skip-confirmation` — **ask Charles before deploying**, per standing rule):
  - [ ] Normal boot: window shows `Loading Schwung`, times out, Schwung boots, `healthy` appears, stamp cleared.
  - [ ] Back in window → picker; select Stock → stock Move boots with no shim (`tr '\0' '\n' < /proc/$(pidof MoveOriginal)/environ | grep LD_PRELOAD` is empty), no schwung-manager on :7700; `default` now reads `stock`.
  - [ ] Reboot → window shows `Loading Stock Move`; Back → select Schwung → Schwung boots, default restored.
  - [ ] Watchdog: `chmod -x` the schwung entry over SSH, reboot twice → forced picker with failure banner, cursor on Stock; restore.
  - [ ] Factory-reset net still works: `mv` the shim aside, reboot → stock Move boots (then restore).
  - [ ] Web-update activation: install the previous release, then upgrade through schwung-manager (:7700) — after reboot the boot window appears, proving post-update.sh + heal mirrored the new entrypoint with no desktop installer.
  - [ ] Never-blessed simulation: unbless heal (`chown ableton /data/UserData/schwung/bin/schwung-heal && chmod 0755 …`), restore the OLD entrypoint content to `/opt/move/Move`, web-update → device boots old-style (no window), Schwung fully works, repair banner shows. Re-bless afterward.
  - [ ] Partial-heal simulation: new entrypoint at `/opt/move/Move` but old shim in `/usr/lib` → selector runs, `healthy` never appears (old shim), yet the 30 s liveness watcher clears the stamp — two reboots do NOT force the picker.

**Verify:** the checklist above, plus `for t in tests/host/*.sh; do bash "$t"; done` all green and `make -C tests/host test` green.

**Steps:**

- [ ] **Step 1:** Doc edits (CLAUDE.md bullet, BOOT_TARGETS.md refresh, ARCHITECTURE.md line, manual note).
- [ ] **Step 2:** Ask Charles for a deploy window, then run the hardware checklist. Fix what it finds.
- [ ] **Step 3:** Commit docs; open the PR (branch → PR → green CI → merge; main is protected).

```bash
git add docs/BOOT_TARGETS.md CLAUDE.md docs/ARCHITECTURE.md
git commit -m "docs: boot selector — target-author contract as built, index hooks"
```

---

## Task order & dependencies

1 → 2 (entrypoint sources the lib) · 3 → 4 (binary links the core) · 2, 4 → 5 (installer references the shipped files) · 6 independent after 1 (needs the registry path convention only) · 7 last, blocked by all.

## Out of scope (tracked, deliberate)

- schwung-manager feature-gating when launched by a non-Schwung target (separate work item from the spec).
- `schwung-minimal` install profile.
- Manager "Boot Target" page.
- Screen-reader announcement of the boot window (boot-select has no D-Bus/TTS plumbing; note as a follow-up in the PR description).
