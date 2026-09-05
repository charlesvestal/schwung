# Being a Boot Target

Schwung ships a boot selector: `/opt/move/Move` is a thin Schwung-owned
entrypoint that shows `Loading <name> — press Back to change` for ~2 seconds at
every boot, then execs one registered **target**. Pressing Back in that window
opens a picker (jog scrolls, jog-click selects and sets the new default, Back
cancels). This document is everything a third-party platform needs to do to be
one of those targets.

**Status: implemented.** The selector is built on branch `boot-selector-spec`
(design: `docs/superpowers/specs/2026-09-05-boot-selector-design.md`), pending
on-hardware verification and release. The schema and paths below match the
implementation (`src/shim-entrypoint.sh`, `src/host/boot_target_lib.sh`,
`src/boot-select.c`, `src/schwung-entry.sh`).

## The contract in one paragraph

Install Schwung first (it owns the selector). Drop a directory under
`/data/UserData/boot-targets/<your-id>/` containing a `boot.json` and an entry
script. Never touch `/opt/move/Move`, `/usr/lib/schwung-shim.so`, or
`/etc/ld.so.preload` — Schwung's installer and its boot-time heal own those,
and a platform that rewrites them will be silently reverted (or will fight
heal, which is worse). That's it: your target appears in the picker by name.

## Registration

```
/data/UserData/boot-targets/<id>/
  boot.json     # required
  entry.sh      # your entry script (any name; boot.json points at it)
  healthy       # optional, see Watchdog
```

`<id>` is lowercase-hyphenated, and must not be `schwung` or `stock` — both are
reserved. `stock` is a built-in row with no directory (see below); `schwung`'s
`boot.json` is **self-registered by Schwung's own selector on every boot**, not
written once by an installer — a wiped, hand-edited, or missing
`schwung/boot.json` heals itself on the very next boot, pointing `exec` at
`/data/UserData/schwung/schwung-entry.sh`. Do not create a `schwung/` directory
of your own; the selector owns and rewrites it unconditionally.

`boot.json`:

```json
{
  "name": "V",
  "exec": "/data/UserData/boot-targets/v/entry.sh",
  "version": "0.1.0",
  "author": "djhardrich"
}
```

- `name` — what the boot window and picker display.
- `exec` — absolute path to your entry script. It is exec'd (not sourced) as
  user `ableton`, replacing the selector process.
- Unknown fields are ignored; add what you like.

## Your entry script

It is the last thing the selector runs, so it should end in `exec` of your
main binary. Rules:

- **You run as `ableton`, not root.** If you need root-side setup, ship your
  own setuid helper the way Schwung ships `schwung-heal` — the selector will
  not escalate for you.
- **Launch your own services.** The selector starts nothing on your behalf: no
  schwung-manager, no display-server. If you want schwung-manager's module
  store / file browser (it can install Schwung modules your platform loads),
  launch it yourself from your entry script — it lives at
  `/data/UserData/schwung/schwung-manager`. Schwung-specific manager features
  (Remote UI, slot configs) are gated when it runs outside Schwung.
- **Reset scheduling before spawning workers.** The boot context is ordinary,
  but if your platform inherits or acquires realtime priority, children must
  be SCHED_OTHER — see `docs/REALTIME_SAFETY.md` for why FIFO-70 children
  starve Move's own audio threads.
- **Never write to `/tmp` on the device.** The root FS is ~463 MB and usually
  full. Use `/data/UserData/`.
- **Do not modify** `/opt/move/Move`, `/opt/move/MoveOriginal`,
  `/usr/lib/schwung-shim.so`, or `/etc/ld.so.preload`. `MoveOriginal` is the
  stock firmware backup and the device's last-resort boot path; if you want to
  run it (shimmed or bare), exec it from your entry script.

## Watchdog

The selector counts boot attempts per target and clears the count when the
boot looks good. Two un-cleared attempts in a row → the picker opens
unconditionally with `<name> failed to start`, defaulting to Stock Move. Your
platform can never boot-loop the device.

Two ways your boot counts as good — pick either:

1. **Do nothing.** A detached watcher clears the stamp if your process is
   still alive ~30 seconds after exec. Staying alive is participation.
2. **Opt in (better):** touch `/data/UserData/boot-targets/<id>/healthy` once
   your platform has actually reached working state (Schwung's own shim
   touches it after ~30 seconds of continuously clocked SPI frames). This
   catches "alive but wedged", which the liveness fallback cannot. Touch it
   once your platform has been in
   working state for tens of seconds, not merely started — a first-frame or
   first-callback touch defeats the watchdog entirely: a build that crashes
   seconds into the session would still mark every boot healthy, and the
   attempt count could never reach two.

A target whose entry script is permanently missing or broken (so its process
never starts, and neither the liveness fallback nor a `healthy` touch can ever
fire) accumulates strikes with no decay: the forced picker appears on **every**
boot from then on, not just the first two, until the target is repaired or
another default is chosen. This is deliberate — the alternative is a watchdog
that quietly stops warning you.

## Installing / uninstalling your platform

- **Depend on Schwung.** Check that the boot selector is present before you
  register — e.g. `grep -q 'Boot selector' /opt/move/Move` (its comment
  banner), or simply `test -f /data/UserData/schwung/bin/boot-select`. If
  Schwung isn't installed, tell the user to install it first rather than
  improvising your own entrypoint.
- Install = create your `boot-targets/<id>/` directory. You may set
  `/data/UserData/boot-targets/default` to your id **only on explicit user
  choice** — never as a silent side effect of installing.
- Uninstall = remove your directory. If `default` names your id, rewrite it to
  `schwung`. The selector also tolerates a dangling default (falls back to
  Schwung, then stock), so a sloppy uninstall degrades gracefully.

## What the user sees

- Every boot: `Loading <name> — press Back to change`, ~2 s.
- Back during the window: the picker. Selecting a row boots it **and makes it
  the new default** — there is no boot-once mode, because the next boot's
  window is always an escape hatch.
- Two failed boots of any target: the picker, with a failure banner, cursor on
  Stock Move.
