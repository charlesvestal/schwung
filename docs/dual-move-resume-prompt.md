# Resume prompt for dual-MoveOriginal next session

Paste this into a fresh Claude session. It's self-contained — assumes
no memory of prior conversations.

---

I'm resuming a dual-MoveOriginal experiment on the Schwung framework.
The branch is `dual-move-instances` in `/Volumes/ExtFS/charlesvestal/
github/schwung-parent/schwung` (also reachable at `/Users/charlesvestal/
github/schwung-parent/schwung` via filesystem alias — same inodes).

## What you need to read first, in this exact order

1. `docs/dual-move-progress.md` — running progress / hand-off doc.
   The "Slice 1 status" section at the bottom is the most important
   part — it tells you what landed, what regressed, and the suspect
   list for the audio-glitch root cause.
2. `docs/dual-move-spi-broker-design.md` — the implementation-ready
   design for the broker. Slice 1 is partially done; Slices 2-5
   pending.
3. `docs/dual-move-poc.md` — proof-of-concept results that justify
   the design.
4. `docs/dual-move-recon.md` — Phase 0 reconnaissance findings.
5. `CLAUDE.md` (root) — project overview, build commands, code style.
6. `docs/REALTIME_SAFETY.md` — what you cannot do in the SPI callback.
7. `docs/SPI_PROTOCOL.md` — SPI buffer layout for the broker work.

## Current state

- The branch tip on `dual-move-instances` includes Slice 1 — A-mode
  broker reads `/schwung-move-b-tx`, sums audio, gated by stale-seq
  detection so it's a no-op when no B is running.
- **Slice 1 caused user-reported audio issues on hardware (SPI-related).**
  The user reverted the device to `main` to keep it usable. Slice 1 code
  is committed on the branch but **not currently deployed**.
- The "Open issue" subsection in `docs/dual-move-progress.md` lists the
  suspect list. **Read it before opening the editor.** The leading
  hypothesis is first-touch page faults inside the SPI callback.
- `git log dual-move-instances --oneline -10` shows the recent work,
  including the XMOS SysEx logger commit and the Slice 1 commit.

## What to do this session

**Diagnose and fix the Slice 1 audio glitch BEFORE writing any new
slice code.** Order:

1. Read all the listed docs first. Don't skip. Especially
   `REALTIME_SAFETY.md` — `schwung_shim.c` runs at SCHED_FIFO 90 on
   core 3, ~900µs budget per frame. No file I/O, no allocations, no
   locks the non-RT side holds.
2. Re-deploy the `dual-move-instances` Slice 1 build to the device.
3. Verify the regression reproduces — check the shim timing logs
   (`spi_section_*` counters) to confirm a spike correlates with the
   new audio-sum block.
4. If it's first-touch page faults (the leading hypothesis): pre-fault
   `move_b_tx_shm->payload` and `move_b_rx_shm->payload` at init in
   `shim_init_subsystems` (memset(0) the whole 768-byte payload,
   `mlock` the mappings). Re-test.
5. If it's something else: read the timing log to localize, then fix.
6. Only when audio is byte-identical to `main` (no glitches) should
   you move on to Slice 2.

After Slice 1 is clean, slices 2-5 are:

- Slice 2: B-mode shim (client). Intercepts `open("/dev/ablspi0.0")`
  and `ioctl`; round-trips via SHM. Verify second instance boots and
  audio reaches DAC summed with stock.
- Slice 3: focus flag in `/schwung-control` + display/LED/MIDI-in
  gating in A-mode broker. Toggle the flag manually via a small
  test tool first; visual confirmation on hardware.
- Slice 4: hardware shortcut (Shift+Vol+Step1) wires the toggle.
- Slice 5: LED state cache + replay on switch (so LEDs don't stay
  stuck at the previous instance's state).

## Workflow guidance

- Use the `superpowers:systematic-debugging` skill for the audio-glitch
  investigation. **Don't guess** — instrument first, look at numbers,
  then hypothesize.
- Use the `superpowers:test-driven-development` skill where applicable
  (the pure logic in the broker is unit-testable on the host even
  though the wrappers aren't).
- Use the `superpowers:verification-before-completion` skill before
  claiming any slice is done.
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
  doesn't work; use `head -n 3`. `od -A` doesn't exist; use `hexdump`.
- **Never write to `/tmp`** on the device. rootfs is full. Use
  `/data/UserData/schwung/...`.
- `dbus-monitor --address ADDRESS` (space, not equals).
- `busctl` is not installed; use `dbus-send` or `dbus-monitor`.

## What success looks like for this session

Either:

- **Slice 1 audio regression root-caused and fixed**: deploy
  `dual-move-instances` to device, audio output is glitch-free and
  byte-identical to `main`. That's the headline win for this session;
  it unblocks slices 2-5.
- **Diagnostics narrow the cause but no fix yet**: timing logs
  attached to the progress doc, hypothesis updated, next session can
  pick up with concrete evidence.

If you get stuck or run out of context, write your progress into
`docs/dual-move-progress.md` and an updated resume prompt. Don't leave
work half-done across sessions.
