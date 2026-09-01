# Schwung Metronome — design

**Date:** 2026-09-01
**Status:** design, awaiting review
**Problem:** under Move→Schwung (`rebuild_from_la`) Move's metronome is inaudible.

## Why it is silent

`shim_post_transfer` sets

```c
int rebuild_from_la = (link_audio.enabled && link_audio_routing_enabled &&
                       shadow_chain_process_fx &&
                       shim_move_channel_count() >= 4 && la_receiving);
```

and when that holds it **zeroes the mailbox** and rebuilds it from Link Audio
slots 0–3 — the four per-track channels — so it can insert per-slot FX
(`src/schwung_shim.c:2347`). Move's Main channel is deliberately not subscribed:

> Skip Move's Main mix: the shim rebuilds Move's output from per-track slots 0-3
> and never consumes Main. Subscribing to Main still forces Move to publish it on
> its Audio Worker threads … Observed 2026-04-24: Main's ring overran.
> — `src/host/link_subscriber.cpp:477`

The metronome is mixed at Move's master, not into any track, so it is not in
slots 0–3 and it is by construction absent from our reconstruction. Nothing short
of generating our own click recovers it.

**Reconstructing it from Main is not an option.** `Main − Σ(tracks)` is not the
metronome: `Song.abl` carries `returnTracks` and a `masterTrack`, so the residual
is metronome + returns + master-chain colouring. It would also require
subscribing to the channel the sidecar avoids for measured reasons.

## Detection

`MoveOriginal`'s string table carries, in the middle of Move's notification
strings (`"Clip\ncreated"`, `"Notes\ndeleted"`, `"Loop length\nset to {}"`):

```
0x169474  "Metronome\nOn"
0x1909d8  "Metronome\nOff"
```

Move raises that notification on Shift+Step 6 and pushes it out as a
`com.ableton.move.ScreenReader.text` D-Bus signal. **`shadow_dbus.c` already
receives every one of those** through its catch-all `type='signal'` match
(`shadow_dbus.c:637` → `shadow_dbus_handle_text`), and existing features that
depend on that stream (native knob mapping, stock sampler-source tracking) work
with the screen reader switched off — so the signal is unconditional.

### Why this is not the mute-bug pattern

The removed `shadow_dbus.c` mute auto-correct matched **any** announcement whose
text *ended in* `" muted"` / `" soloed"`, so Move's own utterances ("Lay Down Kit
muted") and Schwung's TTS looping back through the same handler both hit it, and
it persisted the result. Here:

- the match is **exact equality** on two whole strings, not a suffix;
- Schwung never utters either string, so there is no loopback path;
- the flag is **not persisted** — it is runtime-only state in the shim.

### Rejected alternatives

| Approach | Why not |
|---|---|
| Step-6 icon LED on MIDI_OUT | The LED also lights while Shift is held and in other step contexts, so lit ≠ metronome on. Rejected on device knowledge. |
| Mirror the Shift+Step 6 gesture | Boot state unknown, and drifts against any change made outside the front panel. |
| Move's D-Bus API | `/com/ableton/move/settings` exposes only `isMoveRunning`. No transport or metronome object. Verified by introspection. |
| Persisted setting file | Not present. See below — this is the fact that closes the boot hole. |
| Read `mIsMetronomeOn` from process memory | The string is an **assert expression**, sitting among `iPos != mSteps.end()` and `mTempoAfterCapture`. No reflection table, no symbols (`MoveOriginal` is stripped), so no deterministic anchor. |

### Boot state

`metronome` appears in **neither** `/data/UserData/settings/Settings.json` nor
`Song.abl` (whose top-level keys are `$schema, stepEditorResolution, tempo,
globalGrooveAmount, timeSignature, rootNote, scale, melodicLayout, tracks,
returnTracks, masterTrack, scenes, grooves, metadata`). Move does not persist the
metronome, so it is **off at boot** — and initialising `shadow_metronome_on = 0`
is the truth rather than a guess.

### String matching

The exact wire text is unconfirmed (the binary holds the display form, with a
newline; the announcement may normalise it). Match defensively: lowercase,
collapse all whitespace runs to one space, trim, then compare against
`"metronome on"` / `"metronome off"`. Any other text leaves the flag alone.
Confirm on hardware when the build lands.

## Settings

The knob grid holds **8 params per page** (`KNOBS_PER_PAGE`); a section is not
capped at one page — the bank bar and page picker page through them like any
other grid — but a section that spills leaves an orphan page. Measured: adding a
9th param to Audio plans a second page named `Audio - 2` holding one lonely knob.

Audio is at exactly 8, so room is made rather than spilled. **`skipback_shortcut`
and `skipback_seconds` move to Shortcuts** — the first names the button combo and
the second its length, and Skipback is a shortcut feature (Shift+Capture). They
move as a pair; splitting them across two sections would be worse than leaving
both in Audio.

