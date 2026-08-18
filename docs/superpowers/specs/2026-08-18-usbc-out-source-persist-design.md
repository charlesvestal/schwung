# Persist Move's USB-C Audio-Out Source (Mic / Main Out)

**Date:** 2026-08-18
**Status:** Approved, ready for implementation
**Branch:** `usbc-out-persist`

## Problem

Move's Settings menu has a USB-C audio-out source with two values, **Mic** and
**Main Out**. It controls what the connected computer receives over USB-C.
Move's firmware does not persist it: every reboot it reverts to **Mic**, and the
user has to re-pick **Main Out** by hand.

Schwung should make the user's choice survive a reboot.

## Prior art

Two existing bodies of work bear on this, and both were partly wrong about it.

**movesniff** (`~/Downloads/movesniff-withdocs/docs/protocol-table.md`) reversed
the XMOS audio-IO envelope:

```
F0 00 21 1D 01 01 37 <inner[15]> F7        (23 bytes total)
```

The 15-byte inner buffer holds up to five TLV entries of `<key> <lo7> <hi7>`
(14-bit values), zero-padded. Known keys: `0x12` = input routing + monitoring
(bit0 = USB-C input vs analog, bit1 = monitoring), `0x14` = "monitoring state
bit, XMOS-side semantics still being reversed", `2`/`3` = input preamp gain,
`4`/`5` = speaker volume, `10`/`11` = headphone volume. The findings doc lists
the meaning of `0x14` as open question Q2.

**vimana2-rust** (`crates/vimana-platform/src/move_hw/schwung_ui.rs:356`)
concluded the feature was impossible:

> "There is no known SysEx to safely control the USB-C output source (mic vs
> mix) without side effects on speaker output. Both sub=0x12 bit1 and sub=0x14
> cause the XMOS to mute speakers (feedback prevention)."

Its `set_usbc_output()` is therefore a no-op — software-only.

## Capture (2026-08-18, this device)

Method: `touch /data/UserData/schwung/log_xmos_sysex_on` (the tap at
`src/schwung_shim.c:4977`, which logs every cin 0x04–0x07 MIDI_OUT packet),
then toggle the setting. Idle traffic is zero, so every line is user-caused.
Sequence performed: Main Out → Mic → Main Out (default is Mic). Raw capture
retained alongside this spec.

Each toggle emits **a pair** of messages in a single SPI frame:

| Setting | Message 1 | Message 2 |
|---|---|---|
| **Main Out** | `F0 00 21 1D 01 01 37 12 02 00×12 F7` | `F0 00 21 1D 01 01 37 14 01 00×12 F7` |
| **Mic** | `F0 00 21 1D 01 01 37 12 00 00×12 F7` | `F0 00 21 1D 01 01 37 14 00 00×12 F7` |

Three events at f12827 / f14820 / f16977, values `02` / `00` / `02`, matching
the toggle sequence exactly. Identical in the PRE (shadow) and POSThw
(hardware) views, so these are Move's own bytes, not something Schwung
introduced.

### What this resolves

"USB-C Out: Main Out" is precisely the two bits vimana2-rust gave up on. The
speaker muting it observed is not a side effect — it *is* the feature: routing
main out to USB-C engages the monitor path, and the XMOS mutes the speakers to
prevent a feedback loop. This also answers movesniff's open question Q2: `0x14`
tracks the USB-C-out source alongside `0x12` bit1.

### What this constrains

- **The pair is atomic.** Both messages, same setting, same frame. Schwung's
  existing injector can only send `37 12`, which would set it half-way.
- **`37 12` bit0 is the *input* route bit**, shared with Move's sampling-page
  source toggle. It read `0` (analog) throughout. Recording the whole observed
  payload rather than synthesizing one means that bit can never desync from
  what Move last wanted.

## Design

Schwung never synthesizes the message. It records the exact bytes Move emits
and replays them at boot.

### New module: `src/host/shadow_xmos_audio.{c,h}`

One responsibility — observe, persist, and replay the XMOS audio-out setting.
A separate file keeps this out of `schwung_shim.c`, which the cleanup review
already flags as oversized.

| Entry point | Thread | Behavior |
|---|---|---|
| `xmos_audio_observe(midi_out)` | RT, per frame | Walks the 20 MIDI_OUT slots with a SysEx reassembler (cin 0x4 = 3 bytes; 0x5/0x6/0x7 = end with 1/2/3). On a complete `37 12` or `37 14` envelope, copies the payload into a volatile snapshot and bumps a sequence counter. No allocation, no I/O, no locks. |
| `xmos_audio_worker_tick()` | Worker (SCHED_OTHER) | If the sequence moved, persists both payloads to `config/xmos_audio_out.json`. All file I/O lives here. |
| `xmos_audio_worker_boot_arm()` | Worker, ~5 s after start | Reads the file and publishes a pending replay. **If the stored value is the Mic default, does nothing at all.** |
| `xmos_audio_emit_pending(midi_out)` | RT, per frame | Emits one of the two messages into empty slots, the other on the next frame, then clears. |

Mirrors the existing `shim_inject_boot_jack` arming pattern
(`src/host/shim_worker.c:20`, consumed at `src/schwung_shim.c:6304`).

### Persistence format

`/data/UserData/schwung/config/xmos_audio_out.json` — the two full payloads as
hex, plus a decoded field for legibility and for the settings UI:

```json
{
  "usbc_out": "main",
  "sysex_12": "F0 00 21 1D 01 01 37 12 02 00 ... F7",
  "sysex_14": "F0 00 21 1D 01 01 37 14 01 00 ... F7"
}
```

