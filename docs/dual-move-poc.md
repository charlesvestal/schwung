# Dual-MoveOriginal PoC — what we proved

**Date:** 2026-04-27
**Branch:** `dual-move-instances`

## Result: PoC is feasible. Only one real blocker remains.

A second `MoveOriginal` process can run on a Move device — successfully
booting through D-Bus service registration, audio engine startup, XMOS
PowerState read, and initial-song lookup — given:

- Per-instance `dbus-daemon` on `unix:abstract=move-b-bus`
- `DBUS_SYSTEM_BUS_ADDRESS=unix:abstract=move-b-bus` in instance B's env
- Stock MoveOriginal not running (i.e., `/dev/ablspi0.0` not held)

The only hard blocker for *simultaneous* operation is **SPI device
contention** — `/dev/ablspi0.0` is single-owner. The schwung shim
already brokers it for one client; extending the broker to handle a
second is the real engineering work.

## Methodology

Tooling captured under `recon/` and `dual-move/`:

- `dual-move/instance-b.conf` — the per-instance bus config used
- `recon/move-b-success.log` — instance B's full boot log (stock killed)
- `recon/move-second-launch.log` — original raw launch (stock running) showing the original blockers

The experimental flow on the device:

```bash
# 1. Spawn per-instance bus
dbus-daemon \
  --config-file=/data/UserData/schwung/dual-move/instance-b.conf \
  --fork --print-address
# Prints: unix:abstract=move-b-bus,guid=...

# 2. Stop stock (frees /dev/ablspi0.0). Suspend launcher first so it
#    doesn't auto-restart. SIGKILL still triggers the crash-display path
#    when launcher resumes — see "MoveLauncher crash detection" below.
kill -STOP $(pgrep -f /opt/move/MoveLauncher)
kill -9    $(pgrep -f /opt/move/MoveOriginal)

# 3. Launch instance B
DBUS_SYSTEM_BUS_ADDRESS=unix:abstract=move-b-bus \
  /opt/move/MoveOriginal > /tmp/move-b.log 2>&1

# 4. Restore stock when done
kill -9 $(pgrep -f /opt/move/MoveLauncher)
/etc/init.d/move start
```

## What instance B did successfully

From `recon/move-b-success.log`:

- Wi-Fi DBus errors (connman not on private bus) — logged, recovered.
- Hostname errors (`com.ableton.system`, Avahi missing) — logged, recovered.
- **All 8 `com.ableton.move.*` D-Bus services registered** on the private bus:
  Browser, Performance Recording, Screen Reader, SSH Keys, Song Renderer,
  Settings, Web Service Authentication, Cloud Authentication.
- "Unable to register successful system startup" (`com.ableton.update`
  missing) — logged, recovered.
- Memory bump 121 MB → 256 MB (audio engine init).
- LedBinningCodes, PowerState read from XMOS via SPI.
- `frames-dropped:5` warnings — audio loop running, dropping frames as
  expected during warm-up.

## The original "FileExists" red herring

When stock MoveOriginal is running and we launch instance B with the
private bus, the launch log shows:

```
error: Couldn't start Move's D-Bus services ... [FileExists] Failed to request bus name (File exists)
error: Exception caught in SPI Connection: Can't open device
```

Deep tracing (dbus-monitor + strace sendmsg) revealed:

- Instance B's `RequestName(com.ableton.move)` on the private bus
  **succeeded** (NameAcquired :1.5).
- Only one RequestName was ever sent.
- The "FileExists" log entry comes 35 ms before the SPI error — and
  MoveOriginal does not exit on the D-Bus error. It exits on SPI.

So the D-Bus FileExists error appears to be a benign log artifact
from sd-bus / sdbus-c++'s init path when stock A is alive — possibly
from a name-monitor that detects names on the system bus and reports
EEXIST. Whatever its source, it does **not** block the second instance
from continuing. With stock killed, the FileExists message disappears
entirely (see `recon/move-b-success.log`).

## MoveLauncher crash detection (operational footnote)

When MoveOriginal exits abnormally (we used SIGKILL), MoveLauncher logs:

```
MoveLauncher: Move quit with code 9
MoveLauncher: Move quit with SIGKILL
MoveLauncher: Starting MoveMessageDisplay
```

…and starts `MoveMessageDisplay "Move crashed"`, which takes over the
display. MoveLauncher then exits. Recovery is `/etc/init.d/move start`,
which respawns MoveLauncher (which respawns MoveOriginal under
`schwung-shim.so`).

For *real* dual-instance operation we never want to kill stock — both
instances will run simultaneously, and the shim brokers SPI for both.

## Remaining work

1. **SPI broker for two MoveOriginals.** Extend `src/schwung_shim.c` to
   accept SPI ioctl calls from two MoveOriginal pids, mix audio TX,
   demux MIDI/display/LED to whichever is "active" (per a focus flag in
   `/schwung-control`). This is the only hard problem left.

2. **(Optional) D-Bus FileExists investigation.** Find the exact
   sdbus-c++ code path that emits the spurious FileExists when stock A
   is alive. Probably suppressible. Not needed for PoC since it's
   non-fatal.

3. **(Optional) Per-instance peer services.** If any of `SystemDBusService`,
   `UpdateDBusService`, `MoveWebService` need per-instance state,
   spawn per-instance copies on the private bus. Recon Pass B confirmed
   MoveOriginal *calls* these but the calls degrade gracefully — the
   PoC log shows ServiceUnknown errors and MoveOriginal continues.

## Reversibility

Everything in the PoC is fully isolated and reversible:

- `dual-move/instance-b.conf` lives under the repo (source) and
  `/data/UserData/schwung/dual-move/` (deployed) — neither path is in
  the system D-Bus search path.
- The per-instance dbus-daemon is a process. `kill`, gone.
- The second MoveOriginal is a process. `kill`, gone.
- No `/etc/`, init scripts, systemd units, or binaries touched.
- DFU is not needed.

The only operational hiccup is that killing stock triggers the
"Move crashed" display until `/etc/init.d/move start` is run again —
~5 seconds of recovery.
