# Dual MoveOriginal Instances Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Run two independent MoveOriginal processes simultaneously on one Move device, sharing SPI hardware (audio mailbox, MIDI, display, LEDs) via the Schwung shim. The two instances are separate "sets," synced via Ableton Link, with a shortcut to switch which one owns the display + LEDs at any moment.

**Architecture:** Schwung's existing shim (`schwung_shim.c`) already brokers SPI ioctl for one MoveOriginal + shadow audio. Extend that broker to track two MoveOriginal PIDs, sum their audio mailboxes into the real DAC TX, gate display/LED writes by which instance is "focused," and route incoming MIDI/touch only to the focused instance. A second shim (`move-mux-shim.so`) layered between MoveOriginal and the kernel rewrites filesystem paths and singleton names (D-Bus, JACK, ALSA, lockfiles) per-instance so two copies of an unmodified MoveOriginal binary can coexist. Link sync is free — each instance is its own peer on the network and they discover each other via `lo`.

**Tech Stack:** C (LD_PRELOAD shim), POSIX (`open`/`openat`/`stat`/D-Bus/JACK/ALSA shims), existing Schwung SPI broker (`schwung_shim.c`), systemd (per-instance services), Ableton Link (sync, already used by Move).

**Risk profile:** Phase 0 is reconnaissance — its findings can invalidate Phases 2–4. Treat each phase as a GO/NO-GO checkpoint: if a phase reveals MoveOriginal can't be remapped (e.g. it embeds a kernel-level lock we can't intercept), abandon and document.

---

## Background context for an engineer with no prior Schwung exposure

Read these first, in this order:

1. `CLAUDE.md` (root) — project overview, naming, build/install commands.
2. `docs/SPI_PROTOCOL.md` — the 768-byte SPI buffer that all hardware I/O flows through.
3. `docs/REALTIME_SAFETY.md` — what you cannot do in the SPI callback path.
4. `src/schwung_shim.c` — the existing LD_PRELOAD shim that brokers SPI for MoveOriginal + Schwung shadow. **The dual-instance work is essentially a generalization of this file.**
5. `docs/plans/2026-04-17-link-audio-official-api-migration.md` — context for how shadow audio currently mixes into Move's mailbox; you'll be doing the same but with a second MoveOriginal as the source instead of (or in addition to) shadow.

Key facts to internalize:

- MoveOriginal is a **closed-source binary** owned by Ableton. We cannot modify it. Everything we do is via `LD_PRELOAD` interposition.
- Schwung already runs as a sibling to MoveOriginal. The shim is loaded into MoveOriginal's address space and intercepts its `ioctl(SPI_IOC_MESSAGE...)` calls to mix in shadow audio.
- The SPI device (`/dev/ablspi0.0`) is single-owner. The shim is the only thing talking to it. MoveOriginal *thinks* it owns it but actually goes through us.
- Audio is summed in `int16` interleaved stereo at 44100/128. Mixing two Moves is just an extra `+=` in the same path that already handles shadow.
- The hard problem is **process-level singletons**: MoveOriginal almost certainly registers a JACK client name, a D-Bus name, opens hardcoded paths under `/data/UserData/`, and writes a pidfile. Two copies will collide on every one of those unless we virtualize them.

---

## Phase 0: Reconnaissance (GO/NO-GO checkpoint)

**Purpose:** Enumerate every singleton MoveOriginal holds. Without this list, Phase 1's path-remap shim is incomplete and Phase 3 will crash.

**Output:** `docs/dual-move-recon.md` — a checklist of every shared resource, with notes on how to virtualize each.

### Task 0.1: Capture file I/O of a running MoveOriginal

**Files:**
- Create: `scripts/recon/strace-move.sh`
- Output: `/data/UserData/schwung/recon/move-strace.log` (on device — never `/tmp`, see CLAUDE.md)

**Step 1: Write the trace script**

