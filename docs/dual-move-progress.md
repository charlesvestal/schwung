# Dual-MoveOriginal — progress as of 2026-04-27

Branch: `dual-move-instances`. Latest commit at last update: see `git log
dual-move-instances -- docs/dual-move-progress.md src/schwung_shim.c
src/host/shadow_constants.h`.

This is the running progress / hand-off doc for the "two MoveOriginal
instances on one device" experiment. Read alongside:

- `docs/plans/2026-04-27-dual-move-instances.md` — original plan (mostly
  superseded by what we actually learned).
- `docs/dual-move-recon.md` — Phase 0 recon (Pass A idle + Pass B
  interactive, both as root via ssh).
- `docs/dual-move-poc.md` — proof-of-concept results.
- `docs/dual-move-spi-broker-design.md` — design for the next session's
  shim work.

## What we proved

A second `/opt/move/MoveOriginal` can run on the device today, given:

1. A per-instance `dbus-daemon` running on `unix:abstract=move-b-bus`
   (config: `dual-move/instance-b.conf`).
2. `DBUS_SYSTEM_BUS_ADDRESS=unix:abstract=move-b-bus` set in the
   instance-B environment.
3. `/dev/ablspi0.0` is free (i.e. stock A is stopped).

Under those conditions instance B logs:

- Successful registration of all 8 `com.ableton.move.*` services on the
  private bus (Browser, PerformanceRecording, ScreenReader, SSHKeys,
  SongRenderer, Settings, WebServiceAuthentication, CloudAuthentication).
- ServiceUnknown errors for connman / `com.ableton.system` /
  `com.ableton.update` / Avahi — recovered, MoveOriginal continues.
- Audio engine init, `frames-dropped` warnings (audio loop running),
  XMOS PowerState read, LED binning code read.

`recon/move-b-success.log` is the reference log.

## What's blocking simultaneous operation

Exactly one thing: **`/dev/ablspi0.0` is single-owner.**

`schwung-shim.so` (loaded into stock MoveOriginal via LD_PRELOAD) is the
sole broker for SPI right now. To run two MoveOriginals at the same
time, we need to extend the shim to broker SPI for two clients, plus
gate display / LEDs / MIDI-in by a "focused instance" flag.

The "FileExists" D-Bus error that appears when stock is alive is a red
herring — non-fatal, MoveOriginal continues past it. Investigating its
source is optional polish, not on the critical path.

## What's committed on the branch

```
43363153  poc: dual-MoveOriginal feasibility proven — only SPI broker left
5550ee5e  recon: raw second MoveOriginal launch — D-Bus + SPI blockers confirmed
90d4a92d  move-mux-shim: wrap remaining path-taking syscalls (Task 1.3)
89802b1b  move-mux-shim: LD_PRELOAD wrappers for open/openat/stat/lstat/access
7ecd8968  move-mux-shim: relative include in test for IDE resolution
d4aeb867  move-mux-shim: pure path-remap logic with unit tests
7229b0d7  recon: pass B interactive trace — UserData writes + D-Bus traffic
8282bc0a  recon: full strace + /proc capture as root (unblocks Phase 0)
015a68fe  recon: capture MoveOriginal file/posix I/O for dual-instance feasibility
b3b50e6b  plan: dual MoveOriginal instances on one device
```

Notable artifacts:

- `src/move_mux_shim.{h,c}` + `tests/host/test_move_mux_shim.{c,sh}` +
  `scripts/build-move-mux-shim.sh` — a complete LD_PRELOAD path-remap
  shim with 21 syscall wrappers and 14 host-runnable unit tests. Built
  early in the plan but **not load-bearing** for the PoC: we pivoted to
  shared UserData (UUIDs prevent set-file collisions; `Settings.json`
  torn-write risk is acceptable in practice). Kept as insurance.
- `dual-move/instance-b.conf` — the per-instance D-Bus daemon config.
- `scripts/dual-move-launch.sh` — wraps the spawn-bus + launch-B flow.
  Idempotent. `restore` subcommand revives stock via `/etc/init.d/move
  start`.
- `recon/` — proc snapshots, strace logs (most ignored via `.gitignore`),
  D-Bus monitor traces from the deep investigation.

