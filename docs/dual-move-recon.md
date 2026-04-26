# Dual-Move Recon — MoveOriginal singletons (Phase 0 / Task 0.1)

First-pass enumeration of the file, socket, and device singletons held by a
running stock `MoveOriginal` process. The goal is to know exactly what we
must virtualize before we attempt to launch a second instance alongside the
first.

This document is the deliverable for Task 0.1 of
`docs/plans/2026-04-27-dual-move-instances.md`.

## Methodology

Three signals were captured:

1. **Live `strace -p <pid>` (60s, idle)** — *blocked, see "ptrace blocker" below*.
2. **`/proc/<pid>/*` snapshot** of the running MoveOriginal (pid 1458),
   focused on `net/unix`, `net/tcp[6]`, `net/udp[6]`, `net/netlink`,
   `status`, and `fd/`.
3. **`strings` over `/opt/move/MoveOriginal`** for absolute path literals,
   D-Bus interface names, well-known bus names, and library symbols.

Helper script: `scripts/recon/strace-move.sh` (idempotent; runs all three
prongs and tolerates the strace failure).

Raw outputs (pulled from device, except `move-strace.log` which is local-only):
- `recon/move-proc.log` — `/proc/1458` snapshot (committed)
- `recon/move-strings-paths.log` — categorized `strings` extraction (committed)
- `recon/move-strace.log` — empty (`strace: attach: ptrace(PTRACE_SEIZE, 1458): Operation not permitted`); ignored via `.gitignore`
- `recon/move-paths.txt` — distinct paths found, sorted unique
- `recon/move-sockets.txt` — UDS endpoints + abstract D-Bus per-process names

### Limitations of this pass

1. **No interactive UI exercise was possible.** The controller running this
   recon cannot physically touch the Move device. The trace was captured
   while MoveOriginal was idle (display refreshing, audio engine ticking,
   no project changes). A richer trace covering "open a Set / save a
   project / plug in MIDI" is a follow-up task; the singletons surfaced
   here are the boot-and-idle subset.
2. **`strace -p` failed.** MoveOriginal carries Linux file capabilities
   (`cap_ipc_lock,cap_sys_nice,cap_sys_resource=ep`), which marks the
   process non-dumpable. The kernel then refuses `ptrace()` from a tracer
   that lacks `CAP_SYS_PTRACE`, even when the tracer shares uid. As user
   `ableton` we cannot attach. The same restriction blocks reading
   `/proc/<pid>/{maps,environ,cwd,exe}` (those returned `Permission
   denied`).
3. **`strings` output is sparse.** `MoveOriginal` is stripped C++; most
   filesystem paths used at runtime are *constructed* (Qt-style joins,
   `XDG_DATA_HOME`-style env lookups, cloud-config inheritance) rather
   than stored as literals. We see `/dev/ablspi0.0` and a handful of
   `/proc/self/*` strings but very few `/data/UserData/*` paths.

### Unblocking the deeper trace

To replace the empty `move-strace.log` with a real one, any of these would
work (in order of effort):

- Run the script as root (e.g. via the schwung-manager root daemon, or a
  one-shot init.d service). Root has `CAP_SYS_PTRACE`.
- Strip file capabilities from a *recon copy* of the binary
  (`setcap -r /opt/move/MoveOriginalRecon`) and run that copy under
  `strace ./MoveOriginalRecon`. We'd lose `cap_sys_nice` (priority bumps)
  and `cap_ipc_lock` (mlock for realtime audio), but boot/idle file I/O
  still surfaces.
- Add a one-off LD_PRELOAD shim that logs every `open*`, `connect`,
  `bind`, `socket` and writes to `/data/UserData/schwung/recon/`. The
  Schwung shim already runs in MoveOriginal's address space; extending it
  with a recon mode is cheap.

The third option is the recommended next step — it sidesteps both the
ptrace gate and the missing source-builds, and we already own the shim.

## Findings — paths under `/data/UserData/`

