# Move → Schwung audio breakup — MEASURED, and it is not osirus

**Status: root cause found. Move's Link Audio stream is spliced when a
CPU-heavy module is loaded; the cause is total CPU capacity, not scheduling
priority, and osirus is exonerated as a source of the artifact. The remedy is
undecided (§6a).** Supersedes the 2026-08-27 morning version of this file,
which framed it as an osirus/host bisect.

---

## 1. The one-line answer

**When a CPU-heavy module is loaded, Move's Link Audio stream becomes spliced:
~4% of its 125-frame blocks do not continue the previous one, ~15 times a
second.** The `Move → Schwung` toggle does not create those splices — it
decides whether you *listen* to them, because it switches the shim from passing
Move's own mailbox mix to **rebuilding the mailbox from Link Audio**. Off, you
hear Move's clean output. On, you hear the Link Audio reconstruction, splices
included. With a cheap module in the slot there are no splices to hear (§6).

That accounts for every fact the previous session could not:

| Fact | Explained by |
|---|---|
| Fine until Move→Schwung is on | the toggle selects the LA reconstruction as the audio you hear |
| Survives a cold boot | nothing accumulates; the stream is like this from the start |
| Reproduces on main, PR #303 **and** v0.11.6 | none of them touch Move's publisher |
| Reproduces on osirus 0.5.0 / 0.6.0 / 0.6.1 | osirus contributes none of it |
| Only "osirus's" audio breaks up | in rebuild mode Move's track is **summed into the slot** before FX |
| Muting the Move track fixes it | that removes the spliced source from the sum |
| Happens with a synth pad too | it is not drum transients |

## 2. The measurement — reproduce it in five minutes

`align_dump_trigger` dumps the two inputs to the slot sum *separately*, at the
exact point they are combined:

```bash
ssh ableton@move.local "cd /data/UserData/schwung && \
  rm -f slot0_move_track.pcm slot0_synth_src.pcm && touch align_dump_trigger"
# ~3 s later both files exist: s16le stereo 44.1k, 2.9 s each
```

Then run the **phase test**, which is immune to the drum-transient confound
that fooled the first pass here. For every candidate phase `p`, take the mean
`|diff|` of samples at positions `≡ p (mod 125)`:

```
slot0_move_track.pcm   BEST phase = 112   mean|diff| 5.61x overall
                       every other phase  1.00 - 1.02x
                       control period 128 -> 1.77x   (smear of the same spike)
                       control period 127 -> 1.75x
slot0_synth_src.pcm    BEST phase 1.05x, std 0.020   -> FLAT. CLEAN.
```

A transient cannot prefer one phase-of-125. **125 frames is Move's Link Audio
block** (`max_frames=125` on every telemetry line; 44100/125 = 352.8 blocks/s,
which is exactly the observed `produced_count`).

Shape of the splice, at the boundary:

```
frame 7612:  2376 2328 2280 2233 2183 2138 2092 [1001] 1028 1056 1083
             descending smoothly ................... ^ jumps AND reverses

exact repeat of >=2 frames across boundary:   0 / 1022     -> not duplication
median boundary step 36  vs  median local slope 34         -> 96% of boundaries clean
boundaries with step > 3x local slope:        4.2%         -> ~15 / second
```

Not a repeat, not clipping, not a level jump. Audio is **missing**.

## 3. What is NOT the cause — do not re-test these

| Variable | Evidence |
|---|---|
| **osirus** | its own output measured **0 discontinuities**, phase-flat at 1.05x, max sample-to-sample delta 508 on a peak of 8100 |
| osirus CPU | burns 82% of a core and reports `ur=0` for minutes at a time — it is not short of CPU |
| osirus thread priority | tested at FIFO 70 **and** FIFO 20; see §4 |
| our mixing / clipping | the splice is present in `slot0_move_track.pcm` **before** the sum; `synth_src` is clean |
| our ring reader | the splice is locked to the producer's **125**-frame phase, not our 128-frame read |
| the sidecar's writer | ring write → `__sync_synchronize()` → release-store `write_pos` is correct, and `produced_count` = 352.8/s means **no callback is missed** |
| starve / catchup / would_overrun | all quiet during the measurement — this is not a ring-management fault |
| the stale sidecar (§5) | the missing commit is **108 insertions, 0 deletions** — pure telemetry |

## 4. The priority hypothesis, and why it died

osirus is fork-parallel. Its DSP child (`ppid` = MoveOriginal, `comm` inherited
as `Audio Main/SPI`, worker thread named `DSP`) inherits scheduling from
whatever forked it. Measured:

```
913   949  FF  35  10.5   Link Main          <- Move's LA publisher
977   980  FF  70  79.7   DSP                <- osirus, ABOVE Link Main
```

That is a real contract violation and it is worth fixing on its own merits —
it is also **the burn number §2.3 of the PR #303 handoff said had never been
measured**: 16.3 s of CPU in 20 s, i.e. 81.5% of a core, at realtime priority.

But it is not this bug:

| | osirus underruns | LA starve | audible |
|---|---|---|---|
| FIFO 70, cores 0–3 | ~12 / 10 min | 44 starves, 1857 would-overrun | breakup |
| FIFO 20, cores 0–2 | **88 / second** | ~1–2 / 15 s | **worse** |