```bash
#!/bin/bash
# Run on device. Traces MoveOriginal for 60s, captures all path-touching syscalls.
set -e
PID=$(pgrep -f MoveOriginal | head -1)
[ -z "$PID" ] && { echo "MoveOriginal not running"; exit 1; }
mkdir -p /data/UserData/schwung/recon
exec strace -f -e trace=openat,open,stat,statfs,access,unlink,rename,mkdir,opendir,connect,bind,socket \
    -p "$PID" -o /data/UserData/schwung/recon/move-strace.log &
TPID=$!
sleep 60
kill "$TPID" 2>/dev/null || true
echo "Trace saved. Lines: $(wc -l < /data/UserData/schwung/recon/move-strace.log)"
```

**Step 2: Deploy and run**

```bash
scp scripts/recon/strace-move.sh ableton@move.local:/data/UserData/schwung/
ssh ableton@move.local "chmod +x /data/UserData/schwung/strace-move.sh && /data/UserData/schwung/strace-move.sh"
```

While trace runs, exercise MoveOriginal: open a set, change a track, save a project, plug in MIDI.

**Step 3: Pull and analyze**

```bash
scp ableton@move.local:/data/UserData/schwung/recon/move-strace.log ./recon/
grep -oE '"[^"]*UserData[^"]*"' recon/move-strace.log | sort -u > recon/move-paths.txt
grep -oE 'connect.*"[^"]*"' recon/move-strace.log | sort -u > recon/move-sockets.txt
```

**Step 4: Document findings**

Write to `docs/dual-move-recon.md`:
- Every distinct path under `/data/UserData/` touched
- Every UNIX socket connected/bound (D-Bus, JACK, PulseAudio, etc.)
- Every TCP/UDP port bound
- Every device file opened (`/dev/snd/*`, `/dev/ablspi*`, `/dev/input/*`)

**Step 5: Commit**

```bash
git add scripts/recon/strace-move.sh recon/move-paths.txt recon/move-sockets.txt docs/dual-move-recon.md
git commit -m "recon: capture MoveOriginal file/socket I/O for dual-instance feasibility"
```

### Task 0.2: Enumerate JACK clients, D-Bus names, ALSA devices

**Files:**
- Modify: `docs/dual-move-recon.md`

**Step 1: List JACK clients while MoveOriginal runs**

```bash
ssh ableton@move.local "jack_lsp -A 2>/dev/null"
```

Record every Move-owned client name in the recon doc.

**Step 2: List D-Bus names**

```bash
ssh ableton@move.local "dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>/dev/null | grep -i ableton"
ssh ableton@move.local "dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>/dev/null | grep -i ableton"
```

**Step 3: ALSA device usage**

```bash
ssh ableton@move.local "fuser -v /dev/snd/* 2>&1"
ssh ableton@move.local "cat /proc/asound/card*/pcm*p/sub*/status 2>/dev/null"
```

**Step 4: Lockfiles and pidfiles**

```bash
ssh ableton@move.local "find /data/UserData /var/run /run -name '*.pid' -o -name '*.lock' 2>/dev/null"
```

**Step 5: Update recon doc and commit**

Each entry in the doc needs an "interception strategy" column:
- `path` → rewrite via `openat`/`stat` shim (Phase 1)
- `dbus name` → rewrite via `dbus_bus_request_name` shim (Phase 2)
- `jack client name` → rewrite via `jack_client_open` shim (Phase 2)
- `alsa device` → if hardcoded, may need device-namespace remap
- `pid lockfile` → rewrite path
- `tcp/udp port` → if a singleton port (not Link), needs port-remap or netns

```bash
git add docs/dual-move-recon.md
git commit -m "recon: enumerate JACK/D-Bus/ALSA singletons"
```

### Task 0.3: GO/NO-GO checkpoint

**Files:** `docs/dual-move-recon.md` (read)

Review the recon doc. Answer in writing:

1. Are all UserData writes interceptable via `open`/`openat`/`stat`? **If MoveOriginal uses `inotify` on hardcoded paths, document — that's still doable but adds work.**
2. Does MoveOriginal use D-Bus? If yes, is it session or system bus? **System bus is harder (might need policy file edits).**
3. Does MoveOriginal use JACK or talk directly to ALSA? **JACK is easier (named clients trivially remappable).**
4. Are there any kernel-level singletons we can't remap (e.g., hardcoded character device with mandatory locking)?
5. Does MoveOriginal use `/dev/shm` or POSIX shared memory? Those names need to be remapped too.

