# Boot Selector — Design

2026-09-05. Designed with djhardrich's chainloading idea as the starting point:
run a different "main" binary on boot (Schwung+MoveOriginal, stock MoveOriginal,
or a third-party platform such as his "V"), chosen from a stored default, an
interruptible boot window, and a watchdog that prevents boot loops.

## Goals

- Any installed platform ("target") can be the thing that runs on boot.
- The user can switch targets from the device at every boot, with no memorized
  gesture: the boot screen itself says how.
- A broken target can never boot-loop the device: the watchdog falls back, and
  the ultimate fallback is always stock Move.
- Third-party targets need close to zero cooperation to participate, and the
  cooperation that exists is documented in one file (`docs/BOOT_TARGETS.md`).

## Non-goals

- A `schwung-minimal` install profile (loader + manager substrate only) is
  future work, noted but not designed here.
- A Boot Target page in schwung-manager is a later nice-to-have, not part of
  this deliverable.
- No EFI-style bootloader, no image switching. This selects which process is
  exec'd by `/opt/move/Move`; the OS image is untouched.

## Ownership

**The selector ships in Schwung, and Schwung's installer is the sole writer of
`/opt/move/Move`.** Other platforms declare a dependency on Schwung being
installed and register themselves as targets; they never touch `/opt/move/Move`
or the `/usr/lib` shim. One installation path, one heal master. (Decided over
the alternative — a neutral selector both installers ship — because a single
writer is the thing that actually prevents the last-installer-wins fight, and
platforms get schwung-manager and the heal infrastructure by leaning on
Schwung.)

## Registry layout

A *neutral* path, so peer platforms never write inside Schwung's directory:

```
/data/UserData/boot-targets/
  default                 # plain text: target id
  .boot-attempt           # watchdog stamp: target id + attempt count
  schwung/boot.json       # self-registered by the selector, every boot
  schwung/healthy         # touched by the shim after ~30s of healthy SPI frames
  v/boot.json             # registered by V's installer
  v/...
```

As built, Schwung's own target entry keeps the name `schwung-entry.sh`
(`/data/UserData/schwung/schwung-entry.sh`) rather than moving under
`boot-targets/schwung/`; `boot.json`'s `exec` field just points at it. The
`schwung/boot.json` file is not written once by the installer — it is
**self-registered by the selector on every boot** (see Entrypoint split
below), so a wiped or hand-edited entry heals itself on the next boot rather
than needing a reinstall.

- `default` is a bare id in its own file — **not** a `features.json` key. The
  installer rewrites `features.json` from a merge and this value must survive
  any Schwung update path, including old installers that predate the selector.
- `boot.json` fields: `name` (display string), `exec` (absolute path to the
  entry script), `version`, `author`. Unknown fields ignored.
- **Stock Move is a built-in row in the selector, not a file.** It cannot be
  deleted, corrupted, or shadowed, and it is the watchdog's terminal fallback.
  Its behavior is `exec /opt/move/MoveOriginal` — truly stock: no LD_PRELOAD,
  no manager, no sidecars.

## Entrypoint split

`/opt/move/Move` (Schwung-owned, installed by `install.sh`, mirrored by
`schwung-heal`) becomes a thin shell selector:

1. **Factory-reset safety net** (unchanged from today): payload gone →
   `exec /opt/move/MoveOriginal`.
2. **`schwung-heal`** (unchanged): mirror shim + entrypoint if stale.
3. **Resolve target**: read `default`; validate the id against the registry
   (unknown/missing → `schwung` if registered, else stock).