## Architectural decisions made

1. **Shared UserData.** Both instances see the same `/data/UserData/`.
   UUIDs in `Sets/<uuid>/...` and `Sentry/<uuid>.run.*` prevent natural
   collisions. `Settings.json` is the one shared mutable file with a
   torn-write risk; rare in practice (preferences don't change during
   play).
2. **One MoveWebService is enough.** Don't run a per-instance copy.
   Web UI may only reach instance A; that's fine for PoC.
3. **Per-instance D-Bus is mandatory.** `/etc/dbus-1/system.d/move.conf`
   hardcodes the three `com.ableton.{move,update,system}` names —
   name-suffixing on a single bus is rejected by the policy daemon.
   Solution: separate `dbus-daemon` per instance bound to a different
   abstract socket. Done.
4. **Instance B runs as `ableton` (uid 1000)** — same as stock. Verified
   via `/proc/<pid>/status`. `move.conf` policy permits both root and
   ableton to own the names anyway, but parity matters less than
   matching stock behavior.
5. **Path-remap shim (`move-mux-shim.so`) is OPTIONAL.** Built but not
   used by the PoC. Could re-introduce per-instance UserData later
   without rewriting it.

## Operational gotchas

1. **Killing stock triggers MoveLauncher's crash display.** Sequence:
   SIGKILL `MoveOriginal` → MoveLauncher logs `Move quit with SIGKILL`
   and starts `MoveMessageDisplay "Move crashed"`. MoveLauncher then
   exits. Recovery: `/etc/init.d/move start`. Captured by
   `scripts/dual-move-launch.sh restore`. The real dual-instance design
   will never kill stock — both will run alongside under the brokered
   shim.
2. **`busctl` is not installed on the device.** Use `dbus-send` or
   `dbus-monitor`. `dbus-monitor` syntax is `--address ADDRESS` (space,
   not equals).
3. **`head -3` is rejected by BusyBox.** Always use `head -n 3`.
4. **Don't write to `/tmp` on the device** (rootfs is full). Use
   `/data/UserData/schwung/...`.
5. **`ssh root@move.local` works** (key-based, no password). Use root
   for ptrace / `/proc/<pid>/maps` access; use `ableton` to run
   user-context commands like dbus-daemon and MoveOriginal.
6. **MoveLauncher caught SIGKILL of MoveOriginal even while STOPPED** —
   the SIGCHLD was delivered when launcher resumed. Don't rely on STOP
   to keep launcher quiescent for long — the cleanest dual-instance
   design will have the launcher running both.

## What's deferred to next session

Per `docs/dual-move-spi-broker-design.md`:

1. Extend `src/schwung_shim.c` (currently 6,785 LOC) to broker
   `/dev/ablspi0.0` for two MoveOriginal pids. Per-pid TX SHM, sum
   audio mailboxes, demux MIDI-in / display chunks / LED packets by an
   `active_move_instance` flag in `/schwung-control`.
2. Wire a hardware shortcut to flip `active_move_instance`. Recon doc
   suggested **Shift+Vol+Step1** (Step 2 is Global Settings, Step 13 is
   Tools; Step 1 is unused).
3. Smoke test on device: launch both, verify no SPI underruns under
   load, verify focus switching is clean (no flicker / stale frame).

The shim work is realtime-sensitive (SCHED_FIFO 90 on core 3, ~900µs
budget per frame). Audit any new code path on the SPI callback hot
path. See `docs/REALTIME_SAFETY.md`.

The user has uncommitted WIP in `src/schwung_shim.c` (~89 lines, an XMOS
SysEx logger for jack-detect investigation). Resolve that — commit, stash,
or accept and rebase — before doing the broker surgery. **(Resolved
2026-04-27: committed as `shim: XMOS SysEx logger for jack-detect
investigation`.)**

## Slice 1 status (2026-04-27, parked)

**Landed on `dual-move-instances`** — see commits at the tip of the branch:

- Header: `src/host/shadow_constants.h` adds `SHM_MOVE_B_TX`, `SHM_MOVE_B_RX`,
  `MOVE_B_SHM_SIZE = 776`, `schwung_move_tx_shm_t`, `schwung_move_rx_shm_t`,
  `active_move_t` enum, `shadow_control_t::active_move_instance` (one byte
  stolen from existing `reserved[2]`; total stays 64 → size-check still
  passes).
