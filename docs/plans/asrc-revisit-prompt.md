# ASRC revisit — prompt for a future session

Paste the block below into a fresh Claude Code session when you're ready to
take another swing at Link Audio async sample-rate conversion.

---

I want to add async sample-rate conversion (ASRC) to the Schwung
`link-subscriber` sidecar to eliminate residual audio discontinuities caused
by clock drift between Ableton Link's audio thread and Move's SPI callback.

**Background:** The migration to public `abl_link` audio API (merged at
`e617866d`, see `docs/plans/2026-04-17-link-audio-official-api-migration.md`)
works, but at ~1 pop per couple minutes during sustained play, caused by the
two threads running on independent clocks (~0.2% long-term drift + burst
jitter). Each catchup in `link_audio_read_channel_shm` drops a block →
brief pitch click.

**What's already set up for you:**

- `libs/libsamplerate/libsamplerate.a` — cross-compiled for aarch64
  (SINC_FASTEST ≈ 13 taps, cheap)
- `libs/libsamplerate/samplerate.h`
- `scripts/build-libsamplerate.sh` — regenerates the static lib from source
  if needed
- `docs/link-audio-crackle-followup.md` — problem statement + approach notes

**Three previous attempts all made audio WORSE, don't repeat these mistakes:**

1. **Hand-rolled linear interpolator** — buggy boundary conditions produced
   blips. Don't roll your own; use libsamplerate.
2. **`SRC_LINEAR` with variable ratio** — libsamplerate docs explicitly say
   `SRC_LINEAR` and `SRC_ZERO_ORDER_HOLD` are "useless for asynchronous
   conversion because the output is not smooth across ratio changes." Must
   use `SRC_SINC_FASTEST` or better.
3. **P-controller swinging ratio ±0.5% per callback** — that's ±0.5% pitch
   modulation, more audible than the original pops. Gain was too high and
   smoothing too weak.

**Prerequisites to verify before touching device code:**

- Build an offline test bench: feed a 1 kHz sine wave + realistic jitter
  pattern through your ASRC, verify no audible artifacts by FFT or ear
  (dump to WAV).
- Confirm `input_frames_used == input_frames` on every `src_process` call,
  or queue leftover input for next call — dropping leftover silently loses
  samples.
- Tune the fill-target P-controller on the bench to converge smoothly (gain
  small, LPF heavy); verify ratio settles near 1.0 with drift < ±0.05% in
  steady state.

**Integration plan:**

- Wire into `scripts/build.sh` around line 326 (where `link-subscriber` is
  linked). Add `-I./libs/libsamplerate`,
  `./libs/libsamplerate/libsamplerate.a`.
- Modify `src/host/link_subscriber.cpp` source callback to pipe samples
  through per-slot `SRC_STATE *` (one per Move channel, allocated at
  main-thread startup, destroyed on shutdown).
- RT-safe: pre-allocated float scratch buffers, no `new` / `malloc` / locks
  / logging inside the callback.
- Target ring fill level ~4 blocks (1024 samples), ratio clamped
  `[0.999, 1.001]` at most (0.1% max pitch shift, much less audible than
  0.5%).

**Success criteria:**

- <1 pop per 30 minutes of sustained play
- No audible pitch drift
- SPI callback timing unaffected (CPU headroom)

Use the `superpowers:writing-plans` skill to write a proper plan first, with
offline test bench tasks before any on-device deploy. Do not touch the
device until the bench shows clean output.
