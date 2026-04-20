# Release notes — next release (pre-ship test plan)

Covers all commits since `v0.9.6` (Apr 14) through `89c1d749`.

## What changed — high-level

- **Link Audio migration** to Ableton's public `abl_link` audio C API:
  reception moved from the shim's `sendto()` hook + `chnnlsv` parser to the
  `link-subscriber` sidecar via `/schwung-link-in` SHM. Plan:
  `docs/plans/2026-04-17-link-audio-official-api-migration.md`.
- **MFX ME-only bus refactor**: master FX now processes the ME bus only, no
  longer round-trips Move's mailbox. Gain staging redone.
  `docs/plans/2026-04-16-mfx-me-only-bus.md`.
- **Unity-view capture path**: skipback, quantized sampler, and native
  bridge read a new `unity_view` buffer independent of master volume, so
  captures are always at full level.
- **Idle fast-path** in `shadow_inprocess_mix_from_buffer` — no CPU cost
  when no slots / FX loaded.
- **Long-press shortcuts** always on, setting removed (330615eb).
- **Analytics opt-out** with first-run prompt (5f56cd25).
- **Tempo persistence** across subscriber restarts; **mute/solo
  persistence** across power cycles.
- **Link peer/channel rename**: `ME` → `Schwung` (breaking change for
  existing Live sets).
- **Configurable Skipback length**: new "Skipback Len" setting in
  Audio (30s/1m/2m/3m/4m/5m). Changing the length preserves whatever
  audio is already in the rolling buffer.

## Pre-ship test plan

### 1 — Audio path rewrites

**MFX ME-only bus refactor** (`6a73e389`)
- [ ] Load a master FX (reverb/delay/compressor); play with several shadow
      slots active. Levels sensible (no clipping at normal volume, not
      whisper-quiet).
- [ ] Sweep master volume through its range — ME + Move mix balance
      smoothly; master FX wet tail follows.

**Unity view for capture** (`0639825f`, `caf1ce87`, `7516e483`)
- [ ] Master volume at 20%, hit Shift+Capture (skipback). Saved WAV at
      full level, not attenuated.
- [ ] Quantized sampler (Shift+Sample) with master at low volume —
      capture still loud.
- [ ] Native-bridge resample → Move's sample library at full level.

**Quantized sampler through unity_view** (`cddb2aa1`)
- [ ] Set Move's master to mid-volume, load a Schwung synth on a slot,
      play, hit Shift+Sample. Captured WAV contains synth at unity.

**Master volume under rebuild_from_la** (`047a3d60`)
- [ ] Move→Schwung on, play Move pad, adjust master volume. Output level
      follows master knob.

**Idle fast-path** (`db92d734`)
- [ ] Sit on main menu with no slots / no FX loaded for 2 min. Move's
      native audio passes through untouched, no CPU spikes in `spi_timing`
      logs.

### 2 — Long-press shortcuts (always-on per `330615eb`)

- [ ] Long-press Track 1..4: opens slot settings.
- [ ] Long-press Menu: opens master FX settings.
- [ ] Long-press Step 2: opens global settings.
- [ ] Long-press Step 13: opens tools menu.
- [ ] Long-press Jog click: opens overtake menu.
- [ ] Confirm none of these fire accidentally during normal Shift+Vol
      combos.

### 3 — Display / analytics / misc

- [ ] Soft-reboot display clear (`53933a66`): if a volume bar / screen
      reader overlay was showing before reboot, it's gone after.
- [ ] Analytics opt-out first-run prompt (`5f56cd25`, `aeedc89b`):
      fresh install (wipe `/data/UserData/schwung/features.json`), boot,
      see the prompt, choice persists.

### 4 — Link Audio migration

Critical path:
- [ ] Move audio → Schwung FX (reverb/delay on shadow slot). Wet signal
      audible, no regression vs pre-migration.
- [ ] Schwung synth → Schwung FX (Braids + cloudseed on a slot, pads
      from Move).
- [ ] Master FX processes combined output.
- [ ] No-FX passthrough: Move pad with no slot loaded — native Move
      audio unchanged.

Breaking change:
- [ ] **Live connections:** sets that received from `ME` / `ME-1..4` /
      `ME-Master` / `ME-Ack` need to reconnect to new names `Schwung` /
      `Schwung-1..4` / `Schwung-Master` / `Schwung-Ack`. Old-named
      connections silently route nowhere.

### 5 — Persistence fixes (recent)

- [ ] **Mute** (`395a9527`): mute a slot, power cycle, still muted.
- [ ] **Solo** (`395a9527`): solo a slot, power cycle, solo persists.
- [ ] **Session tempo** (`d563e24e`): set Move project tempo to e.g.
      87 BPM, power cycle, comes back at 87 not 120.
- [ ] **Routing toggle** (`8030bfda`): Move→Schwung routing off →
      Schwung synth+FX still audible. Back on → Move audio rejoins in
      a few seconds.

### 6 — Regression watch

- [ ] Recording (CC 118): load slot FX, play, hit record. WAV in
      `/data/UserData/schwung/recordings/` contains processed output.
- [ ] Skipback (Shift+Capture): after some play, trigger skipback; saved
      WAV correct.
- [ ] Quantized sampler (Shift+Sample): both resample source and Move-
      input source, clean captures.
- [ ] Overtake modules: load M8 or Controller, verify UI + MIDI works.
- [ ] First boot after fresh install: no SIGSEGVs in `debug.log`;
      `schwung-link-in attached` within ~10s; `Schwung` peer visible in
      Live within ~10s.

### 7 — Known residuals (don't chase)

- **~1 pop per couple minutes** mid-play on sustained Move audio through
  Schwung FX — Link/SPI clock drift; ASRC deferred. See
  `docs/link-audio-crackle-followup.md` and
  `docs/plans/asrc-revisit-prompt.md`.
- **1-bar quantize delay** on Move's Play button when Schwung Link is
  active — Link peer presence, structural.
- **Post-install "hollow" sound** — codec/jack-detect flake. Workaround:
  unplug/replug headphones.
- **Post-install "bitcrushed" sound** — codec state flake. Workaround:
  reboot.

### 8 — Soak

- [ ] **30–60 min mixed play** exercising MFX, slot FX, Move audio,
      Schwung synths, volume sweeps, toggling Link routing, capture /
      skipback / sampler. If nothing crashes or goes silent, ship.