4. **Watchdog check**: read `.boot-attempt` (see below).
5. **Run `boot-select`** (new small C binary): opens `/dev/ablspi0.0`, paints
   `Loading <name> — press Back to change`, samples ~2 s of SPI frames for a
   Back **press edge** (the gesture must begin inside the window — buttons are
   only readable once someone clocks SPI frames, so hold-through-power-on can
   never be detected; edge detection also ignores stale junk in the first
   MIDI_IN frames). On Back, or unconditionally when the watchdog forced it,
   show the picker: jog scrolls, jog-click selects, Back cancels to the
   default. **Selecting sets the default** (the boot window's "press Back to
   change" makes every boot an escape hatch, so sticky selection is safe).
   Closes the SPI fd completely, prints the chosen id on stdout, exits.
6. **Exec** the chosen target's entry script as `ableton`.

Failure containment, in order: `boot-select` missing or crashing → fall through
silently to the default target with no window; entry script missing → stock;
everything broken → `exec /opt/move/MoveOriginal`. **The selector's own failure
mode is always "stock Move boots."** The selector stays deliberately tiny and
boring for exactly this reason.

## Sidecars move into targets

Today's entrypoint starts schwung-manager, display-server, and filebrowser
before exec'ing Move. Those launches move into **Schwung's target entry**. The
selector launches nothing but the target's entry script.

- Stock's behavior: `exec /opt/move/MoveOriginal`, nothing else. No manager.
  Switching back is reboot + Back — accepted.
- V launches its own services, optionally including schwung-manager itself.
  The manager needs gating for Schwung-only features (Remote UI, slot configs)
  when launched outside Schwung — **separate work item**, tracked but not
  designed here.

## Watchdog

Move's init restarts `/opt/move/Move` when the child dies, so each crash
re-enters the selector. Protocol:

- On entry, increment `.boot-attempt` for the resolved target (create at 1).
- Clearing the stamp, two ways (opt-in health signal, liveness as fallback):
  1. **Opt-in health file**: the target touches
     `/data/UserData/boot-targets/<id>/healthy`; the next selector entry treats
     that as a good boot and clears the stamp. Schwung's shim touches it after
     ~30 s of continuously clocked SPI frames (a first-frame touch would mark
     a boot healthy even when a restored module crashes seconds later — the
     historical boot-loop case the watchdog exists for).
  2. **Liveness fallback**: the entrypoint detaches a small watcher before
     exec (background subshell; must reset to SCHED_OTHER like every other
     child) that sleeps ~30 s, confirms the target process is alive, and
     clears the stamp. Zero cooperation needed — a convention-ignorant binary
     participates by simply staying alive.
- Two un-cleared attempts → `boot-select` opens the picker unconditionally
  with a `<name> failed to start` banner. The failed target stays listed (the
  user may want to retry); the highlighted row is stock.

A wedged-but-alive target is not detected by the fallback; the user's remedy is
reboot + Back, which always works because the window shows on every boot.

## Deliverables

1. `boot-select` C binary (SPI window + picker; ARM64 cross-compile in the
   existing Docker build).
2. Entrypoint split: `shim-entrypoint.sh` keeps its name and stays what
   `/opt/move/Move` execs, but its old tail (services + LD_PRELOAD exec) moves
   out to a new `/data/UserData/schwung/schwung-entry.sh`, which is what
   `schwung/boot.json`'s `exec` field points at. `shim-entrypoint.sh` itself
   becomes the thin selector described below.
3. `install.sh` + `schwung-heal` updates: install/mirror the selector and the
   new `schwung-entry.sh`, ship `host/boot_target_lib.sh` and `bin/boot-select`
   in the payload, and assert/chmod them. **The registry itself is not created
   by the installer** — `schwung/boot.json` is self-registered by the selector
   at every boot (see Registry layout), so there is nothing here to migrate;
   an existing `default` file (or its absence, which resolves to `schwung`) is
   left alone.
4. `uninstall.sh`: restore stock `/opt/move/Move`, remove the registry.
5. Shim: touch `healthy` after ~30 s of continuously clocked SPI frames (a
   first-frame touch would mark a boot healthy even when a restored module
   crashes seconds later — the historical boot-loop case the watchdog exists
   for).
6. `docs/BOOT_TARGETS.md` — the target-author doc (written alongside this
   spec; the .md djhardrich asked for).

## Testing

- Target resolution, watchdog counting/clearing, and failure fallthrough
  factored into sourceable shell functions, covered by `tests/host/*.sh`
  (same pattern as the features.json merge test — the logic is lifted out of
  the entrypoint and run on the host, not restated).
- `boot-select`'s SPI frame parsing (Back press-edge detection, jog decoding)
  unit-tested on the host as compiled C tests.
- On-hardware verification: normal boot window, Back → picker, target switch,
  forced-picker after two induced crashes, factory-reset safety net.

## Decisions log (from the design conversation)

- Interrupt window over a hidden key combo: same SPI cost, discoverable,
  doubles as boot feedback. ~2 s added to boot, accepted.
- Back is the interrupt button (single mechanical button; capacitive
  knob-touch notes are too noisy for "any button").
- Window shows on **every** boot — there are always ≥2 targets (Schwung,
  Stock Move).
- Watchdog: opt-in health file first, liveness timer fallback.
- Stock is stock: no sidecars under non-Schwung targets; V rolls its own web
  presence and may launch schwung-manager itself.
- Selector lives in Schwung; Schwung's installer is the sole writer of
  `/opt/move/Move`.
- Picker selection **sets the default** (no boot-once mode).
