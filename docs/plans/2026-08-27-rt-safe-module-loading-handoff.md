# Handoff — RT-safe module loading (PR #303)

Branch `fix/rt-safe-module-loading`, **PR #303 open, all four CI checks green,
NOT merged, NOT released.** Deployed to hardware and tested; the device has
since been reverted to the v0.11.6 release tarball.

Design and rationale: `2026-08-27-rt-safe-module-loading.md`. This file is only
what a next session needs to finish the job.

---

## 1. What is done

`src/modules/chain/dsp/chain_loader.c` — a `synth:module` write asks; a
loader thread builds the instance into a staging record no render path can
reach; `v2_render_block` publishes it by swapping pointers. Synth position
only.

**Hardware-verified 2026-08-27, on device, with instruments armed:**

| Claim | Result |
|---|---|
| 673 ms load stall gone | **confirmed** — no audible gap on load; 345 frames/s, headroom 2508 µs, 1 late IRQ |
| Plugin threads no longer born realtime | **confirmed** — minijv, osirus, obxd and sfz created **zero** new RT threads between them |
| No audible regression on load | **confirmed** — no crackle on minijv |

Before this change that same sequence produced minijv's emulator at FIFO 45,
osirus's boot thread at 70, and sfz's five rayon workers at 70.

## 2. What is NOT done — pick up here

1. **Load sites 2–5 are still synchronous**: audio FX, MIDI FX (both chain
   positions), Master FX slots, plus `load_patch` and boot restore. The staging
   record and commit point are shared machinery, so these are mechanical now.
   Note boot restore being synchronous is why no `schwung-loader` thread exists
   after a cold boot until you swap a module — that confused a live debugging
   session, so it is worth a comment if you touch it.
2. **`version.txt` is empty on device** and the boot screen shows a stale
   version. Unrelated to this branch, found while verifying deploys, and it
   actively misleads a bisect — it cost us a wrong conclusion (see §4). Worth
   its own fix.
3. **The burn number still has never been measured** — "these modules starve
   `Link Main`" remains a hypothesis that fits.
4. **sfz's five threads are rayon, not sfizz.** See §5.

## 3. The thing most likely to be got wrong next: the PRIORITY RUNG

The loader runs at **`SCHED_FIFO 20`** (`CHAIN_LOADER_RT_PRIORITY`), *not*
SCHED_OTHER, and this was learned the hard way on hardware.

Everything a plugin spawns from `create_instance` — threads **and forked
children** — inherits the loader's scheduling. So the number *is* the fleet's
behaviour:

```
70  what it inherited before (the SPI callback). ABOVE Move's Link Audio
    publisher `Link Main` at 35, so a module doing sustained work starves
    Move's audio device-wide. THE BUG.
 0  SCHED_OTHER. Fixes the starvation and BREAKS modules that were silently
    relying on inherited realtime. MEASURED: osirus's forked DSP child
    audibly underran. minijv would have followed by inspection.
20  realtime, so those modules still keep up, but below 35 so none of them
    can outrank Move's publisher. NOTHING IN THE FLEET NEEDS A PATCH.
```

Do not "simplify" this to SCHED_OTHER. It is not tidier, it is a regression,
and it was shipped and heard.

**Two mechanisms, and neither reports failing.** The first version called
`sched_setscheduler` and trusted it. On hardware it silently did not take — the
audit found `schwung-loader` at FIFO 45, inherited straight from the caller.
`sched_setaffinity` succeeded on the same thread two lines later, so it was not
a build or `_GNU_SOURCE` problem. It now calls `pthread_setschedparam` first
(the call minijv makes successfully from the same context), then
`sched_setscheduler`, then **verifies with `sched_getscheduler` and logs
loudly**, dropping to SCHED_OTHER as a last resort if it is still at or above
35. Keep the verification.

## 4. Read this before debugging on hardware again