| Section | Before | After |
|---|---|---|
| Audio | 8 | 6 + `metronome_mode` + `metronome_level` = **8** |
| Shortcuts | 2 | 2 + `skipback_shortcut` + `skipback_seconds` = **4** |

Still seven pages, one per section, so sections-as-levels keeps holding.

| Key | Type | Options / range | Default |
|---|---|---|---|
| `metronome_mode` | enum | `Off` / `Follow` / `On` (short `OFF`/`FOL`/`ON`) | `Off` |
| `metronome_level` | int | 0-100 % | 50 |

- **Off** — never sounds.
- **Follow** — sounds while Move's metronome is on (`shadow_metronome_on`).
- **On** — sounds whenever the transport plays, regardless of Move's state.

`On` exists as the hedge: if the announcement text turns out to be shaped
differently on some firmware, the feature is still usable while detection is
fixed.

**In every mode the click sounds only while `rebuild_from_la` is active.** Outside
it Move's own metronome is audible, and this rule prevents doubling by
construction rather than by a second condition someone can forget.

Both values live in `shadow_control_t`, not `features.json`:
`load_feature_config()` runs once at init, so a value parsed there would need a
reboot to change — the same reason Recall Quantize's division is a control field.
The control buffer has headroom (256-byte container under a `<=` assert), so
adding two fields costs nothing and only shrinking fails the build.

## Generation

New `src/host/shadow_metronome.{c,h}`. RT-safe: no allocation, no file I/O, no
locks. State is a phase accumulator and a countdown.

- **Beat** — `shadow_transport_pulses % 24 == 0`, gated on
  `sampler_transport_playing`. Both are already driven from **cable 0**, which
  "always carries Move's transport state when running … independent of the user's
  MIDI Clock Out preference" (`schwung_shim.c:1273`), so no user setting can
  break sync. Timing quantises to the 128-sample block — 2.9 ms — which is
  inaudible as click jitter.
- **Downbeat** — `shadow_transport_pulses % (24 * beats_per_bar) == 0`.
  `shadow_transport_pulses` resets on MIDI Start (0xFA), which is bar 1 beat 1,
  so bar phase is free once `beats_per_bar` is known.
- **`beats_per_bar`** — from `timeSignature.upper` in the current set's
  `Song.abl`. The shim already tracks the current set
  (`sampler_current_set_name` / `sampler_current_set_uuid`,
  `src/host/shadow_set_pages.c:35`); the JSON read happens on the
  **`shim_worker` thread** on set change, never on the SPI callback, and lands in
  a `shadow_control_t` field. Falls back to 4 if the file cannot be read or
  parsed.
- **Sound** — decaying sine, ~1500 Hz on the downbeat and ~1000 Hz otherwise,
  ~30 ms exponential decay, generated inline. Scaled by `metronome_level`.

## Mixing point

Into `mailbox_audio` immediately **after**

```c
native_capture_total_mix_snapshot_from_buffer(unity_view);
```

and **before** the `rebuild_from_la && mv < 0.9999f` master-volume scaling
(`src/schwung_shim.c:2806`). That position means:

- the click is **not** in `unity_view`, so the Quantized Sampler, Skipback and
  the native resample bridge never record it — a resample stays clean;
- the click **is** scaled by master volume and gets speaker EQ, so it behaves
  like the rest of the DAC output.

## Known limitations, stated rather than hidden

- **Count-in is not covered.** With `isUsingCountIn` set, Move plays a one-bar
  count-in click before recording *even when the metronome is off*, and that is
  equally silent under `rebuild_from_la`. There is no announcement for it, and
  Record + transport is not a sufficient signal. Out of scope for v1.
- The click is Schwung's, not a reproduction of Move's sound.
- Detection needs a toggle to be observed. It is correct from boot (metronome
  starts off), but if a user somehow reaches a state we did not observe, `On`
  mode is the escape hatch.

## Tests

- `tests/host/` unit for the announcement matcher: the two exact strings and
  their whitespace/case variants flip the flag; near misses
  (`"Metronome"`, `"Metronome On Track"`, `"unmuted"`) do not. Mutate the
  matcher to prove the test can fail.
- `tests/host/test_global_settings_contract.sh` updated for the moved pair:
  `WANT_COUNT` becomes audio 8, shortcuts 4, and the "exactly 7 pages" and
  "Audio is at KNOBS_PER_PAGE exactly" assertions must still pass unchanged —
  they are what would catch a spill.
- Beat/downbeat boundary maths in a header so `tests/host/` can run it, the way
  `recall_quantize.h` is: pulse 0 is a downbeat, `24*n` is a beat, a
  `beats_per_bar` of 3 accents every third.
- Assert the click is absent from `unity_view` and present in `mailbox_audio` in
  the same frame.

## Docs to update

`CLAUDE.md` (one bullet), `docs/SHADOW_UI.md` (the section, and why it is its
own), `src/shared/help_content.json`, `../schwung-catalog-site/manual.html`.
