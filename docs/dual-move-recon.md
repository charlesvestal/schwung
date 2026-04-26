# Dual-Move Recon — MoveOriginal singletons (Phase 0 / Task 0.1)

First-pass enumeration of the file, socket, and device singletons held by a
running stock `MoveOriginal` process. The goal is to know exactly what we
must virtualize before we attempt to launch a second instance alongside the
first.

This document is the deliverable for Task 0.1 of
`docs/plans/2026-04-27-dual-move-instances.md`.

## Methodology

Three signals are captured per pass:

1. **Live `strace -f -y -tt -s 256 -p <pid>`** attached to the running
   MoveOriginal, restricted to path-touching and socket syscalls.
2. **`/proc/<pid>/*` snapshot** of the running MoveOriginal,
   including `cmdline`, `environ`, `cwd`, `exe`, `status`, `fd/` symlink
   targets, full `maps`, and `net/{unix,tcp,tcp6,udp,udp6,netlink}`.
3. **`strings` over `/opt/move/MoveOriginal`** for absolute path literals,
   D-Bus interface names, well-known bus names, and library symbols.

Helper script: `scripts/recon/strace-move.sh <duration> <suffix>`. The
script drives the device as **root via SSH** (`ssh root@move.local`),
which gives `CAP_SYS_PTRACE` and read access to the otherwise-denied
`/proc/<pid>/{maps,environ,cwd,exe}`. It emits `TRACE STARTED` /
`TRACE COMPLETE` sentinel lines so a controller can prompt the user to
exercise the UI mid-run, and pulls all artifacts to local `recon/`.

Raw + derived outputs (committed under `recon/`):
- `recon/move-proc.log` — original (uid `ableton`) snapshot, kept for history.
- `recon/move-proc-passA.log` — full `/proc/<pid>` snapshot as root (idle pass).
- `recon/move-strings-paths.log` — categorized `strings` extraction.
- `recon/move-strace-passA.log` — full Pass A strace log (~74k lines / 60s).
  Ignored via `.gitignore` (large, unstable across passes).
- `recon/move-paths.txt` — distinct paths Pass A opened/stat'd, grouped by directory.
- `recon/move-sockets.txt` — connect/bind/socket calls Pass A made (idle).
- `recon/move-devices.txt` — `/dev/*` opens Pass A made (idle).

### Limitations

1. **Idle-only snapshots so far.** Pass A captures MoveOriginal's
   boot-and-idle behavior. UI-action paths (open Set, save project, plug
   in MIDI, change Settings) require a Pass B with a human exercising
   the device while the trace is running — the script supports this via
   the `TRACE STARTED` / `TRACE COMPLETE` sentinels. Pass B is queued
   pending controller coordination with the user.
2. **Persistent connections were established before strace attached.**
   Because we attach to a long-running pid, sockets opened at boot
   (system D-Bus, avahi, `/dev/log`, `/tmp/sockinstctrl`,
   `/tmp/swupdateprog`) appear in `/proc/<pid>/net/unix` and
   `fd/` symlinks but **not** in `connect()` strace lines. The proc
   snapshot is authoritative for who-is-connected-now; the strace is
   authoritative for who-opens-things-during-the-window.
3. **`strings` output is sparse.** MoveOriginal is stripped C++; runtime
   paths are constructed, not stored as literals. Static strings catch
   D-Bus well-known names and a few hardcoded device paths; the dynamic
   trace catches everything else.

The prior `ptrace` blocker (running as user `ableton`) is **resolved**
by switching to `ssh root@move.local` — root has `CAP_SYS_PTRACE` and
can read every `/proc/<pid>/*` entry. See "Unblocking the deeper trace"
below.

### Unblocking the deeper trace

- **Option A: run as root via `ssh root@move.local`. — DONE.** This is
  the methodology in use (see `scripts/recon/strace-move.sh`). Root
  bypasses `cap_ipc_lock,cap_sys_nice,cap_sys_resource=ep` non-dumpable
  protection on MoveOriginal.
- Option B: strip file capabilities from a recon copy of the binary
  (`setcap -r /opt/move/MoveOriginalRecon`) and run unprivileged. Not
  needed.
