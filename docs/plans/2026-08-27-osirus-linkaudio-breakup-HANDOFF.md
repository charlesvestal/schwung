# Move → Schwung audio breakup — measured, half fixed, one-line repro

**Status: still OPEN as an audible fault, but no longer mysterious.** One of the
two mechanisms is found and fixed (PR #306). The other is reproducible in one
command and is characterised below. Supersedes the 2026-08-27 morning version,
which framed this as an osirus/host bisect.

---

## 1. THE REPRO — start here, it needs no module and no listening

```bash
ssh root@move.local
cat > /data/UserData/spin.sh <<'EOF'
while :; do :; done
EOF
taskset 0x7 sh /data/UserData/spin.sh &
chrt -f -p 20 $!          # FIFO 20, cores 0-2
```

**A busy loop that does nothing** takes Move's Link Audio delivery from
`max_gap 8.7 ms / burst 2` to `max_gap 377 ms / burst 127`, and it reverts the
instant you kill it. That is the whole fault, with no module involved.

**DO NOT run the spinner at FIFO 70.** It starves `sshd` on cores 0-2 and the
device has to be power-cycled. FIFO 20 reproduces it fine. (Learned the hard
way; core 3 is free but sshd's threads are not pinned there.)

Watch it with the sidecar's continuity counter (see §3):

```bash
touch /data/UserData/schwung/debug_log_on   # ~100 s before the gate opens, see §6
grep 'continuity slot' /data/UserData/schwung/debug.log
```

## 2. What it is NOT — every one of these was measured, do not re-test

| Suspect | Verdict |
|---|---|
| **osirus** | its own output is **clean in every configuration**: 1.02–1.07x phase-flat, 0 discontinuities, max sample delta 508 on a peak of 8100 |
| osirus CPU starvation | burns 82% of a core and reports `ur=0` for minutes |
| module thread priority | tested at FIFO **70**, FIFO **20**, and pinned to **core 3**. All equivalent |
| core placement | pinning the module to core 3 made it **audibly worse** (core 3 is 42.6% busy, not idle; it went to 95%) |
| **our sidecar's thread priority** | SCHED_OTHER vs **FIFO 45**: `breaks 0.61–1.46%` vs `0.32–1.18%`, `max_gap 282–366 ms` vs `359–478 ms`. **No help** |
| raw CPU capacity | ~54% across four cores at the time of failure. Not saturated |
| UDP / socket buffers | `Udp6InErrors=0`, `Udp6RcvbufErrors=0` under load. Nothing dropped in the kernel |
| our ring management | since PR #306: starve, catchup, would_overrun, la_starve_fallback **all zero** |
| our mixing | the artifact is present in `slot0_move_track.pcm` **before** the sum; `synth_src` is clean |
| our ring reader | the artifact is locked to the producer's **125**-frame phase, not our 128-frame read |
| the sidecar's writer | ring write → fence → release-store is correct, and `produced_count` = 352.8/s means no callback is missed |

**Three host versions and four osirus versions all reproduced it** because none
of them changes CPU load on cores 0-2.

## 3. The instrument that settled "us or them"

`link_cb_note_continuity` in `src/host/link_subscriber.cpp` compares the FIRST
sample of each incoming buffer against the LAST of the previous one for the
same slot, judged against the signal's own local slope. It runs **inside the
source callback** — before our ring, before `read_pos`, before the mixer — so
it observes Move's stream at the earliest point that exists.

```
continuity slot=0  joins=1929  breaks=34  (1.76%)   <- Move track 1, playing
continuity slot=1  joins=1929  breaks=9   (0.47%)
continuity slot=2  joins=1929  breaks=0   (0.00%)   <- silent track
continuity slot=3  joins=1929  breaks=0   (0.00%)   <- silent track
```

The two silent tracks reading **0.00%** are the built-in control: the detector
is not merely counting drum transients, or it would fire on everything.

**Caveat on the word "upstream".** Breaks here prove the audio is already
discontinuous when our callback receives it. They do **not** prove Move's
firmware is at fault — the Link SDK's receiver runs **inside our own sidecar
process**, so the socket options, the thread scheduling and the subscription
are all ours. "Before our ring" is not "before anything we own", and conflating
the two is how this got called an Ableton bug twice (see memory
`link_audio_producer_burst_dropouts`, where the same wrong call was made and
corrected).

## 4. FIXED, and shipped in PR #306: the ring was 46 ms

`LINK_AUDIO_IN_RING_BLOCKS` was `LINK_AUDIO_PUB_SHM_BLOCKS` — inherited from
the publish side because they sit next to each other in the header. We write
the pub ring on a metronome; **Move writes this one in bursts.** Against a
measured 92 ms stall and 85 ms burst, a 46 ms ring is empty less than halfway
through and cannot hold the refill. Catch-up made it worse by discarding at
35 ms — below the burst — so every refill that could have covered the next
stall was thrown away. Starve, burst, discard, starve.

Now 186 ms with the threshold derived from the ring. Result on hardware:

| | before | after |
|---|---|---|
| Starved frames / 30 s | 145 runs, **2.53%** of audio missing | **0**, 0.095% |
| osirus underruns / 30 s | 598 | **0** |
| Move's own `max_gap` | 92 ms | 14–32 ms |

**This did not fix the audible fault** — the reporter reports it as no better —
but it was a real bug and it was masking the other one.

## 4a. THE CONFIGURATION SPACE IS EXHAUSTED — it is a capacity wall

Do not spend another session on priorities or affinities. Every combination was
measured with the §1 spinner rig, and the result is a strict either/or.

Load placement, measuring BOTH sides (Link Audio delivery and our own SPI path):

```
                     LinkAudio gap  burst   SPI frame max   SPI tx max   frames/s
baseline                12,700 us      4       2,487 us       563 us       345
load on cores 0-2      390,286 us    114       2,537 us       588 us       345
load on CORE 3 ONLY     54,818 us      3      54,001 us    51,855 us       328
load on cores 0-1      389,445 us    128            --            --        --
load on core 0 ONLY  1.5-5.7 SEC     134            --            --        --
load on cores 0-3      316-382 ms   93-114           --            --        --
```

- **cores 0-2** (current policy): our audio is FINE, Move's Link Audio collapses.
  This is the shipped configuration and today's symptom.
- **core 3**: Move's Link Audio is fine, **our SPI path collapses** — 54 ms
  frames, 51.9 ms transfers, and frames/s falls 345 → 328, i.e. real dropped
  audio frames.
- **spreading to 0-3**: no better than 0-2.
- **confining to one core**: much worse (1.5-5.7 s gaps).

Scheduling priority, every axis:

```
module DSP at FIFO 70 / FIFO 20 / SCHED_OTHER      all equivalent
sidecar at SCHED_OTHER / FIFO 25 / FIFO 45         all equivalent (340-400 ms)
sidecar affinity 0-2 vs 0-3                        all equivalent
```

Also checked and clean: `sched_rt_runtime_us` = 950000/1000000 (RT throttling
ON, 95%), `ksoftirqd` SCHED_OTHER on all four cores, NET_RX softirqs spread
across all four, `lo` with **0 drops in 2.9M packets**, `Udp6InErrors` and
`Udp6RcvbufErrors` both 0 under load.

**Conclusion: the device cannot run a heavy realtime module and Move's Link
Audio publishing at the same time.** There is no placement or priority that
satisfies both. What remains is a product decision, not a scheduling fix.

One trap worth naming: the two `MoveOriginal` threads restricted to cores 0-2
(`FIFO 10` and `SCHED_OTHER`) are **ours** — `snap_worker_main` in
`schwung_shim.c` and the shim worker. A thread inherits its parent's `comm`, so
they report as `MoveOriginal` and look like Move firmware threads being
starved. They are not. Every one of Move's own threads is `0-3`.

## 5. What is still open

Move's publisher degrades under **any** sustained realtime CPU load on cores
0-2, regardless of who holds it, at what priority, or what we run our own
receiver at. Not yet tried:

Given §4a, the remaining options are product decisions, not scheduling fixes:

1. **Reduce module CPU.** dake's multi-core render split. **Measure it on the
   spinner rig first** — it spreads work across cores 0-2, which is precisely
   the placement that breaks Move's publisher, so a shorter total time may not
   help and could hurt.
2. **Admission control.** When total module load is high, decline to engage
   `rebuild_from_la` and stay on Move's own mailbox mix, with a visible reason.
   Degrades a feature instead of the audio. This is the only option that is
   fully in our control and cannot make anything worse.
3. **Document the limit** and let users choose between a heavy synth and
   Move→Schwung.
4. **Not yet examined:** the receive socket's buffer sizes (`SO_RCVBUF`) and
   how we subscribe. `d0e90664` disproved the 5 s channel-request renewal
   theory, but the socket options have never been looked at. Low expected
   value given no UDP drops are recorded, but it is the one stone left.

## 6. Traps that cost real time today

- **`unified_log` rechecks `debug_log_on` every 100 CALLS, not on a timer.**
  For a subsystem logging once a second that is ~100 seconds of silence after
  arming. Budget three minutes before concluding a flag is broken.
- **An absolute click threshold cannot tell a splice from a kick drum.** Score
  by PHASE mod 125 (`tools/link-audio/analyze_capture.py`). The first pass here
  used `|diff| > 2000` and reported drums.
- **Report phase ABSOLUTE, never per-window.** A 3 s window is 132300 samples
  ≡ 50 (mod 125), so consecutive windows of the same steady artifact print
  59, 122, 0, 0… and look like drift.
- **The splice-rate percentage is meaningless without a phase lock.** With no
  lock it counts ordinary transients — a provably clean `synth_src` scores
  "20.3/s" that way.
- **Three probes measured the wrong thing today**, each caught only by running
  a control: an offset search that included `k=0` (comparing a segment to
  itself, so it "proved" everything was contiguous); a junction-badness
  detector that could not recover a KNOWN 3/11/40-sample cut; and the
  absolute-threshold detector above. **Write the control first.** See memory
  `probes_that_measured_the_wrong_thing`.
- **A 2.9 s capture is too short.** The same configuration scored 5.61x, 1.84x,
  1.01x, 2.58x and 2.94x across five consecutive snapshots. Captures are 30 s
  now (`echo 30 > align_dump_trigger`).
- **`chrt` and `taskset` need root** on the module's threads. `ssh root@` works.
- **PR #303's deferred loader does not cover boot restore.** A module already
  in the slot at boot loads synchronously on the SPI callback and its children
  inherit FIFO 70 on cores 0-3. Verified with `chrt -p` on the `DSP` thread:
  cold boot gives 70/0-3, re-picking the module in the UI gives 20/0-2. That
  is why #303 looked like it changed nothing.

## 7. Related

- PR **#306** — the ring fix, the RT-safe capture, and the build fixes below.
- `docs/plans/2026-08-27-rt-safe-module-loading-handoff.md` — PR #303.
- Memory `link_audio_producer_burst_dropouts` — the same bug, first recorded
  2026-08-19, with the same wrong conclusion drawn and corrected.

**Build bugs found underneath this, all fixed in #306:** `libs/link`
uninitialised made `build.sh` warn and exit 0, `package.sh` omit the sidecar,
and `install.sh` only ever kill it — so the sidecar was frozen on the device
for weeks and rode through the whole bisect as the one thing nobody varied.
And in the Dockerfile, a vestigial `file .../sf2/dsp.so || echo "not found"`
for a module that left this repo long ago **always** failed, so the `||`
**always** fired and masked the exit status of the entire build chain. Every
build failure ever run was swallowed.
