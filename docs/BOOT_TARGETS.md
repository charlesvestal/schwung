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
- **The LED surface arrives still, but unpainted.** The XMOS plays Move's
  power-on LED show autonomously until a MIDI system reset arrives; the
  selector sends that reset at handover (bisected from Move's own boot
  traffic — color writes and per-LED animation cancels do NOT stop it), so
  your target starts on a dark, quiet surface. Paint your own LED state at
  startup; do not expect the show to be stoppable later by ordinary writes.
- **Exit on SIGTERM.** `/etc/init.d/move stop` TERMs the service pid — which,
  through the exec chain, is your platform. A target that ignores TERM
  survives the stop, keeps `/dev/ablspi0.0` open, and the next start finds
  the device busy: the user sees a black screen that even a service restart
  cannot clear (observed on hardware with a TERM-deaf binary; only `kill -9`
  freed it). Handle TERM and exit promptly.
- **Never write to `/tmp` on the device.** The root FS is ~463 MB and usually
  full. Use `/data/UserData/`.
- **Do not modify** `/opt/move/Move`, `/opt/move/MoveOriginal`,
  `/usr/lib/schwung-shim.so`, or `/etc/ld.so.preload`. `MoveOriginal` is the
  stock firmware backup and the device's last-resort boot path; if you want to
  run it (shimmed or bare), exec it from your entry script.

## Watchdog

The selector counts boot attempts per target and clears the count when the
boot looks good. **Three** un-cleared attempts in a row → the picker opens
unconditionally with `<name> failed to start`, defaulting to Stock Move. Your
platform can never boot-loop the device.

Three, not two, because a power cycle inside the liveness window is
indistinguishable from a failed boot: at two, an installer's reboot plus one
human power cycle forced the picker on a device that was working perfectly. A
real crash-loop reaches three in seconds, so the extra strike costs it nothing.
The limit lives in one place, `BT_STRIKE_LIMIT` in `boot_target_lib.sh`.

Two ways your boot counts as good — pick either:

1. **Do nothing.** A detached watcher clears the stamp if your process is
   still alive ~15 seconds after exec. Staying alive is participation.
2. **Opt in (better):** touch `/data/UserData/boot-targets/<id>/healthy` once
   your platform has actually reached working state (Schwung's own shim
   touches it after ~30 seconds of continuously clocked SPI frames). This
   catches "alive but wedged", which the liveness fallback cannot. Touch it
   once your platform has been in
   working state for tens of seconds, not merely started — a first-frame or
   first-callback touch defeats the watchdog entirely: a build that crashes
   seconds into the session would still mark every boot healthy, and the
   attempt count could never reach the strike limit.

A target whose entry script is permanently missing or broken (so its process
never starts, and neither the liveness fallback nor a `healthy` touch can ever
fire) accumulates strikes with no decay: the forced picker appears on **every**
boot from then on, not just the first time it trips, until the target is
repaired or another default is chosen. This is deliberate — the alternative is a watchdog
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

## Registering through Schwung Manager

Anything the manager installs — an ordinary module that ships a binary, or a
platform payload with no module in it — can declare a boot target in its
manifest and let the manager register it, instead of you writing
`boot-targets/<id>/` by hand:

```json
"boot_target": { "name": "V", "exec": "entry.sh" }
```

`exec` is **relative to your own payload directory**; the manager composes the
absolute path (`resolveBootExec` in `schwung-manager/boot_target.go`). A
payload never states where it is installed, so moving the install roots is one
change plus one reconcile pass rather than an edit per target.

Both payload shapes carry the same block: a module's `module.json`, or a
platform's `platform.json` (`id`, `name`, `version`, `author`, and
`boot_target`) — platform payloads install to `/data/UserData/platforms/<id>/`
from a tarball whose top-level directory is `<id>/`.

Rules, all enforced at registration with the reason logged:

- `id` (optional, defaults to your payload id) matches `[a-z0-9-]+` and is not
  `schwung` or `stock` — both reserved, for the reasons above.
- `name` is 1–24 printable-ASCII characters and **must not contain `"` or
  `\`**. This is not a style preference — it is what the two `boot.json`
  readers were *measured* to survive (2026-09-09,
  `tests/host/test_boot_target_manager_json.sh`): a value carrying a `"` is
  truncated at it by both `bt_json_field` (awk) and `bs_json_field` (C), and
  neither can unescape, so such a name is refused at registration rather than
  silently mangled at boot. Two *other* shapes are just as dangerous and are
  why the manager writes the registry the way it does rather than because
  quoting is hard: an **unquoted** value (`"version": 3`) reads back as
  **empty** — `bt_json_field` requires a quote after the colon — and the
  **first textual occurrence of a key wins**, so a nested object carrying the
  same key **shadows** the real one. Neither is a name-content rule (they are
  about how the manager serializes the file, not what you may type), which is
  why the registry is written flat, one field per line, string values only —
  see `writeRegistryEntry` in `schwung-manager/boot_registry.go`.
- `exec` is relative, does not contain `..`, resolves inside your directory
  (symlinks included), and exists after extraction. A file that lost its
  executable bit in transit is chmodded rather than refused.
- The picker holds **14 targets** beside Stock and Schwung. Registration past
  that is refused: `bs_row_insert_sorted` drops the overflow silently, in id
  order, so a target that "did not appear" would be unattributable.

The manager writes `boot.json` with an `owner` field (`module:<id>` or
`platform:<id>`) and **only ever rewrites or deletes entries carrying an
owner it recognises**. A target you installed by hand, as described above, has
no `owner` and is never touched. The reverse is also true: **do not hand-edit
an entry the manager owns** — the next reconcile pass (every install,
uninstall, and update) will put it back.

Uninstalling the payload removes its entry, and heals `boot-targets/default` to
`schwung` if it named the removed target. Installing **never** changes the
default: a new target is a new row in the picker, nothing more.

Uninstalling Schwung removes the whole registry (`scripts/uninstall.sh`), so
every target is deregistered at once. Platform payloads are left on disk under
`/data/UserData/platforms/`; reinstalling Schwung brings them back as picker
rows at the manager's next reconcile, but the previous default is gone.

## What the user sees

- Every boot: `Loading <name> — press Back to change`, ~2 s.
- Back during the window: the picker. Selecting a row boots it **and makes it
  the new default** — there is no boot-once mode, because the next boot's
  window is always an escape hatch.
- Three failed boots of any target: the picker, with a failure banner, cursor
  on Stock Move. Three and not two because a power cycle inside the liveness
  window is indistinguishable from a failed boot — see Watchdog.