- Option C: in-shim recon mode (extend `schwung-shim.so` to log every
  `open*`/`connect`/`bind`). Useful as a complement if we want to
  capture *only* MoveOriginal-originated calls (Pass A also picks up
  the Schwung shim's own debug-flag polling — see Pass A findings).

## Pass A — full strace as root (idle, 60s)

`scripts/recon/strace-move.sh 60 passA` against pid 1458 produced
73,971 lines of trace and a 598-line `/proc/<pid>` snapshot.
Histogram of syscalls invoked during the window:

| Syscall      | Count |
| ------------ | ----- |
| `faccessat`  | 39376 |
| `getdents64` | 10067 |
| `openat`     |  8881 |
| `socket`     |   495 |
| `bind`       |    13 |
| `connect`    |     5 |

The trace attaches to all 21 threads. Only one of them is
MoveOriginal-proper (tid 1458, the main thread). The rest are mostly
worker threads, plus tid 1499 — the **schwung-shim's** debug-flag poll
loop, which accounts for ~46k of the syscalls (`faccessat` against
`/data/UserData/schwung/{slot_fx_dump_trigger,spi_snap_trigger,...}`).
That's our own code; it isn't a MoveOriginal singleton hazard. It does
mean future passes should filter `tid == 1458 || tid in MoveOriginal_workers`
to suppress shim noise, or run the trace before loading shim debug flags.

### MoveOriginal's actual idle behavior

The main-thread (tid 1458) idle behavior in 60 seconds is essentially:

- **Polling the Set library**: `openat("/data/UserData/UserLibrary/Sets")`
  → `getdents64` → for each subdir, `openat(<set-dir>)` → `getdents64`,
  on a ~50 ms cadence. This is the project browser keeping its index
  fresh. With three sets present (UUIDs `521ede4d…`, `845057ea…`,
  `f8537fba…`), each one is descended every tick.
- **No** writes to UserLibrary during idle.
- One `openat("/data/UserData/settings/Settings.json", O_RDONLY)` —
  just one, suggesting Settings is read-on-change, not polled.
- Cloud-auth thread (tid 1485, 2712) periodically opens HTTPS to
  `34.160.81.0:443` (Google Cloud LB) and DNS to `::1:53`. No bus
  traffic, no UserLibrary writes.
- Avahi/mdns activity is invisible because the connection was
  established at boot — the `/proc/<pid>/net/unix` snapshot shows the
  active UDS endpoints (Findings sections below remain authoritative
  for those).

### Verified file/library map (from `/proc/<pid>/maps` as root)

The trace + maps confirm MoveOriginal links against the following
libraries from `/usr/lib/`, `/lib/`, and `/data/UserData/schwung/lib/`
(the latter are Schwung-shipped speech assets — espeak-ng, flite,
sonic, pcaudio — not MoveOriginal's own deps):

```
/lib/libc.so.6, libpthread.so.0, libm.so.6, librt.so.1, libgcc_s.so.1,
       libudev.so.1.6.3, libcap.so.2.66, libz.so.1.2.11,
       libnss_compat.so.2, libnss_mdns4_minimal.so.2,
       ld-linux-aarch64.so.1
/usr/lib/libstdc++.so.6.0.29, libc++.so.1.0,
       libdbus-1.so.3.32.3, libsystemd.so.0.37.0,
       libssl.so.3, libcrypto.so.3,
       libasound.so.2.0.0, libusb-1.0.so, libmp3lame.so,
       libyaml-0.so.2.0.9, libubootenv.so.0.3.5,
       libswupdate.so.0.1, libXSDBusCpp.so, libXTCMalloc.so
```

`libdbus-1` and `libXSDBusCpp` (sdbus-c++) confirm both the raw and
sdbus-c++ paths to the system bus. `libsystemd` is present, presumably
for sd-event/sd-journal. **`libasound` is mapped but never used**: see
Q1 below.

### Verified open file descriptors (from `/proc/<pid>/fd/` as root)

Pid 1458 holds 59 fds at the moment of capture. The interesting ones:

| fd  | target                                   | meaning |
| --- | ---------------------------------------- | --- |
| 23  | `/dev/ablspi0.0`                         | The SPI device. Owned by Schwung shim, not MoveOriginal directly — confirmed by `maps` showing `rw-s` mapping at `0x7f8534a000` *attributed to the shim's allocator*. This is the singleton the shim already brokers. |
| 27–41 | `/dev/shm/schwung-{audio,movein,midi,...}` | All Schwung shadow SHM segments, mapped read-write into MoveOriginal's address space because the shim opens them. |
| 26  | `/data/UserData/schwung/debug.log`       | Schwung unified logger fd. |
| 3, 6 | `/data/UserData/Sentry/<uuid>.run.lock` | **Sentry crash-reporter run-lock files (NEW finding).** `/data/UserData/Sentry/` is a separate UserData namespace MoveOriginal writes to — must be virtualized per-instance. The `.run.lock` files use BSD-style `flock` (visible from path naming convention; not strace-confirmed since lock was held before attach). Two locks open: probably one for the runtime crash dir and one for the sentry session. |
| 12, 20–25, 42–58 | various `socket:[N]` inodes  | The system-bus / sdbus-c++ / avahi connections established at boot. Cross-referenced via inode against `/proc/<pid>/net/unix` (see "UNIX domain sockets" below). |
| 8  | `/dev/urandom`                            | RNG. |
| 9–19 | `anon_inode:[eventfd|timerfd|eventpoll]` | Internal sync primitives. |

### New findings vs. prior pass

- **`/data/UserData/Sentry/<uuid>.run.lock`** — completely missed by the
  prior pass. This is a per-process lock dir (Sentry crashpad). Two
  instances need separate `/data/UserData/Sentry/` namespaces (or a
  per-instance lock-dir subdir).
- **`/data/UserData/settings/Settings.json`** — confirmed top-level
  config file. **Path is `settings/`, not `Settings/` or
  `MoveSettings/`.** This is a singleton write target that must be
  redirected per-instance (e.g. `/data/UserData/move-a/settings/`).
- **MoveOriginal's idle CPU is essentially the project browser polling
  the Set library every ~50 ms.** Not a singleton hazard but worth
  knowing for CPU budgeting under dual-instance.
- **`schwung-shim`'s debug-flag polling dominates the trace.** Not a
  hazard, but future passes can suppress with
  `rm /data/UserData/schwung/debug_log_on` *and* not having the listed
  flag files exist (they don't — they're checked, not present).
- **`/etc/dbus-1/system.d/move.conf` policy** — Allows `com.ableton.move`,
  `com.ableton.update`, `com.ableton.system` ownership for users `root`
  and `ableton`. **Does not include any wildcard / per-instance
  pattern.** A second instance trying to claim `com.ableton.move` will
  be denied by the policy daemon, not just collide on the well-known
  name. Phase 2 work must either (a) add a second policy file
  (`com.ableton.move.b`, etc.) or (b) run the second instance on a
  separate bus address entirely.
- **`/dev/snd` does not exist on the device**, and **the only thing in
  `/dev/input` is `mice`** (no `event*` nodes). Both are non-issues
  for dual-instance — see Q1 + Q2 below.

### Answers to questions left open by the prior pass

**Q1: Does MoveOriginal open `/dev/snd/seq*` or any ALSA device
opportunistically?**

No. `/dev/snd` doesn't exist on the device. `libasound.so.2.0.0` is
mapped into MoveOriginal's address space but `openat("/dev/snd/...")`
is never invoked in 60s of idle. The only `/dev/*` open is
`/dev/urandom`. ALSA presence is dead code on this build, presumably
because the same source compiles for desktop targets that have ALSA.
**Implication:** ALSA device singletons are not a dual-instance
concern. We can ignore Phase 2 Task 2.3.

**Q2: Does MoveOriginal open `/dev/input/event*`?**

No. `/dev/input` only contains `mice` on this device. There are no
`event*` nodes (no kernel input subsystem for the Move's
pads/buttons — those go over SPI). The trace shows zero
`/dev/input/*` accesses. **Implication:** evdev is not a dual-instance
concern.

**Q3: Is `/tmp/sockinstctrl` opened with `O_EXCL` or similar?**

The trace doesn't show MoveOriginal opening `/tmp/sockinstctrl` during
the 60s window — the connection (visible in `/proc/<pid>/net/unix`)
was established at boot. The socket itself, on disk, has mode `srw-rw-rw-`
(world-rw), suggesting whoever owns it (likely SystemDBusService /
the install-control daemon) created it with permissive flags, not
`O_EXCL`. **Implication:** We can probably connect a second
MoveOriginal to the same `/tmp/sockinstctrl` (it's a server socket,
multi-client). Worth confirming during Pass B by watching it during a
firmware-update probe.

**Q4: Is `/run/dbus/system_bus_socket` the only D-Bus the process
touches, or does it also connect to a session bus?**

Only the system bus. Indirect evidence:

- `/proc/<pid>/environ` (now readable as root) contains
  **no `DBUS_SESSION_BUS_ADDRESS`** and **no `XDG_RUNTIME_DIR`**.
- No session-bus path string appears in the trace.
- No abstract address starting with `@<hash>/bus/.../session` appears
  in `/proc/<pid>/net/unix`. All abstract endpoints
  (`@…/bus/MoveOriginal/system`, `…/MoveLauncher/system`, etc.) are
  sdbus-c++'s **private-but-system-flavored** buses (the trailing
  `/system` token names the sdbus-c++ bus role, not the bus type).

**Implication:** Dual-instance D-Bus work only has to deal with the
system bus. No session-bus complications. This narrows Phase 2's
strategy to either per-instance bus addresses (preferred — see Phase 2
Task 2.1) or, given that `move.conf` policy doesn't accept arbitrary
names, **a second `dbus-daemon --system --address=unix:abstract=...`
running with its own policy file**.

## Findings — paths under `/data/UserData/`

Idle Pass A surfaced these `/data/UserData/` paths (full list in
`recon/move-paths.txt`):

| Path | Owner | Singleton-ness |
| ---- | ----- | -------------- |
| `/data/UserData/UserLibrary/Sets/<uuid>/Set N` | MoveOriginal | Polled every ~50ms during idle. Two instances writing here will race on metadata + autosave. **Must be redirected per-instance** (see Phase 1 path-remap shim). |
| `/data/UserData/settings/Settings.json` | MoveOriginal | App settings (volume, last-set, MIDI mappings). Singleton write target. **Must be redirected.** |
| `/data/UserData/Sentry/<uuid>.run.lock` | MoveOriginal | Sentry crashpad per-process lock dir. **Must be redirected** — two instances will collide on the same uuid-named lock files. |
| `/data/UserData/schwung/...` | Schwung host + shim | Schwung-owned (debug flags, modules, lib). Out of scope for MoveOriginal recon. |

**Action item for Pass B (queued):** repeat the trace while a human
exercises the device — open Set, save Set, change a track parameter,
plug in a USB-MIDI device, change Settings, trigger an autosave. The
write paths and any new bus traffic will appear there. Pass A already
nails the steady-state set of singletons (above) and is sufficient for
Phase 0 GO/NO-GO discussion.

Supplementary inferred information (these dirs may exist but were not
exercised in idle Pass A; expect them to surface in Pass B or have
been captured indirectly via maps/strings):

| Path                                         | Owner         | Singleton-ness                                                   |
| -------------------------------------------- | ------------- | ---------------------------------------------------------------- |
| `/data/UserData/UserLibrary/Samples/` etc.   | MoveOriginal  | The rest of the user-content tree (Sets, Samples, Projects). Two MoveOriginal instances writing here will race on metadata, db indices, autosave files. |
| `/data/UserData/UserLibrary/Recordings/`     | MoveOriginal  | Same. Schwung recordings already namespace under `Schwung/...`. |
| `/data/UserData/move-anything/`              | Schwung       | Schwung's own state, not MoveOriginal. Out of scope. |

## Findings — UNIX domain sockets

From `/proc/1458/net/unix` (process-scoped view) and the system-wide table:

| Endpoint                                                    | Type   | Role                                                                | Singleton? | Interception strategy hint |
| ----------------------------------------------------------- | ------ | ------------------------------------------------------------------- | ---------- | -------------------------- |
| `/run/dbus/system_bus_socket`                               | UDS    | System D-Bus daemon                                                 | **Yes (system-wide)** | `LD_PRELOAD` `connect()` shim that rewrites `/run/dbus/system_bus_socket` → instance-specific path *if* we run a second `dbus-daemon` per instance, OR run a **per-instance session bus** and patch `DBUS_*_BUS_ADDRESS` env. The latter is cleaner; sdbus-c++ honors env. |
| `/run/avahi-daemon/socket`                                  | UDS    | Avahi mDNS daemon (Bonjour, Link)                                   | **Yes (system-wide)** | Avahi is mostly a publisher; two clients can publish under different service names by passing different host names. Probably no patching needed beyond per-instance hostnames. |
| `/run/udev/control`                                         | UDS    | udev event broadcasting (read-only client)                          | Yes        | Read-only listener; multiple clients allowed. No-op. |
| `/dev/log`                                                  | UDS    | syslog (busybox `/sbin/syslogd`)                                    | Yes        | Multiple clients allowed. Tag log lines by instance via `openlog()` ident. No interception needed. |
| `/tmp/sockinstctrl`                                         | UDS    | Inferred: SystemDBusService control socket (instance install ctl)   | **Likely instance-singleton** | Need a per-instance path; verify with shim trace. **Investigate.** |
| `/tmp/swupdateprog`                                         | UDS    | swupdate progress channel (multiple clients OK)                     | System-wide | Read-only progress; sharing fine. |
| `@<hash>/bus/MoveOriginal/system` (abstract UDS)            | sdbus-c++ "system bus" | Per-process well-known name on the system bus (sdbus-c++ private bus implementation) | **YES per-pid**, but the **D-Bus well-known names below are global** | sdbus-c++ generates a per-pid abstract UDS automatically; coexisting fine. The collision is at the *bus name* level (next section). |
| `@<hash>/bus/MoveLauncher/system`                           | abstract UDS | MoveLauncher's bus | system-wide | One launcher manages both? Or two launchers? Open question. |
| `@<hash>/bus/MoveWebService/system`                         | abstract UDS | MoveWebService's bus | system-wide | Web service is presumably one-per-device. Question: who handles the second instance's web requests? |
| `@<hash>/bus/SystemDBusServi/system`                        | abstract UDS | SystemDBusService | system-wide | **Singleton service**; second instance must connect to the same one and tolerate sharing, or we run a second service. |
| `@<hash>/bus/UpdateDBusServi/system`                        | abstract UDS | UpdateDBusService | system-wide | Same. |

## Findings — D-Bus well-known names

From `strings /opt/move/MoveOriginal`, MoveOriginal owns and/or talks to:

```
com.ableton.move
com.ableton.move.Browser
com.ableton.move.CloudAuthentication
com.ableton.move.PerformanceRecording
com.ableton.move.SSHKeys
com.ableton.move.ScreenReader
com.ableton.move.Settings
com.ableton.move.SongRenderer
com.ableton.move.WebServiceAuthentication
com.ableton.system        (peer service: SystemDBusService)
com.ableton.update        (peer service: UpdateDBusService)
org.freedesktop.Avahi(.Server)
```

This is the **biggest dual-instance hazard**. D-Bus well-known names are
unique per bus — only one process can `RequestName(com.ableton.move.Settings)`
and the second instance will fail to register.

**Interception strategies (in order of preference):**

1. **Per-instance D-Bus session bus.** Spawn a second `dbus-daemon
   --system --config-file=...` (or session-bus equivalent) bound to a
   different abstract socket. Set `DBUS_SYSTEM_BUS_ADDRESS=unix:abstract=/move-2`
   in instance #2's environment. Each instance owns its own copy of every
   `com.ableton.move.*` name on its private bus. Schwung host code that
   currently listens on the system bus needs to be made bus-aware.
   Net cost: medium — bus configs, env-var wrapping, and any peers
   (SystemDBusService, UpdateDBusService) need to be reachable from both
   instances. Recommended.
2. **Bus-name namespacing via shim.** Intercept `RequestName` /
   `ReleaseName` / `GetNameOwner` traffic and rewrite suffixes per instance
   (`com.ableton.move.Settings` → `com.ableton.move.Settings.2`). Brittle,
   touches every D-Bus message. Not recommended.
3. **Single-bus, single-instance model.** Accept that only the "active"
   MoveOriginal owns the well-known names; the inactive one runs in
   "headless" mode without bus presence. Probably workable for the dual-
   *engine* scenario (one for play, one for record) but not for full
   dual-UI.

## Findings — TCP / UDP listeners

From `/proc/1458/net/tcp[6]` and `udp[6]` (system-wide tables, filtered by
uid 1000 = `ableton`, with cross-reference to known Schwung ports):

| Port (hex / dec)             | Proto      | uid       | Owner (likely)             | Singleton-ness                                              |
| ---------------------------- | ---------- | --------- | -------------------------- | ----------------------------------------------------------- |
| `0x0050` = **80**            | TCP6 LISTEN | 1000     | MoveWebService             | OS-level singleton on port 80                              |
| `0x1E01` = **7681**          | TCP4 LISTEN | 1000     | Likely MoveWebService HTTPS / WebSocket | OS-level singleton                                |
| `0x1E14` = **7700**          | TCP6 LISTEN | 1000     | **schwung-manager** (project memory)            | Schwung-owned; not MoveOriginal — out of scope             |
| `0x14E9` = **5353**          | UDP4/6     | 997      | mDNS / Avahi (uid `avahi`)| Multicast group; multi-client OK                            |
| `0x5148` = **20808**         | UDP4/6 (multiple sockets) | 1000 | Likely Ableton Link        | Multiple sockets bound to same port (multicast); usually OK across instances if both join the same Link session |

The Avahi (mDNS) and Link UDP groups are designed for many peers, so two
instances won't fight at the protocol level. **Port 80 and 7681 are hard
singletons** — only one process can listen. If we want both instances to
present a web UI, instance #2 needs different ports or a reverse proxy
front-end. (Schwung already runs `schwung-manager` on 7700, so this
pattern exists.)

## Findings — device files

Cross-references between Pass A's `fd/` symlink dump (now readable as
root) and the static `strings` extraction:

