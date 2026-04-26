# Resume prompt for dual-MoveOriginal next session

Paste this into a fresh Claude session. It's self-contained — assumes
no memory of prior conversations.

---

I'm resuming a dual-MoveOriginal experiment on the Schwung framework.
The branch is `dual-move-instances` in `/Volumes/ExtFS/charlesvestal/
github/schwung-parent/schwung` (also reachable at `/Users/charlesvestal/
github/schwung-parent/schwung` via filesystem alias — same inodes).

## What you need to read first

In this exact order:

1. `docs/dual-move-progress.md` — running progress / hand-off doc.
   Tells you what's done, what works, what's deferred, and the
   operational gotchas.
2. `docs/dual-move-spi-broker-design.md` — the implementation-ready
   design for the next phase (SPI broker in `src/schwung_shim.c`).
3. `docs/dual-move-poc.md` — the proof-of-concept results that justify
   the design.
4. `docs/dual-move-recon.md` — Phase 0 reconnaissance findings.
5. `CLAUDE.md` (root) — project overview, build commands, code style.
6. `docs/REALTIME_SAFETY.md` — what you cannot do in the SPI callback.
7. `docs/SPI_PROTOCOL.md` — SPI buffer layout for the broker work.

## Current state

- Branch HEAD: `43363153 poc: dual-MoveOriginal feasibility proven —
  only SPI broker left`
- The user has uncommitted WIP in `src/schwung_shim.c` (~89 lines, an
  XMOS SysEx logger). **Ask the user how they want to handle it before
  modifying the shim.** Options: (a) they commit it first, (b) we work
  alongside it, (c) they stash it. Don't make this decision for them.
- `git status` will also show several other uncommitted user WIP files
  (`schwung-manager/*`, `src/host/link_audio.*`, etc.) — none of those
  are mine. **Never `git add -A` or `git commit -a`.** Always stage by
  exact path.
- The user previously had a destructive-git incident with prior
  Claude. Treat reset, rebase, cherry-pick, amend as red-flag commands
  that need explicit approval. See user memory `feedback_never_reset_hard`.

## What was proven (in one paragraph)

A second `MoveOriginal` boots successfully on the device given (a) a
per-instance `dbus-daemon` on `unix:abstract=move-b-bus`, (b) the
`DBUS_SYSTEM_BUS_ADDRESS` env var pointing at it, and (c) `/dev/
ablspi0.0` not held by stock. All 8 `com.ableton.move.*` D-Bus services
register on the private bus, audio engine starts, XMOS state reads.
The only thing blocking *simultaneous* dual-instance operation is SPI
device contention. Verify by running `bash scripts/dual-move-launch.sh
launch-b` (warns + prompts before killing stock; auto-restores on exit).

## What you should do this session

The next concrete piece of work is the SPI broker in
`src/schwung_shim.c`. The full design is in `docs/dual-move-spi-broker-
design.md`. Approach:

1. **Read all the listed docs first.** Don't skip. Especially
   `REALTIME_SAFETY.md` — `schwung_shim.c` runs at SCHED_FIFO 90 on
   core 3, ~900µs budget per frame. No file I/O, no allocations, no
   locks the non-RT side holds.
2. **Coordinate with the user about their `src/schwung_shim.c` WIP**
   before touching the file. Don't over-plan; ask what they prefer.
3. **Implement in vertical slices**, each independently testable on
   hardware:
   - Slice 1: A-mode (broker) reads B's TX SHM and sums audio. With no
     B running, broker behavior is byte-identical to today (verify by
     diffing audio output).
   - Slice 2: B-mode shim (client). Intercepts `open("/dev/ablspi0.0")`
     and `ioctl`; round-trips via SHM. Verify second instance boots
     and audio reaches DAC summed with stock.
   - Slice 3: focus flag in `/schwung-control` + display/LED/MIDI-in
     gating in A-mode broker. Toggle the flag manually via a small
     test tool first; visual confirmation on hardware.
   - Slice 4: hardware shortcut (Shift+Vol+Step1) wires the toggle.
   - Slice 5: LED state cache + replay on switch (so LEDs don't stay
     stuck at the previous instance's state).
4. **Smoke test on device after each slice.** Use `scripts/dual-move-
   launch.sh launch-b` (or extend it to launch B with
   `MOVE_INSTANCE_ROLE=client` once Slice 2 is in).
5. **Keep commits small and per-slice.** Verify `git status` clean of
   unintended files after every commit. Use `git log -1 --stat` to
   double-check.

## Workflow guidance

- Use the `superpowers:test-driven-development` skill where applicable
  (the pure logic in the broker is unit-testable on the host even
  though the wrappers aren't).
- Use the `superpowers:verification-before-completion` skill before
  claiming any slice is done.
- Use the `superpowers:systematic-debugging` skill for any audio
  glitch / SPI underrun investigation.
- Don't dispatch subagents for the shim work itself. The realtime-
  critical context is too sensitive for fresh-context delegation.
  You can delegate test-script writing or doc updates to subagents
  if useful.

## Device details

- `ssh root@move.local` works (key-based, no password). Use root for
  ptrace / system-level operations.
- `ssh ableton@move.local` works for user-context launches (instance B
  must run as ableton, uid 1000, to match stock).
- BusyBox device — many GNU coreutils flags are unavailable. `head -3`
  doesn't work; use `head -n 3`.
- **Never write to `/tmp`** on the device. rootfs is full. Use
  `/data/UserData/schwung/...`.
- `dbus-monitor --address ADDRESS` (space, not equals).
- `busctl` is not installed; use `dbus-send` or `dbus-monitor`.

## What success looks like for this session

Either:

- **Slice 1 + Slice 2 landed and tested**: two MoveOriginal processes
  running simultaneously on the device, both producing audio (mixed,
  even if display/LEDs are still buggy). That's the headline win.
- **Slice 1 only**: broker code in place but no B yet. Audio output
  byte-identical to today verified. Set up for next session to land
  Slice 2.

If you get stuck or run out of context, write your progress into
`docs/dual-move-progress.md` and an updated resume prompt. Don't leave
work half-done across sessions.