- Shim (`src/schwung_shim.c`):
  - Role detection from `MOVE_INSTANCE_ROLE` env (default A/broker).
  - `shm_open(SHM_MOVE_B_{TX,RX}, O_CREAT|O_RDWR, 0666)` + `ftruncate(776)`
    + `mmap` in `shim_init_subsystems` next to the other shadow SHM.
  - End-of-`shim_pre_transfer` audio sum: read B's TX `seq`; if stale ≥2
    frames, no-op. Otherwise saturating-add 256 int16s from
    `payload[AUDIO_OUT_OFFSET..]` into the mailbox.
  - Inserted right BEFORE the `mute_move_audio` memset so muting Move
    also mutes B's contribution.
  - File-scope statics: `move_role`, `move_b_{tx,rx}_shm`,
    `move_b_last_seq`, `move_b_stale_frames`.
  - Compile-time size-checks pass (`MOVE_B_SHM_SIZE == sizeof(...)`).
- Build: cross-compiles cleanly with `./scripts/build.sh`.
- Deploy: `./scripts/install.sh local --skip-modules --skip-confirmation`
  succeeded; `/dev/shm/schwung-move-b-{tx,rx}` exist at 776 bytes; A's
  log emits `dual-move role = A (broker)` (printf, captured by
  MoveLauncher logs not unified logger).
- Verified single-instance (no B running): `seq=0` reads correctly,
  stale-frame counter trips on frame 2.

### Open issue — audio glitches with Slice 1 enabled

User reports audio works but there are SPI-related audio issues with the
Slice 1 build deployed. Parked the work and reverted the device to `main`
to keep the device usable. Suspects (not investigated yet):

1. **First-touch page faults on the new SHM pages.** On the first ~2 SPI
   frames, reading `move_b_tx_shm->payload + AUDIO_OUT_OFFSET` walks
   pages that were ftruncate'd but never faulted in. Page-fault inside
   the SPI callback (FIFO 90, ~900µs budget) is RT-unsafe.
   - Fix: pre-fault the payload at init with a single `read` /
     `memset(0)` of the whole region (in `shim_init_subsystems`, NOT
     in the callback), and `mlock` the mapping.
2. **Cache pollution of the audio inner loop.** The new 512-byte SHM
   mailbox plus existing mailbox doubles the working set of the
   pre-transfer audio path. Probably small, but worth profiling once
   page-faults are ruled out.
3. **`volatile` read of `seq` is not a real acquire fence on aarch64.**
   Even with no B running, the seq=0 path matches and we still execute
   the saturating-add over 256 int16s of zeros for the first 2 frames.
   That alone shouldn't glitch audio, but might combine with #1.
4. **Background `link_in_attach_retry_thread` and TTS init noise.** Not
   from Slice 1, but worth checking timing logs to confirm the spike is
   from the new code path and not a coincident init storm.

Diagnostic step before resuming: enable shim timing logs (the
`spi_section_*` counters in `schwung_shim.c`) and confirm whether the
spike correlates with the new audio-sum block.

## Slices 2-5

Unchanged from the design doc. Don't begin until Slice 1's audio
behavior is byte-identical to today (or the regression is root-caused
and fixed).

## Quick references

- Branch: `dual-move-instances`
- Run the PoC: `bash scripts/dual-move-launch.sh launch-b` (interactive,
  prompts before killing stock; auto-restores on exit).
- Status: `bash scripts/dual-move-launch.sh status`
- Bus address: `unix:abstract=move-b-bus`
- Bus config: `dual-move/instance-b.conf`
- Successful boot log: `recon/move-b-success.log`
- Failure-mode log (stock alive): `recon/move-second-launch.log`
- Existing shim: `src/schwung_shim.c` (lines 4090, 5030 for current XMOS
  WIP; lines 60–80 for SPI handle declarations)
- Realtime safety rules: `docs/REALTIME_SAFETY.md`
- SPI buffer layout: `docs/SPI_PROTOCOL.md`
