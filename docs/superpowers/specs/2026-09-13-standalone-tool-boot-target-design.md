# A standalone tool as a boot target (Dronage Move)

**Date:** 2026-09-13
**Status:** design approved; local build + hardware test, then an issue upstream.

## What and why

Dronage Move (`dronage-tool`, boorch) is a `"standalone": true` tool: Schwung
launches it through `launch-standalone.sh`, which kills Move, frees
`/dev/ablspi0.0` and runs the binary, then restarts Move when it exits. That is
already a full takeover — nearly what the boot selector does at boot — so
dronage should be able to boot directly, without Move starting first.

It stays a standalone tool as well. Nothing about the tool path changes.

## Scope decision: opt-in only, no generic adapter

The tempting generalisation is "any module declaring `standalone` is offered as
a boot target". Rejected: **a tool must not appear in the boot picker unless its
author asked for it.** Measured 2026-09-13, dronage is the ONLY standalone
module in the catalog — all 139 entries probed, every tool/overtake manifest
read (release-only repos downloaded and opened) — so the generic mechanism
would ship with exactly one user and would opt in future authors by surprise.

The contract for opting in already exists (`docs/BOOT_TARGETS.md`): a
`boot_target` block in the payload manifest, registered by the manager.
Dronage simply does not carry one yet.

## The two artifacts

Both live in dronage's payload, beside the `standalone` binary.

### 1. `module.json` block

```json
"boot_target": { "id": "dronage", "name": "Dronage", "exec": "boot-entry.sh" }
```

Placed **after** `id` and `name`. The host reads `module.json` with a
first-occurrence matcher (`json_get_string`, `src/host/module_manager.c`) and
this block carries its own `name`, so a block placed earlier renames the
module. The manager refuses such a manifest outright.

`name` is 11 characters, plain ASCII, no `"` or `\` — inside the 24-char
registration cap that both `boot.json` readers were measured to survive.

### 2. `boot-entry.sh`

Ordered by the failure it prevents:

1. Resolve its own directory; `BIN=$dir/standalone`. Not executable →
   `exec /opt/move/MoveOriginal`. Every failure path ends in Move booting.
2. Run the binary in the **foreground with `wait`** — not `exec`. The plain
   `exec` is what the docs suggest, and it is wrong here: see 4.
3. `trap TERM INT` → forward to the child, wait, `KILL`, exit **without the
   handover in 4**. `/etc/init.d/move stop` TERMs the service pid, which
   through the exec chain is this script; handing over there would start
   Schwung out of a stop. (Dronage is Rust with no TERM handler, so the
   default disposition already terminates it — the trap exists for the
   SPI-holder hazard in `BOOT_TARGETS.md`, where a TERM-deaf target leaves a
   black screen only `kill -9` clears.)
4. On the child's **own** exit — the QUIT in its MODE menu, and the
   `Back`+`Menu`+jog fallback — `exec /data/UserData/schwung/schwung-entry.sh`
   if present, else `exec /opt/move/MoveOriginal`. Dronage has no `fork`,
   `exec` or `system` symbols: it cannot relaunch Move itself, and today it
   does not have to, because `launch-standalone.sh` does it. As a boot target
   nothing else would, and QUIT would leave a dead device.
5. There is deliberately **no `healthy` touch**, and the reason is worth
   stating because it looks like an omission. `healthy` and the selector's
   liveness watcher are both *clears* of the same strike stamp
   (`bt_watchdog_enter`, `boot_target_lib.sh`), and the handover in 4 keeps
   this script's pid alive — which is the pid the watcher checks at 15 s. So
   the stamp is cleared whatever dronage does, and a touch on top of that
   changes nothing.

   The consequence is real and accepted: **the watchdog cannot see a dronage
   that fails**, because a failure lands in Schwung with the pid intact rather
   than dying. The user is never stranded — a broken dronage boots them into
   Schwung, where the picker and the default are both reachable — but the
   three-strike banner will not appear for it. The alternative (exit without
   handover on a fast failure, so the strike stands) trades a usable device
   for a black screen and three power cycles. Usable device wins.

It does **not** start schwung-manager. The selector starts nothing on a
target's behalf, and a boot into dronage is a clean instrument takeover. (Noted
against it: dronage's own help text points users at Schwung Manager's file
browser to retrieve recordings. Rebooting into Schwung is the answer.)

## Local install, and why the script goes in the module directory

For the hardware test the script is written to
`/data/UserData/schwung/modules/tools/dronage-tool/boot-entry.sh` — the exact
path the payload will ship — and `boot-targets/dronage/boot.json` is written by
hand to point at it.

That placement matters for what happens when boorch ships the block. A
hand-written entry is **unowned**, and the manager never touches an unowned
row, with one exception: an entry whose recorded `exec` resolves *inside the
payload directory of the module now declaring the same id* is **adopted** once
(`bootExecInsidePayload`, `schwung-manager/boot_reconcile.go:296`). Pointing
the test row at the module directory puts it exactly in that exception, so the
released version takes ownership of the row instead of colliding with it. An
entry parked anywhere else would be refused forever and could never be removed
by the manager.

Known consequence of the test setup: updating dronage-tool before boorch ships
the block wipes the module directory and leaves the row's `exec` dangling — the
target keeps its row, strikes three times, and the picker opens. That is the
watchdog working, and it is why the row is a test rather than a shipped state.

## Hardware test

1. Deploy; reboot. The picker (Back during the window) lists `Dronage`.
2. Select it: it boots, pads and audio work, the LED surface starts dark.
3. QUIT from the MODE menu → lands in Schwung, not a black screen.
4. Reboot into it again, then `/etc/init.d/move stop` over SSH → the process
   exits, no SPI holder left, `start` recovers.
5. Kill the binary from SSH mid-session: the handover fires and Schwung comes
   up, rather than the screen going black.

## Upstream

`boorch/dronage-move-release` is release-only — its tree is `.gitignore`,
`README.md`, `THIRD-PARTY-LICENSES.txt`, `release.json`. `module.json` and the
binary come out of a private build, so there is nothing there for a pull
request to change. The hand-off is an **issue** carrying both artifacts, the
test evidence, and a pointer to `docs/BOOT_TARGETS.md`.

## Schwung-side change

One docs addition: a short "a standalone tool as a boot target" recipe in
`docs/BOOT_TARGETS.md`, carrying the `wait`/trap/handover reasoning above.
Nothing in the host, shim, or manager changes — the contract already supports
this, and the recipe is what was missing.