**GO criteria:** every singleton has a documented interception path. If even one doesn't (e.g., MoveOriginal opens a hardcoded netlink socket and refuses to start if it's already taken), STOP and document why this is infeasible.

**STOP if NO-GO.** Write `docs/dual-move-infeasible.md` with the blocker and abandon.

---

## Phase 1: Path-remap shim (`move-mux-shim.so`)

**Purpose:** A second LD_PRELOAD library, distinct from `schwung-shim.so`, that rewrites all `/data/UserData/` paths to a per-instance subdirectory based on `$MOVE_INSTANCE_ID`. This makes two MoveOriginal processes see two independent UserData trees.

This is a separate `.so` from the SPI shim because (a) the SPI shim is loaded into MoveOriginal already and we don't want to disturb it, and (b) it's easier to test path-remap logic standalone.

### Task 1.1: Skeleton and build

**Files:**
- Create: `src/move_mux_shim.c`
- Create: `tests/move_mux_shim_test.c`
- Modify: `Makefile` (add `move-mux-shim.so` target — find existing shim build rule and clone)

**Step 1: Write the failing test**

```c
// tests/move_mux_shim_test.c
#include <assert.h>
#include <string.h>
#include "../src/move_mux_shim.h"

int main(void) {
    char out[512];

    // No instance set → unchanged
    setenv("MOVE_INSTANCE_ID", "", 1);
    assert(mux_remap_path("/data/UserData/foo", out, sizeof(out)) == 0);
    assert(strcmp(out, "/data/UserData/foo") == 0);

    // Instance "a" → /data/UserData/move-a/foo
    setenv("MOVE_INSTANCE_ID", "a", 1);
    assert(mux_remap_path("/data/UserData/foo", out, sizeof(out)) == 1);
    assert(strcmp(out, "/data/UserData/move-a/foo") == 0);

    // Path outside UserData → unchanged
    assert(mux_remap_path("/etc/hosts", out, sizeof(out)) == 0);
    assert(strcmp(out, "/etc/hosts") == 0);

    // Already-remapped path → unchanged (idempotent)
    assert(mux_remap_path("/data/UserData/move-a/foo", out, sizeof(out)) == 0);
    assert(strcmp(out, "/data/UserData/move-a/foo") == 0);

    // Buffer too small → returns -1
    char tiny[8];
    assert(mux_remap_path("/data/UserData/foo", tiny, sizeof(tiny)) == -1);

    printf("OK\n");
    return 0;
}
```

**Step 2: Run test to verify it fails**

```bash
make move-mux-shim-test
./build/move-mux-shim-test
```

Expected: link error, `mux_remap_path` undefined.

**Step 3: Write minimal `mux_remap_path`**

```c
// src/move_mux_shim.h
#pragma once
#include <stddef.h>

// Returns 1 if remapped, 0 if unchanged, -1 if buffer too small.
int mux_remap_path(const char *in, char *out, size_t out_sz);
```

```c
// src/move_mux_shim.c (excerpt — the pure logic, not the LD_PRELOAD plumbing)
#include "move_mux_shim.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char USERDATA_PREFIX[] = "/data/UserData/";

int mux_remap_path(const char *in, char *out, size_t out_sz) {
    const char *id = getenv("MOVE_INSTANCE_ID");
    if (!id || !*id) {
        if (strlen(in) >= out_sz) return -1;
        strcpy(out, in);
        return 0;
    }
    size_t plen = sizeof(USERDATA_PREFIX) - 1;
    if (strncmp(in, USERDATA_PREFIX, plen) != 0) {
        if (strlen(in) >= out_sz) return -1;
        strcpy(out, in);
        return 0;
    }
    // Idempotency: if already starts with /data/UserData/move-<id>/, leave alone
    char already[64];
    int n = snprintf(already, sizeof(already), "/data/UserData/move-%s/", id);
    if (n > 0 && (size_t)n < sizeof(already) && strncmp(in, already, n) == 0) {
        if (strlen(in) >= out_sz) return -1;
        strcpy(out, in);
        return 0;
    }
    int written = snprintf(out, out_sz, "/data/UserData/move-%s/%s", id, in + plen);
    if (written < 0 || (size_t)written >= out_sz) return -1;
    return 1;
}
```