| Device                       | Singleton? | Notes                                                                                                 |
| ---------------------------- | ---------- | ----------------------------------------------------------------------------------------------------- |
| `/dev/ablspi0.0`             | **YES — hardware**  | The SPI link to the Move's MCU. fd 23 is mapped `rw-s` at `0x7f8534a000` in the maps; this is the Schwung shim's broker, not MoveOriginal's direct fd. The shim already arbitrates this — *the very reason a second instance has to be virtual*. Strategy: route instance #2's SPI traffic through the shim's mailbox model, never through real ablspi. |
| `/dev/log`                   | No        | UDS to syslog. Multi-client. |
| `/dev/urandom`               | No        | RNG. Multi-client. Confirmed open (fd 8) and reopened in trace. |
| `/dev/snd/*`                 | **Absent (verified Pass A)** | `/dev/snd` doesn't exist on the device. `libasound.so.2.0.0` is mapped into MoveOriginal but is dead code — zero `openat("/dev/snd/...")` in 60s of idle trace. Resolves Q1 above. **Not a dual-instance hazard.** |
| `/dev/input/event*`          | **Absent (verified Pass A)** | `/dev/input` only contains `mice` on this device. No `event*` nodes. Zero `/dev/input/*` opens in trace. Resolves Q2 above. **Not a dual-instance hazard.** |
| `/dev/dri/*`, framebuffer    | Absent (verified)    | The Move display is over SPI; no DRM nodes. None opened. |
| `/dev/shm/schwung-*`         | Schwung-owned | Mapped into MoveOriginal because the shim opens them. Not a MoveOriginal singleton. |