Idle trace surfaced **none**. MoveOriginal didn't open any
`/data/UserData/...` file in the 60-second window we could see (limited
to the proc snapshot, since strace was blocked). This is expected for
*idle* — saved-set / project I/O happens on user action.

**Action item for Task 0.1 follow-up:** capture a richer trace via the
LD_PRELOAD recon shim, ideally during a "open Set → save → load → undo"
sequence.

What we *do* know about `/data/UserData/` interaction comes from already-
shipped Schwung knowledge (`CLAUDE.md` and `docs/SPI_PROTOCOL.md`):

| Path                                         | Owner         | Singleton-ness                                                   |
| -------------------------------------------- | ------------- | ---------------------------------------------------------------- |
| `/data/UserData/UserLibrary/`                | MoveOriginal  | The user content tree (Sets, Samples, Projects). Not a singleton in the OS sense, but two MoveOriginal instances writing here will race on metadata, db indices, autosave files. |
| `/data/UserData/UserLibrary/Recordings/`     | MoveOriginal  | Same. Schwung recordings already namespace under `Schwung/...`. |
| `/data/UserData/Move/...` (per-app config)   | MoveOriginal  | Likely contains app prefs, last-set, MIDI mappings. Two instances would clobber each other on shutdown. Must be redirected per-instance via XDG/HOME. |
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

From `/proc/<pid>/fd/` symlinks (counts only — the symlinks themselves are
unreadable as user `ableton`) and from `strings /opt/move/MoveOriginal`:

| Device                       | Singleton? | Notes                                                                                                 |
| ---------------------------- | ---------- | ----------------------------------------------------------------------------------------------------- |
| `/dev/ablspi0.0`             | **YES — hardware**  | The SPI link to the Move's MCU. One physical device, one open at a time. The Schwung shim already arbitrates this — *the very reason a second instance has to be virtual*. Strategy: route instance #2's SPI traffic through the shim's mailbox model, never through real ablspi. |
| `/dev/log`                   | No        | UDS to syslog. Multi-client. |
| `/dev/urandom`               | No        | RNG. Multi-client. |
| `/dev/snd/*`                 | **Absent** | `/dev/snd` does not exist on the device (no ALSA hardware). Yet `MoveOriginal` is linked against `libasound` and contains `MidiInAlsa` / `MidiOutAlsa` / `RtApiAlsa` symbols. So MoveOriginal can *try* to use ALSA for USB-MIDI but won't find any cards. Schwung's USB-MIDI path is via the SPI MIDI tunnel (cable 2), not ALSA. Worth confirming whether MoveOriginal's USB MIDI flows through `/dev/ablspi0.0` (SPI bridge) or attempts an ALSA `snd-rawmidi` it never finds. |
| `/dev/input/event*`          | Unknown   | Not visible without readable `fd/`. **Investigate** whether MoveOriginal opens any `/dev/input/event*` (capacitive keyboard layer? button matrix?). If so, evdev is single-reader by default — singleton hazard. |
| `/dev/dri/*`, framebuffer    | Unknown   | Not visible. The Move display is over SPI, not DRM, so probably none. |

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

For dual-Move-instances we need to neutralize, in priority order:

1. **`/dev/ablspi0.0`** — **already done** by Schwung's shim (mailbox).
2. **D-Bus well-known names** (`com.ableton.move.*`) — needs a per-
   instance bus. **Largest piece of new work.**
3. **`/data/UserData/UserLibrary` and config dirs** — needs per-instance
   redirection (env-var or `openat()` shim). Follow-up trace required to
   enumerate the exact paths.
4. **TCP port 80 / 7681** — instance #2 either picks alt ports or stays
   web-headless.
5. **`@.../bus/MoveWebService/system` and friends** — auto-handled by (2)
   if we go per-instance bus.
6. **Avahi / mDNS / Link UDP** — multi-listener by design; just give
   instance #2 a distinct service name.

## Next steps

- **Task 0.1.b**: write a recon LD_PRELOAD shim variant that logs every
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