**Step 4: Run test to verify it passes**

```bash
make move-mux-shim-test && ./build/move-mux-shim-test
```

Expected: `OK`.

**Step 5: Commit**

```bash
git add src/move_mux_shim.h src/move_mux_shim.c tests/move_mux_shim_test.c Makefile
git commit -m "move-mux-shim: pure path-remap logic with unit tests"
```

### Task 1.2: Wrap `openat` / `open` / `stat` / `access`

**Files:**
- Modify: `src/move_mux_shim.c` (add LD_PRELOAD wrappers)
- Create: `tests/move_mux_shim_integration.sh`

**Step 1: Write the integration test**

```bash
#!/bin/bash
# tests/move_mux_shim_integration.sh — runs on Linux (CI or device)
set -e
mkdir -p /tmp/mux-test/data/UserData
echo "real" > /tmp/mux-test/data/UserData/foo
mkdir -p /tmp/mux-test/data/UserData/move-a
echo "remapped" > /tmp/mux-test/data/UserData/move-a/foo

# With remap: cat sees "remapped"
result=$(MOVE_INSTANCE_ID=a LD_PRELOAD=./build/move-mux-shim.so \
    chroot /tmp/mux-test /bin/cat /data/UserData/foo)
[ "$result" = "remapped" ] || { echo "FAIL: got '$result'"; exit 1; }
echo "OK"
```

(If chroot is overkill on dev, use a simpler test that just stat()s a file under a tmp dir with the prefix temporarily renamed. The point is to exercise the LD_PRELOAD path.)

**Step 2: Run to verify it fails**

```bash
bash tests/move_mux_shim_integration.sh
```

Expected: FAIL — wrappers not implemented.

**Step 3: Implement wrappers**

```c
// src/move_mux_shim.c (append)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);
typedef int (*stat_fn)(const char *, struct stat *);
typedef int (*lstat_fn)(const char *, struct stat *);
typedef int (*access_fn)(const char *, int);

static open_fn   real_open;
static openat_fn real_openat;
static stat_fn   real_stat;
static lstat_fn  real_lstat;
static access_fn real_access;

__attribute__((constructor))
static void mux_init(void) {
    real_open   = dlsym(RTLD_NEXT, "open");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_stat   = dlsym(RTLD_NEXT, "stat");
    real_lstat  = dlsym(RTLD_NEXT, "lstat");
    real_access = dlsym(RTLD_NEXT, "access");
}

int open(const char *pathname, int flags, ...) {
    char remapped[1024];
    const char *p = pathname;
    if (mux_remap_path(pathname, remapped, sizeof(remapped)) == 1) p = remapped;
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return real_open(p, flags, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    char remapped[1024];
    const char *p = pathname;
    if (pathname[0] == '/') {
        if (mux_remap_path(pathname, remapped, sizeof(remapped)) == 1) p = remapped;
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return real_openat(dirfd, p, flags, mode);
}

int stat(const char *pathname, struct stat *st) {
    char remapped[1024];
    const char *p = pathname;
    if (mux_remap_path(pathname, remapped, sizeof(remapped)) == 1) p = remapped;
    return real_stat(p, st);
}

// ... lstat, access similarly
```

**Step 4: Run integration test**

```bash
make move-mux-shim.so && bash tests/move_mux_shim_integration.sh
```

Expected: `OK`.

**Step 5: Commit**

```bash
git add src/move_mux_shim.c tests/move_mux_shim_integration.sh
git commit -m "move-mux-shim: wrap open/openat/stat/access for path remap"
```

### Task 1.3: Cover the rest from recon (`unlink`, `rename`, `mkdir`, `opendir`, `__xstat`, `realpath`, etc.)

**Files:**
- Modify: `src/move_mux_shim.c`
- Modify: `tests/move_mux_shim_integration.sh`

For each syscall in `recon/move-paths.txt` that isn't yet wrapped, add a wrapper following the same pattern. Add an integration test exercising it. Commit per syscall family.

**Critical:** glibc has versioned `__xstat`, `__xstat64`, `__fxstatat` symbols on some libc builds — check what MoveOriginal actually links against (`ldd` + `nm -D --version`) and wrap those too. If you only wrap `stat()` and MoveOriginal calls `__xstat()`, your remap silently does nothing.