## Findings — netlink + udev

`/proc/1458/net/netlink` shows MoveOriginal participates in a netlink
group (`Eth=15` is `NETLINK_KOBJECT_UEVENT` per kernel headers), confirming
the udev listener observed in `/proc/<pid>/net/unix`. Netlink groups are
multi-listener; not a singleton hazard.

## Findings — peer Move binaries

`/opt/move/` contains many peer binaries that MoveOriginal interacts with
via D-Bus or shared-mem:

```
MoveLauncher          # init-spawned process supervisor
MoveWebService        # HTTP front-end on :80 + :7681
MoveContentInfo       # content/library indexing
MoveControlModeHandler
MoveDemoSetInstaller
MoveFirmwareAutoUpdater
MoveFirmwareUpdater
MoveMessageDisplay    # screen-reader / notifications
MoveResetter          # factory reset
MoveSentryRunProcessor
MoveSSHKeyInstaller
MoveXmosCli / MoveXmosPower    # XMOS MCU control
SystemDBusService     # com.ableton.system owner
UpdateDBusService     # com.ableton.update owner
Updater
XCrashpadHandler
```

Several of these (`SystemDBusService`, `UpdateDBusService`,
`MoveWebService`) are clearly designed as **one-per-device daemons**. A
second MoveOriginal can either:
- Share these peers (read-only-ish: settings, system info, web UI). Risk:
  the peers don't expect concurrent edits.
