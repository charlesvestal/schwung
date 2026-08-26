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

## 5. What is still open

Move's publisher degrades under **any** sustained realtime CPU load on cores
0-2, regardless of who holds it, at what priority, or what we run our own
receiver at. Not yet tried:

1. **Reduce the load.** dake's multi-core render split for osirus. Note it
   spreads work across MORE cores, which may not help contention even though
   it shortens total time — measure with the spinner rig before assuming.
2. **Keep cores free.** The spinner used `taskset 0x7` (cores 0-2). Test
   whether load confined to ONE core still breaks delivery — if Move's
   publisher only needs one uncontended core, an affinity policy is a fix.
3. **Look at how we subscribe.** `SourceProcessor` / channel-request TTL and
   the socket options are ours. `d0e90664` tested the 5 s renewal theory and
   disproved it, but the buffer sizes on the receive socket have never been
   examined.
4. **Admission control.** If we cannot keep Move's publisher healthy under
   load, refuse to engage `rebuild_from_la` and stay on Move's own mailbox mix
   with a visible reason. Degrades a feature instead of the audio.

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