```bash
ssh ableton@move.local "nm -D /opt/move/MoveOriginal/MoveOriginal | grep -E 'stat|open' | head -30"
```

### Task 1.4: Smoke test — boot one instance under the shim alone

**Files:**
- Create: `scripts/launch-move-instance.sh`

**Step 1: Pre-create UserData skeleton for instance "a"**

```bash
ssh ableton@move.local "cp -a /data/UserData /data/UserData-move-a-template && \
    mkdir -p /data/UserData/move-a && \
    cp -a /data/UserData-move-a-template/. /data/UserData/move-a/"
```

(Adjust based on what's actually in UserData; you may want to symlink read-only assets like factory packs to save disk.)

**Step 2: Stop normal MoveOriginal, launch under shim with instance ID**

```bash
ssh ableton@move.local "systemctl --user stop move || systemctl stop move"
ssh ableton@move.local "MOVE_INSTANCE_ID=a LD_PRELOAD=/data/UserData/schwung/move-mux-shim.so /opt/move/MoveOriginal/MoveOriginal 2>&1 | head -200"
```

Watch for: does it boot? Does it say "saved to /data/UserData/move-a/..."? Touch the screen, save a project, verify it lands in `/data/UserData/move-a/`.

**Step 3: Document findings**

Update `docs/dual-move-recon.md` — what crashed, what worked, what extra paths showed up that recon missed.

**Step 4: Commit**

```bash
git add scripts/launch-move-instance.sh docs/dual-move-recon.md
git commit -m "move-mux-shim: smoke test single instance under path-remap shim"
```

### Task 1.5: GO/NO-GO checkpoint

Did one MoveOriginal boot cleanly under `move-mux-shim.so` and write only to `/data/UserData/move-a/`? Did anything spuriously go to the real `/data/UserData/`?

If anything leaked to the real path, find the missing syscall and wrap it. Loop until clean.

**GO criteria:** zero writes from instance "a" land outside `/data/UserData/move-a/` during a 5-minute exercise (open set, edit pattern, save, switch tracks, plug in MIDI).

---

## Phase 2: D-Bus / JACK / ALSA name remap

**Purpose:** Wrap the connection-establishment functions so each MoveOriginal registers under a unique well-known name. Without this, the second instance fails to claim its bus name and exits.

Scope is dictated by Phase 0 recon. If MoveOriginal doesn't use D-Bus, skip 2.1. Same for JACK.

### Task 2.1: D-Bus name suffix

**Files:**
- Modify: `src/move_mux_shim.c`
- Test: `tests/dbus_name_remap_test.sh`

Wrap `dbus_bus_request_name` (and `dbus_connection_request_name` if used). Append `.move_<id>` to the requested name. Same for `dbus_bus_get_unique_name` callers — though that one's auto-generated and shouldn't conflict.

Verify with `dbus-monitor` while running two instances: each registers a distinct name.

### Task 2.2: JACK client name suffix

**Files:**
- Modify: `src/move_mux_shim.c`

Wrap `jack_client_open`. Append `_<id>` to the client name argument before passing through. JACK itself will then see two distinct clients.

Verify with `jack_lsp -A` after launching both instances.

### Task 2.3: ALSA device — usually no remap needed

If both instances open `/dev/snd/pcmC0D0p`, that's fine — the SPI broker (Phase 4) is going to consume their audio output anyway, not the kernel ALSA path. Document that ALSA "just works" because both instances write to a device the broker virtualizes.

If MoveOriginal opens ALSA in exclusive mode and refuses if it's busy, intercept `snd_pcm_open` and return a fake handle the broker manages.

### Task 2.4: Commit and checkpoint

Commit per syscall family. After each, verify a single instance still boots cleanly.

---

## Phase 3: Boot instance "b" alongside instance "a"

**Purpose:** Get two MoveOriginal processes coexisting. Audio is going to be wrong (both writing to SPI mailbox uncoordinated) and display will be a mess (both writing display chunks, racing) — but they should both be alive, claiming distinct UserData trees and distinct JACK/D-Bus names.

### Task 3.1: Launch script for instance b

**Files:**
- Create: `scripts/launch-move-instance.sh` (parameterize)
- Create: `systemd/move-a.service`, `systemd/move-b.service`

```bash
#!/bin/bash
# scripts/launch-move-instance.sh <a|b>
set -e
ID=$1
[ -z "$ID" ] && { echo "usage: $0 <a|b>"; exit 1; }
export MOVE_INSTANCE_ID="$ID"
export LD_PRELOAD="/data/UserData/schwung/move-mux-shim.so:/data/UserData/schwung/schwung-shim.so"
exec /opt/move/MoveOriginal/MoveOriginal
```

systemd unit files: stock template, `Environment=MOVE_INSTANCE_ID=a` etc., `Restart=on-failure`, `CPUAffinity=` to be filled in once we know CPU budget.

### Task 3.2: Stop stock service, launch both

Disable the stock `move.service`. Launch `move-a.service` and `move-b.service`. Both should reach a "ready" state (logs show set loaded, no crash loops).

Expect: the display is garbled (both writing display chunks), audio is duplicated/destroyed, but neither process exits.

If one exits, capture stderr and find what singleton we missed. Loop back to Phase 0/1/2 as needed.

### Task 3.3: Checkpoint

**GO criteria:** both processes stable for 60+ seconds. Nothing in dmesg about device contention. JACK shows two clients, D-Bus shows two names. UserData writes go to the right subdirs.

---

## Phase 4: Extend `schwung_shim.c` to broker SPI for two MoveOriginals

**Purpose:** Generalize the existing single-MoveOriginal SPI broker to handle two. The shim is loaded into both processes (because `LD_PRELOAD` applies to both), so each process's `ioctl` calls land in their own copy of the shim — but they both target the same SHM-coordinated broker state.

Read `src/schwung_shim.c` carefully before this phase. Key existing concepts:
- `mailbox` (the real DAC TX) is composed from Move audio + shadow audio (post-MFX).
- `ioctl` is intercepted; the shim runs the real ioctl, then post-processes.
- Shadow audio comes from SHM (`/schwung-audio`).

### Task 4.1: Per-instance SHM mailbox

**Files:**
- Modify: `src/schwung_shim.c`
- Modify: `src/host/shadow_constants.h` (add new SHM segment names)

**Plan:** instead of MoveOriginal's `ioctl` writing to the real SPI device, intercept and route to a per-instance SHM mailbox (`/schwung-move-a-tx`, `/schwung-move-b-tx`). Each instance writes audio to its own SHM. A new broker thread (or the existing SPI thread) reads both, sums, writes to real SPI.

**Step 1: Add SHM segments**

Add to `shadow_constants.h`:
```c
#define SCHWUNG_MOVE_A_TX_SHM "/schwung-move-a-tx"
#define SCHWUNG_MOVE_B_TX_SHM "/schwung-move-b-tx"
// Each is a single-buffer mailbox of 768 bytes (full SPI TX layout) with seq counters for read/write.
```

**Step 2: Detect which instance the shim is loaded into**

In `mux_init` (or a new `__attribute__((constructor))`), read `$MOVE_INSTANCE_ID` and stash. Subsequent `ioctl` calls write to the right per-instance SHM.

**Step 3: Broker reads both and composes real SPI buffer**

In the shim's existing SPI-output composition path, replace "Move TX" with "sum of move-a TX + move-b TX, gated by audio mute flags." Use atomic seq counters so we don't read torn buffers.

**Step 4: Display chunks gated by `/schwung-control.active_move_instance`**

Display data lives in bytes 80–251 of the SPI TX buffer. Only one instance's display can reach the real SPI. Add a flag to `/schwung-control` (existing SHM):
```c
typedef enum { ACTIVE_MOVE_A = 0, ACTIVE_MOVE_B = 1 } active_move_t;
```

Broker copies the display region from whichever instance is active; the other instance's display writes are dropped on the floor.

**Step 5: LEDs gated similarly**

LEDs are part of MIDI_OUT (cable 0, the first 80 bytes of SPI TX). Filter: only the active instance's LED packets reach real SPI. The inactive instance's MIDI_OUT bytes are buffered (so when it becomes active, its current LED state can be replayed in one burst).

**Step 6: MIDI_IN routed to active instance only**

MIDI_IN (cable 0) — pads, knobs, buttons, touch — goes only to the active instance. The inactive instance gets an empty MIDI_IN buffer this frame. Cable 2 (external USB) — decide policy (probably also active-only, with a future option to route to both).

**Step 7: Write tests for the composition logic**

Carve the buffer composition out of the shim into a pure function `compose_spi_tx(move_a_tx, move_b_tx, shadow_audio, active, out)` and unit-test it standalone.

**Step 8: Commit**

Multiple commits — per concern (audio mix, display gating, LED gating, MIDI_IN routing).

### Task 4.2: Realtime safety review

Per `docs/REALTIME_SAFETY.md`: the SPI callback is FIFO 90 on core 3, ~900µs budget. Adding a second source means an extra 256-byte memcpy + 128-sample int16 add. That's microseconds — fine.

What's *not* fine: any new `unified_log` call, any new lock, any new SHM `ftruncate`. The broker must allocate all SHM up front (in init) and use lock-free seq counters for cross-process synchronization.

Run a stress test: both instances loaded, full audio output, JACK + RNBO running. Watch for SPI underruns in `dmesg` and audio glitches.

### Task 4.3: Checkpoint

**GO criteria:**
- Display shows instance A only when `active=A`, instance B only when `active=B`. No glitching.
- LEDs match the active instance's intended state.
- Audio plays from both instances simultaneously, mixed correctly.
- Switching `active_move_instance` via `echo` to the SHM control struct visibly switches display + LEDs within one frame.
- No SPI underruns over a 5-minute test.

---

## Phase 5: Focus-switch shortcut

**Purpose:** Bind a hardware shortcut that toggles `active_move_instance` so the user can flip between sets.

### Task 5.1: Pick the shortcut

Read `docs/long-press-shadow-shortcuts.md` for context on how shadow shortcuts are detected. The shim already has shortcut-detection logic (`shadow_ui_trigger`, Shift+Vol combos).

Candidate: **Shift+Vol+Step1** (Step 2 is Global Settings, Step 13 is Tools — Step 1 is free). Or a long-press of a dedicated button.

### Task 5.2: Implement the toggle

**Files:**
- Modify: `src/schwung_shim.c` (extend shortcut detection)

When the shortcut fires:
1. Increment `active_move_instance` modulo 2.
2. Replay the new active instance's last-known LED state (from the buffered LED state per Task 4.1 step 5).
3. Mark the new active instance's display dirty so it does a full redraw on next frame.

### Task 5.3: Test

**GO criteria:** pressing the shortcut switches display + LEDs cleanly, with no flicker or stale frame. Audio from both keeps playing.

### Task 5.4: Commit

```bash
git commit -m "shim: focus-switch shortcut for dual-Move instances"
```

---

## Phase 6: Link sync verification

**Purpose:** Verify both instances appear as separate Link peers and tempo/transport-sync each other automatically.

### Task 6.1: Observe peer discovery

```bash
ssh ableton@move.local "ip -6 mr show 2>/dev/null"  # multicast routes
ssh ableton@move.local "ss -ulpn | grep 20808"      # Link uses UDP 20808
```

Both Move processes should be bound and visible to each other on `lo` (and to Live on the network).

### Task 6.2: Tempo-change test

In instance A, change tempo to 110. Verify instance B follows. Vice versa.

If they don't sync: check whether MoveOriginal binds Link to a specific interface. Loopback (`::1`) might not get the multicast traffic by default — may need to enable `IFF_MULTICAST` on `lo` or bind Link to an interface that sees both peers (any wired/wifi interface counts).

### Task 6.3: Transport-sync test

Start in A, B should follow. Stop in A, B should stop (or not, depending on Link transport-sync settings — that's a Move setting, not ours).

### Task 6.4: Document and commit

Update `MANUAL.md` with the dual-instance feature: how to enable, how to switch, how Link sync works. Update `docs/MODULES.md` if any module-side changes are required.

---

## Phase 7: Service plumbing, autostart, watchdogs

**Purpose:** Make this run cleanly on boot, survive instance crashes, and play nice with the Schwung installer.

### Task 7.1: systemd units

`systemd/move-a.service`, `systemd/move-b.service` — environment, dependencies (`After=network.target schwung-shim-loaded.target` if any), restart policy, CPU affinity (probably 0x3 for instance a, 0x6 for b — keep core 3 free for SPI per `docs/REALTIME_SAFETY.md`).

### Task 7.2: Feature flag

Add to `features.json`:
```json
"dual_move_instances": false
```

When `false`, install script preserves stock single-Move behavior. When `true`, installs the dual units and disables stock `move.service`.

### Task 7.3: Installer integration

Modify `scripts/install.sh` to install `move-mux-shim.so` next to `schwung-shim.so` and to enable the dual units if the flag is set. Don't auto-enable — this is opt-in via Global Settings.

### Task 7.4: Global Settings UI

Expose the toggle in the Global Settings menu (`src/host/menu_ui.js` or wherever Schwung settings live). Display a warning: "Experimental — doubles CPU load."

### Task 7.5: Watchdog

If instance B crashes, instance A should keep running. systemd `Restart=on-failure` handles this. The shim broker should detect a dead instance (SHM seq counter stops advancing) and stop summing its audio (zero it instead of reading stale data).

### Task 7.6: Commit and final checkpoint

Tag a release: `git tag dual-move-experimental-v0.1`. Document in `docs/EXPERIMENTAL.md` that this is alpha.

---

## Out of scope (explicit non-goals)

- **Three or more instances.** The plan generalizes if Phase 1 works, but Move's CPU won't have headroom — don't promise this.
- **Per-instance USB-MIDI device routing.** All external MIDI goes to the active instance only, full stop. A future feature could route per-channel, but not now.
- **Synchronized recording across instances.** Each instance's recordings land in its own UserData. Combining them is a post-process problem.
- **Session import between instances.** Each instance is independent; copying a set from A to B is a userspace `cp`.
- **Headless instance shadow-mode access.** While B is unfocused, you can't open shadow UI on it. (Conceivable but doubles the shim complexity.)

---

## Verification plan (run before declaring done)

Use **superpowers:verification-before-completion** before claiming this works.

1. Boot both instances cold from systemd. Both reach ready state in <30s.
2. Load a different set in each. Verify file isolation (`ls /data/UserData/move-a/Sets/ /data/UserData/move-b/Sets/`).
3. Play 2 minutes of audio from both simultaneously while switching focus every 10 seconds. No crashes, no audio glitches >0.5%, display switches cleanly.
4. Change tempo in A → B follows within 1s (Link sync).
5. Plug in external USB MIDI keyboard. Verify it routes to whichever is focused.
6. Shadow UI: open Schwung shadow on the focused instance. Should work normally.
7. Run for 30 minutes under load. No `dmesg` SPI errors, no zombie processes, both UserData trees only contain their own writes.

---

## Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| MoveOriginal uses a kernel-level singleton we can't intercept | Med | Phase 0 reconnaissance gates everything else. Stop early if discovered. |
| CPU budget insufficient for 2× Move + Schwung + RNBO | High | Pin instances to different cores; possibly disable RNBO when dual-mode active. |
| Display/LED flicker on switch is unacceptable UX | Med | Replay last-known state on switch (Phase 4 Task 4.1 step 5). If still bad, double-buffer the display SHM per instance. |
| Link sync doesn't work over `lo` | Low | Bind Link to a real interface (wifi up always on Move). |
| ALSA exclusive open prevents second instance | Med | Phase 2 Task 2.3 — wrap `snd_pcm_open` if needed. |
| State file corruption when one instance crashes mid-write | Med | Each UserData is independent; instance A is unaffected. systemd restarts B. |
| `__xstat` versioned glibc symbols unwrapped → silent path leaks | High | Phase 1 Task 1.3 explicitly checks `nm -D` against MoveOriginal. |

---

## Skills referenced

- @superpowers:verification-before-completion — Phase 7.6 and final checkpoint
- @superpowers:test-driven-development — Phase 1 unit tests
- @superpowers:systematic-debugging — for any "instance B exits with status 1" investigation
- @superpowers:requesting-code-review — before merging any phase