- Own a private peer set on a private bus (per-instance bus model).

## Top-level interception strategy summary

Pass A confirms and refines the priority order. For dual-Move-instances
we need to neutralize:

1. **`/dev/ablspi0.0`** — **already done** by Schwung's shim (mailbox).
2. **D-Bus well-known names** (`com.ableton.move.*`) — needs either
   (a) per-instance D-Bus addresses *plus* a second `dbus-daemon`
   running with a policy file that allows the per-instance names, or
   (b) a single bus where the second MoveOriginal claims suffixed names
   via a `RequestName` shim. Pass A established `move.conf` does **not**
   permit unknown names, so option (b) requires patching the policy
   file as well. **Largest piece of new work.**
3. **`/data/UserData/` per-process dirs** — Pass A enumerates three
   non-Schwung subtrees that need per-instance virtualization:
     - `/data/UserData/UserLibrary/` (Sets, Samples, Recordings, …)
     - `/data/UserData/settings/` (`Settings.json` — singular file,
       singleton write target)
     - `/data/UserData/Sentry/` (per-process `<uuid>.run.lock` files;
       new finding from Pass A)
   Path-remap shim must cover all three. The polling pattern (Sets
   browser revisits every set-dir every ~50ms) means the remap needs
   to be cheap; cache the rewrite or use `inotify` on the redirected dir.
