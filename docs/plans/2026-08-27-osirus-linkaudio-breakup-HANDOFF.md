# URGENT — audio breaks up whenever Move → Schwung (Link Audio) is on

**Status: OPEN, unfixed, reproduces on a cold boot. Not caused by any change
made on 2026-08-27.** This is the priority.

## The symptom, in the reporter's words

> *"things work fine until i turn move->schwung on"* — move→schwung being the
> **Link Audio subscriber**.

> *"this is more constant than the 4 seconds, this is really bad"*

Constant audio breakup, worst with osirus loaded. **A reboot does not fix it.**

## START HERE — the control we never ran

The single most valuable experiment, and it was somehow never done in six hours
of bisecting:

1. Cold boot. **Link Audio (Move → Schwung) OFF.** Load osirus. Play.
2. Same boot, **Link Audio ON**. Play.

The reporter says (1) is fine and (2) breaks. **Confirm that directly and write
down the result**, because everything below assumes it and nothing has verified
it in a controlled way. If (1) also breaks, the whole framing changes.

Then re-run the 3-point bisect from `link_audio_producer_burst_dropouts`, which
is the closest existing description of this:

| # | Setup | Expected (2026-08-19) |
|---|---|---|
| 1 | Stock Move, no Schwung | 0 stalls |
| 2 | Schwung, Link Audio on, **empty slots** | 0 stalls |
| 3 | Schwung, Link Audio on, **modules loaded** | ~1 stall per 4–40 s |

If #2 now breaks, this is **worse than** the known bug and is a new failure. If
#3 is *constant* rather than every 4–40 s, likewise. That distinction is the
first thing to establish.

## What is already ELIMINATED — do not re-bisect these

Six hours went into this. All of it reproduced the breakup:

| Variable | Tested | Verdict |
|---|---|---|
| Host: PR #303 loader branch | yes | reproduces |
| Host: `main` | yes | reproduces |
| Host: **v0.11.6 release tarball** | yes, hash-verified | reproduces |
| osirus v0.6.1 (released that morning) | yes | reproduces |
| osirus v0.6.0 | yes | reproduces |
| osirus v0.5.0 (April, pre-0.6 line) | yes | reproduces |
| Debug logging overhead | armed vs disarmed | reproduces either way |
| DSP Clock % wrong for ROM model | reporter confirmed correct per ROM | not it |
| Accumulated state from ~8 `install.sh` runs | **reboot** | reproduces — not it |

Three host versions × three module versions. **When everything you can vary
still fails, the variable is something you are not varying.**

## Unexplained observations worth chasing

Captured on the device on host v0.11.6, osirus v0.5.0, diagnostics off:

- **`1167 Zs link-subscriber <defunct>`** — the Link Audio sidecar was a
  ZOMBIE, unreaped, while Link Audio was still enabled. So Schwung was
  subscribing to a stream nothing was servicing. **Is it dead on a cold boot
  too? Is it crash-looping? Who is supposed to reap or restart it?** This is
  the single most suspicious thing seen all session.
- **Load average 5.79–7.41 on a four-core device**, two minutes after boot,
  with `top` showing no process above 0.0% CPU. Load without visible CPU
  usage means **uninterruptible sleep (D state)** — I/O or a stuck kernel path,
  not compute. Find what is in D state.
- **The ROM files are dated Aug 26 14:11**, the day before the symptom
  appeared, and `virus_a_firmware.mid` and `virus_a_presets.mid` are **exactly
  the same size (514611 bytes)**. Never checked whether they are the same file.
  `md5sum` them. A wrong/duplicated ROM is a cheap thing to rule out and fits
  the timeline better than any code change.

## Concrete next steps, in order

1. Run the START HERE control and the 3-point bisect. Record actual results.
2. `md5sum` the four ROM files against known-good copies.
3. Determine whether `link-subscriber` is alive on a cold boot with Link Audio
   on; if not, find where it dies (`src/host/link_subscriber.cpp`), and whether
   anything restarts it.
4. Find the D-state processes behind the load average:
   `ps -eo pid,stat,wchan,comm | awk '$2 ~ /D/'`
5. Only then consider code. `link_audio_read_channel_shm` and the latency-comp
   nudge in `schwung_shim.c` are the readers; the sidecar is the writer.

## Traps that cost time — do not repeat

- **`version.txt` is EMPTY on device and the boot screen shows a stale
  version.** It is useless as a version oracle. Use a UI feature with a known
  landing commit instead: the **8-slot Master FX UI** (`MASTER_FX_SLOTS 8`,
  `464caf4a`, 2026-08-21) is in **no tag**, so seeing it means you are on
  main/a branch, never on a release. That is what caught a v0.11.6 "install"
  that had not taken.
- **Verify a deploy AT THE MOMENT you test it.** An md5 check run after a later
  reinstall was treated as proof of an earlier one. It was not.
- **`install.sh` does not clear diagnostic flags** in `/data/UserData/schwung/`.
  They survive every reinstall. Disarm before listening tests; leaving
  `debug_log_on` armed has previously *caused* the dropouts being hunted.
- **Two confident hypotheses died here** — the loader priority, and the DSP
  clock. Both fit. Neither was right. Run the experiment before believing the
  mechanism.

## Related

- Memory `link_audio_producer_burst_dropouts` — the known open Link Audio bug
  (`Link Main` at FIFO 35 starved by our DSP at 70). Closest prior art, but the
  observed pattern here is *constant*, not every 4–40 s.
- `docs/plans/2026-08-27-rt-safe-module-loading-handoff.md` — PR #303, which is
  independent of this and was cleared by testing on `main` and v0.11.6.
- `docs/plans/2026-08-22-rt-thread-audit-findings.md` — the instrument
  (`rt_thread_audit_on`) and its arming gotchas.