A long stretch of 2026-08-27 was spent chasing an osirus audio breakup that was
**not caused by this branch**. What it cost, and why:

- **Repeated `install.sh` runs are not equivalent to clean boots.** Roughly
  eight ran in one session. By the end the device had a **zombie
  `link-subscriber`** (`1167 Zs link-subscriber <defunct>`) with Link Audio
  still enabled, and a load average of 5.79 on four cores. Power-cycle between
  host swaps; do not bisect through accumulated service-restart state.
  **NOTE: a reboot did NOT fix the breakup**, so accumulated deploy state was
  *not* the cause — that was a theory of mine that did not survive the test.
  The zombie subscriber and the load average are still unexplained and may be
  symptoms rather than causes. See the osirus handoff.
- **Diagnostics were left armed across every reinstall.** The flag files live
  in `/data/UserData/schwung/` and `install.sh` does not clear them, so
  `debug_log_on`, `rt_thread_audit_on` and `spi_tally_on` rode through main and
  v0.11.6 untouched. Disarm before any listening test.
- **Verify the deploy landed AT THE MOMENT you test it, not afterwards.** An
  md5 check was run after a later reinstall and treated as proof of an earlier
  one. The real check that caught it was the user noticing the **8-slot Master
  FX UI**, which `MASTER_FX_SLOTS 8` (`464caf4a`, 2026-08-21) puts in **no
  tag at all** — so it cannot appear on any release. A UI feature with a known
  landing commit is a better version oracle than `version.txt`, which is empty.
- **Four osirus releases and three host versions all reproduced it.** When
  everything you can vary still fails, the variable is something you are not
  varying. Stop bisecting and measure.
- The real cause is still open: the user reports it is fine until **Move →
  Schwung (the Link Audio subscriber)** is switched on. That points at the
  known open bug `link_audio_producer_burst_dropouts` — plus, on this device, a
  dead sidecar. **Retest from a cold boot before assuming anything.**

## 5. Module repos

Three PRs, all **belt-and-braces now rather than required**, because the FIFO 20
rung means unmodified modules already inherit correct scheduling:

| Repo | PR | State |
|---|---|---|
| `schwung-jv880` | #9 | emu thread asks for FIFO 20 instead of inheriting. Needs a listening test. |
| `schwung-virus` | #4 | demote/pin/name the four workers, **plus** the forked DSP child asks for FIFO 20 — that second part was added after the SCHED_OTHER underrun and is what makes it safe. |
| — | — | sfz: see below. |

**sfz's five threads are `rayon`, not sfizz.** The 2026-08-22 audit recorded
them as "sfizz's global FilePool ThreadPool ✅ cross-checked against source" —
but `schwung-sfz` **dropped sfizz** and uses `xsynth` (Rust); `build.sh`'s only
mention of sfizz is the comment saying so. The real source is
`ParallelismOptions::AUTO_PER_CHANNEL` building a `rayon` pool sized to CPU
count (4 on Move) plus the shim's own thread at `xsynth_shim/src/lib.rs:530` =
the five observed. The audit's own rule — *"a lead list, not a verdict; confirm
in source before acting"* — was followed against source that is not compiled.

Consequences: the **sfizz submodule is vestigial** and should probably be
removed; `docs/plans/2026-08-27-sfizz-filepool-rt.patch` targets dead code and
should be deleted rather than applied. If sfz ever needs its own fix, `xsynth`
**is** a fork we control (`charlesvestal/xsynth`, branch `schwung-fork`), and
rayon's `.start_handler()` is the hook.

## 6. Merge checklist

- [ ] Cold-boot retest of the osirus/Link-Audio breakup, to confirm it is
      independent of this branch (it reproduced on `main` and v0.11.6, so it is)
- [ ] Decide whether to land load sites 2–5 in the same release or follow up
- [ ] `min_host_version`: **no bump needed** for any module
- [ ] Release checklist proper: `version.txt`, `module-catalog.json`, manual