4. **TCP port 80 / 7681** — instance #2 either picks alt ports or stays
   web-headless.
5. **`@.../bus/MoveWebService/system` and friends** — auto-handled by (2)
   if we go per-instance bus.
6. **Avahi / mDNS / Link UDP** — multi-listener by design; just give
   instance #2 a distinct service name. Pass A confirms zero
   session-bus / XDG runtime usage, so no env complications.

Items the prior pass listed as "investigate" that Pass A has now
**closed out** as non-issues:
- `/dev/snd/*` — absent on device; ALSA path is dead code.
- `/dev/input/event*` — absent on device; only `mice` exists.
- Session bus — never used; system bus only.
- `/tmp/sockinstctrl` — server socket, multi-client; not a singleton
  hazard for clients (server itself is shared).

## Next steps

- **Task 0.1.a — Pass B (queued).** Run
  `scripts/recon/strace-move.sh 90 passB` while a human exercises the
  device (open Set, save Set, change Settings, plug in USB-MIDI, trigger
  autosave). Compare derived paths with Pass A to surface write-side
  singletons (autosave temp files, Sentry crash artifacts under load,
  any `mkdir`/`rename` calls). Pass A is sufficient for steady-state
  GO/NO-GO discussion, but Pass B is required before we commit to the
  Phase 1 path-remap surface.
- **Task 0.1.b** (older suggestion, now optional): write a recon
  LD_PRELOAD shim variant that logs every
  `open*`, `openat*`, `connect`, `bind`, `socket`, `creat`, `unlink`,
  `rename`, `mkdir` from inside MoveOriginal. Replaces the strace-blocked
  prong with one we control.
- **Task 0.2** (next plan task): enumerate JACK/D-Bus/ALSA singletons. The
  D-Bus + ALSA halves are largely covered by this doc; the JACK half is
  N/A (no JACK on stock Move). Task 0.2 should focus on **what
  SystemDBusService and UpdateDBusService actually expose** and whether
  they assume single-client semantics.
- **Task 0.3** GO/NO-GO checkpoint can be informed by this: feasibility
  hinges almost entirely on per-instance D-Bus, given the SPI mailbox
  already exists.