The hex payloads are authoritative on replay; `usbc_out` is display only.

### Emission constraints

MIDI_OUT is 20 slots / 80 bytes total, and each message is 8 packets. Sending
the pair in one frame would occupy 16 of 20 slots and starve Move's LED and
knob traffic. Therefore:

- One message per frame, never both.
- Empty-slot search only — never a blind write to a fixed offset.
- If 8 slots aren't free, defer to the next frame rather than overwrite.

### Safety fix to existing code

`src/schwung_shim.c:4931` (the `spi_sysex_inject` debug trigger) blind-writes
`out[0..31]` without checking occupancy, unlike the LED and jack-inject paths.
Per `memory/hollow_audio_jack_boot.md`, a sloppy XMOS injection hard-powered-off
the device twice. Since this design fires an XMOS injection automatically at
boot rather than only by hand, that path is rerouted through the new safe
emitter — keeping the debug affordance, removing the footgun.

### Boot ordering

The movesniff boot capture shows Move asserting its own
`37 12 route=analog mon=off` at ~0.6 s and `37 14 bit=0` at ~0.64 s. The replay
at ~5 s lands well after. The delay is a single constant if hardware disagrees.

### Global Settings

An item under Audio — "USB-C Out: Mic / Main Out" — reading the persisted
value. Changing it there emits the pair immediately through the same emitter
and persists. Changing it from Move's own menu instead is picked up by the
sniffer, so the two surfaces cannot drift.

## Testing

**Host unit tests** (`tests/host/`, the CI-gated suite):

- Reassembler: correct extraction of `37 12 02` / `37 14 01`; LED SysEx (`3B`)
  ignored; fragments at non-contiguous slots handled; truncated envelopes
  rejected; a buffer of pure zeros produces nothing.
- Emitter: never overwrites an occupied slot; defers when fewer than 8 free;
  emits message 1 then message 2 then stops.

**Hardware:**

- Set Main Out, reboot, confirm USB-C carries main out and speakers mute.
- Re-arm `log_xmos_sysex_on` across a reboot and confirm our pair appears on
  the wire *after* Move's own boot assert.
- Confirm a Mic-default device emits nothing at all at boot.

## Risks

- **Move re-asserts later than 5 s** (e.g. on set load) and overrides us. The
  reboot capture settles this; it's a constant, not a redesign.
- **Future firmware changes the encoding.** Sniff-and-replay degrades
  gracefully — Schwung replays whatever it last observed, without needing to
  understand it.

## Out of scope

- USB-C audio *input* routing (`37 12` bit0) — already handled by Move's
  sampling page; we preserve whatever it set.
- Input preamp gain, speaker/headphone volume (other `0x37` TLV keys).
- Per-set or per-project scoping — this is one device-wide value.

## Docs to update

`CLAUDE.md`, `docs/API.md` (if the setting gets a JS accessor),
`src/shared/help_content.json`, `../schwung-catalog-site/manual.html`.
Worth also feeding the `0x14` finding back to the movesniff protocol table,
which currently lists it as unreversed.

---

## What was actually built (2026-08-18)

Two refinements to the design above, both adopted during implementation.

### 1. Persist a preference, not the raw payloads

The design proposed storing both 23-byte payloads and replaying them verbatim.
That would also re-assert `37 12` bit0 — the USB-C *input* select owned by
Move's sampling page, which Move does **not** restore at boot. Replaying it
would silently change the user's recording input.

Implemented instead: persist a single value (0 = Mic, 1 = Main Out) to
`/data/UserData/schwung/usbc_out_state`. At replay, take the `37 12` payload
Move itself emitted this boot and flip only bit1; `37 14` is synthesized since
it carries nothing else. Bit0 stays authoritative from Move. Verified on
hardware: Move asserted `37 12 00` at f4, our replay sent `37 12 02`.

### 2. Persistence is gated for ~7 s after boot

Not in the original design, and found only by testing on hardware. Move asserts
its Mic default at ~0.6 s into every boot, carrying no user intent. Without a
gate, the observer persisted it and clobbered the stored preference — the
feature would have appeared to work, then failed silently on the second reboot.
The shim also observes its *own* replay, since emit runs earlier in the same
`pre_transfer` than scan.

Both are suppressed by gating persistence on `tick >= 35` (~7 s) and clearing
the pending value when the replay arms. Accepted trade-off: a change made in
the first ~7 s of boot is not persisted.

### Emission is stricter than specified

The design said "empty-slot search only". Code review found that insufficient:
free slots that are not *contiguous* let our SysEx splice into a message already
in flight, corrupting both — the same hazard `schwung_shim.c` already guards for
RNBO. `xmos_audio_emit` therefore requires a contiguous run, refuses while any
cable-0 SysEx is mid-flight, and never partial-writes. On hardware this defers
emission through Move's boot LED storm — the replay landed at frame 866 rather
than immediately, which is the retry path working as intended.

### Move's UI cannot be brought into agreement

Task 4 established that Move never persists this setting anywhere: no key in
`Settings.json` (whose mtime predated the user's change), and the dialog is
`ListViewDelegate<UsbAudioOutputSourceDelegate, NullTransactionPolicy>`. Its
screen keeps reading "Mic" after a replay. Confirmed end to end: a connected
computer receives Main Out while the screen says Mic.

This makes the deferred Global Settings row worth building after all — with
Move's screen showing a stale value there is otherwise no honest place to read
the truth. Shape (per the user): a persistence toggle, not a second Mic/Main Out
control, e.g. `Persist USB-C Out — On (currently: Main Out)`.