At FIFO 20 osirus starves itself (pitch/speed wobble, matching its `ur` counter
and matching #303's note that SCHED_OTHER made this child audibly underrun).
Neither rung removes the crackle, because the crackle is in the Link Audio data.

**Why PR #303 "reproduced":** its deferred loader covers a `synth:module`
*write* only. `v2_load_synth` is still used for "patch load, boot restore", so
a module already in the slot at boot is loaded synchronously on the SPI
callback and its children still inherit FIFO 70 on cores 0–3. Verified on
device: after a cold boot on the #303 build there is **no `schwung-loader`
thread at all** and the DSP child reads `FIFO 70, aff 0-3`. Re-pick the module
in the UI and it becomes `FIFO 20, aff 0-2`. Both states were confirmed with
`chrt -p` before listening.

## 5. Build bug found on the way — the sidecar was never being shipped

`libs/link` is an **uninitialised submodule**. Consequence chain:

```
git submodule status  ->  -e9a2e414... libs/link      (leading '-')
build.sh              ->  "Warning: Link SDK not found at libs/link/,
                           skipping link-subscriber"     ... and exits 0
package.sh            ->  only adds ./link-subscriber "if it was built"
install.sh            ->  only KILLS link-subscriber; never installs it
```

So the Link Audio sidecar — the sole reception path for Move→Schwung audio —
was frozen at whatever binary was placed on the device once, and rode
unchanged through every deploy, including all three host versions bisected on
2026-08-27. The deployed binary did not contain `max_burst_run` at all.

It is **not** the cause (the missing commit is pure telemetry) but it is why
nobody could see this: the callback-delivery instrumentation added in
`d0e90664` was never on the device. Fixed for this session by
`git submodule update --init --recursive libs/link` and a rebuild; the tarball
then contains `schwung/link-subscriber` and the deployed md5 matches.

**This needs a real fix**: a silent skip that produces a working-looking
tarball is the same class of failure as the empty `version.txt`. Either make
the submodule a hard build requirement, or fail the build, or at minimum have
`install.sh` refuse to proceed when the tarball has no sidecar.

## 6. It is OURS: CPU capacity, not scheduling priority

The controlling experiment was run. Same Move, same routing, same sidecar, same
shim — only the module in slot 1 changed:

```
                    phase-125 lock     verdict
osirus  move_track     5.61x           hard structural splice
braids  move_track     1.02x           NONE
either  synth_src      1.05x / 1.16x   the module itself is clean
```

**Swap osirus for braids and Move's Link Audio stream stops being spliced.** So
Move's publisher is not inherently lossy; we are starving it.

Read the **phase ratio**, not a splice count. Where there is no phase lock the
`step > 3x local slope` fraction just counts ordinary transients at an
arbitrary phase — `osirus: synth_src` scores "20.3/s" by that metric while
being provably clean at 1.05x. That is the same trap as the absolute-threshold
detector in §7.

**The lever is not priority.** The osirus dump above was taken at **FIFO 20 on
cores 0–2** — already below `Link Main` (FIFO 35), already off the SPI core —
and it starved Move's publisher anyway. This corrects the mechanism recorded in
memory `link_audio_producer_burst_dropouts` ("our DSP at 70 starves Link Main
at 35"): the rung is not what matters. What matters is that osirus consumes
~82% of one core out of four, and Move's audio engine cannot make its deadline
alongside that. Cache/memory-bandwidth contention is a candidate mechanism that
has NOT been measured.

Note also that our own SPI-callback cost roughly doubles when routing comes on
(`pre avg 160µs → 338µs`), all of it inside Move's FIFO-70 thread. That is a
second, independent contribution that has not been separated from the module's.

Prior art, and it is a **different** phenomenon — do not merge the two:
`d0e90664`'s commit message recorded rare (~1 per 5–40 s) ~205 ms stalls on all
four slots simultaneously with Move's `Link Main` showing a 172 ms continuous
sleep, and placed those in Move's firmware publisher. This bug is constant
~15/s block splices under load.

## 6a. What to do about it — not yet decided

Options, none tested:

1. **Reduce osirus's cost.** A contributor (dake) reports a multi-core render
   split with >2x improvement. It requires modules to have no shared mutable
   statics between instances — that requirement needs writing into
   `docs/MODULES.md`.
2. **Admission control.** If total module CPU exceeds a threshold, refuse to
   engage `rebuild_from_la` and stay on Move's clean mailbox mix, with a
   visible reason. Degrades a feature rather than the audio.
3. **Reduce our own per-frame SPI cost** in rebuild mode (the 160→338 µs).
4. **Do nothing and document it** as a capacity limit of the platform.

Measure before choosing: separate the module's contribution from the shim's by
re-running §2 with osirus loaded but `rebuild_from_la` cost minimised.

## 7. Instruments that work, and one that lies

- `align_dump_trigger` — the decisive one. Separates the two summands.
  **Caveat:** it only writes `move_track` when `have_move_track` is true, so a
  starved frame is *skipped* and the file splices across it. Check both files
  are the same length before trusting a discontinuity near a starve.
- The **phase test** (§2), not an absolute-threshold click detector. The first
  pass here used `|diff| > 2000` and could not distinguish a splice from a
  kick drum. See memory `probes_that_measured_the_wrong_thing`.
- `chrt -p <tid>` on the `DSP`-named thread — a version oracle that cannot lie
  about whether a loader change took.
- osirus's own `virus_debug.log` `ur=` counter — free, always on, and it is
  what exonerated osirus.
- **`unified_log` only rechecks `debug_log_on` every 100 CALLS**, not on a
  timer. For a subsystem that logs once a second that is ~100 seconds of
  silence after arming. Budget ~3 minutes before concluding a flag is broken.
