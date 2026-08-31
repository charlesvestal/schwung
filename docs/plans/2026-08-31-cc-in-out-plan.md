# CC In / CC Out — readiness plan

**Date:** 2026-08-31
**Status:** Plan only. Nothing here is built.
**Trigger:** Anticipated Ableton support for (a) Move receiving CC to control its own
devices and (b) Move tracks sending CC out. Both would open up movy controlling Move
devices, and Move's native sequencer automating Schwung modules.

---

## 1. What already exists

This was the surprise of the investigation and it changes the scope substantially.
Schwung already has most of a CC system; it is just narrow.

| Capability | Where | Shape |
|---|---|---|
| CC in, absolute | `src/modules/chain/dsp/chain_midi.c:829` | CC **102–109** on a slot's receive channel → that slot's 8 chain knobs, scaled to each param's declared range. Consumed, not forwarded. |
| CC in, relative | `chain_midi.c:756` | CC **71–78** → same 8 knobs as encoder deltas, with time-based acceleration; enums never accelerate. |
| CC out | `chain_params.c:991` (`knob_emit_cc_out`) | Per-patch opt-in `knob_cc_out`, default off. Emits CC 102–109 on the slot's recv channel, cable 2. Change detection at CC resolution; a dropped send deliberately leaves `last_cc_out` unrecorded so it self-heals. |
| Mapping storage | `chain_internal.h:100` (`knob_mapping_t`) | 64 bytes: `{cc, target[16], param[32], current_value, last_cc_out}`. `MAX_KNOB_MAPPINGS 8`. Persisted in the patch JSON (`chain_patch.c:1208`). |
| Modulation bus | `chain_mod.c:382` (`chain_mod_emit_value`) | Multi-source, non-destructive, base-value snapshot, range-scaled, `MAX_MOD_TARGETS 32` × `MAX_MOD_SOURCES_PER_TARGET 8`. LFO1/LFO2 are just two sources on it. **A CC source would be a third.** |
| Param metadata | `chain_params` contract | type / min / max / step / options for ~every fleet module. The LFO target picker already walks it hierarchically (`lfo_target_groups.mjs`). |
| Emission primitives | `plugin_api_v1.h:160,189` | `midi_send_external` (cable 2) and `midi_inject_to_move` are in `host_api_v1_t` — available to every module, not just the chain. |

**So the gap is not "Schwung has no CC support."** The gap is that only 8 params per
slot are addressable, only at four fixed CC numbers, only inside the chain.

## 2. The finding that matters most

**A Move track's CC out may not reach us at all, and we can say exactly why.**

- `schwung_shim.c:1298` — the MIDI_OUT voice-message reader does `if (cable != 2) continue;`.
  Cable 0 (Move's own output) is skipped for **every voice message**.
- `schwung_shim.c:1237–1270` — cable 0 is tapped **only** for system realtime
  (clock / start / stop). The comments record that this tap was deliberately moved
  *from* cable 2 *to* cable 0, because cable 2 only exists when the user enables
  Clock Out. That reasoning applies equally to track CC.

Two futures:

| Where Ableton puts track CC | What we need |
|---|---|
| **Cable 2** (external USB out) | Nothing. It reaches slots today via the cable-2 MIDI_OUT reader; echo detection compares against MIDI_IN cable 2, so Move-originated CC will not false-positive as an echo. |
| **Cable 0** (alongside notes to Move's own instruments) | **A new cable-0 voice tap.** Nothing currently sees it. |

Cable 0 is the likelier one, by the same argument that moved the clock tap there.

**Hazard to design against when that day comes:** a cable-0 voice tap has the same
shape as the bug that broke song-mode for a month — injected traffic returning as
input and re-triggering. Any such tap must be **CC-only, never notes** (pads already
reach slots by another route) and guarded by the existing recent-dispatch ring.

## 3. What we are deliberately NOT building

Recorded so it is not re-proposed. Each of these was in an earlier draft of this plan
and was cut for the stated reason.

| Cut | Reason |
|---|---|
| `cc_binding.h` — a swappable "what does this CC mean" adapter layer | Abstraction designed for an unknown. If the format surprises us, a `switch` in one function is the same hedge and costs an hour to write **then**, against facts. |
| Cable-0 CC tap | Untestable until Move emits track CC. Build it when we know it is needed and can verify it fires. |
| `midi_inject_to_move` for CC (Schwung → Move devices) | Literally inert until Move listens. Would ship dark and unexercised. |
| Mod bus → CC out (LFO sweeping external gear) | Genuinely nice, entirely unrelated to Ableton. Belongs in its own spec, on its own merits, with its own rate-limit design (20-packet mailbox, 344 CC/s at block rate). |
| Bidirectional emit-on-change across all mappings | A motorised-controller feature nobody asked for. `knob_cc_out` already covers the case that exists. |
| Host-side universal CCs (sustain 64, mod wheel 1, expression 11, all-notes-off 123) | Would double-apply in modules that already handle them. **Blocked on the audit** in §4.3, which is the thing that tells us who does. |
| Fleet PRs (`cc_native`, per-param `cc` hints) | Blocked on the same audit. Writing 100 PRs against a guess is the expensive mistake. |

## 4. What is worth doing, none of it a bet

Three workstreams. None depends on Ableton shipping anything. Each is independently
useful and independently shippable.

### 4.1 Arbitrary CC → arbitrary param

**The problem today is not the count of 8, it is the fixed numbers.** A controller
sending CC 21–28 cannot touch a Schwung slot at all.

Scope:

- Generalise `knob_mapping_t` → `cc_mapping_t`: add `uint8_t channel`, `uint8_t mode`,
  `uint8_t flags`. Raise `MAX_KNOB_MAPPINGS` 8 → 32 (2 KB/slot; not a concern).
- **Seed the first 8 entries with today's CC 71–78 / 102–109 defaults**, so every
  existing patch behaves identically and nothing migrates.
- Lookup runs **after** the legacy 71–78/102–109 block and **before** the fall-through
  to `synth->on_midi`. That ordering is what makes existing behaviour bit-identical.
  An explicit mapping onto 102–109 is a conflict to warn about, not a silent override.
- A mapped CC is **consumed, not forwarded** — the rule the CC 102–109 block already
  follows (`chain_midi.c:860`). Otherwise a module that natively handles CC 74 also
  moves its own cutoff and double-applies.
- Per-mapping mode, default **Absolute**:
  - `ABSOLUTE` — CC owns the value, 0–127 spans the declared range. What a sequencer
    lane means. **Excluded from the patch autosave snapshot**, so automation never
    rewrites the saved patch value.
  - `MODULATE` — emits on the existing bus as source `cc<n>` via
    `chain_mod_emit_value`. Non-destructive by construction, layers with LFOs, needs
    no autosave exclusion because the base is what gets saved.

UI:

- **Learn from the knob grid.** A gesture on the focused cell arms learn; wiggle the
  control; done. Reuses the cell the user is already looking at, so no target picker
  is needed for the common case.
- **A trailing "MIDI" page**, alongside My Presets and Module, listing existing
  mappings for review / mode toggle / delete. Per `docs/SHADOW_UI.md`, trailing pages
  are appended after the whole walk, never injected into a level.

Cap-raise hazard: per the standing rule, grep the **literal** bounds (`i < 8`, `[8]`,
the `chain_patch.c` parse loop, `shadow_ui.js`'s knob editor), not the constant.
Raising a chain cap has broken five sibling sites before.

### 4.2 CC observatory

~80 lines. Arm a file under `/data/UserData/schwung/`, log every distinct
`(cable, channel, cc)` the shim observes with a value-shape summary (fixed / ramping /
0–127 / 0,127 only), dump on demand. Off by default, disarmed like every other
diagnostic in `docs/DIAGNOSTICS.md`.

**This is the part that makes waiting productive rather than passive.** When the
Ableton beta lands, point it at a Move track with an automation lane and know the
answer in one session — cable, channel, numbering, resolution — instead of
reverse-engineering the format while simultaneously writing the feature against it.

Independently useful today: "what is this controller actually sending" is a recurring
support question with no current answer.

### 4.3 Fleet CC audit

Host-side, no device, no Ableton. `dlopen` each catalogued module on the Mac, feed
every CC 0–127 on every channel, diff `chain_params` before and after. Report per
module: which CCs it consumes, which params they move, which it ignores.

Precedent: a DSP contract is already dumpable on the Mac this way, and the fleet audit
sheet (PR #333) established the shape of a whole-fleet report.

Answers the two questions that gate everything deferred in §3:

1. Which modules already handle sustain / mod wheel / expression — i.e. can the host
   safely implement them universally, and who needs to opt out?
2. Which modules steal CC numbers a user is likely to want to map — the conflict list.

Known starting point: `schwung-dx7` routes only CC 1/64/123 and drops CC 2/4.
Expectation is that a large fraction of the fleet ignores sustain entirely.

## 5. When Ableton ships

The decision tree, written now so it is short then.

1. Run the observatory (§4.2) against a Move track with a CC automation lane.
2. **If track CC appears on cable 2** — verify it reaches a slot's mapping table
   unchanged. Likely zero code.
3. **If it appears on cable 0** — add the cable-0 voice tap: CC-only, never notes,
   echo-guarded by the recent-dispatch ring, behind a `features.json` flag.
4. **If it is 14-bit / NRPN / anything else** — that is where the one `switch` gets
   written, against a captured fixture rather than a guess.
5. Separately, for **Move receiving CC** (movy → Move devices): confirm with the
   observatory that Move responds, then wire `midi_inject_to_move`. Per the standing
   ownership rule, this needs its **own inject queue** — inject-queue ownership
   follows who pushed, and sharing one is what broke song-mode.
6. Only then write fleet PRs, informed by §4.3.

## 6. Sequencing

§4.1, §4.2 and §4.3 are mutually independent and can be done in any order or in
parallel. §4.2 is the smallest and has the longest lead time value (it wants to exist
*before* the beta), so it is the natural first.

## 7. Open questions

- **Learn gesture.** Which gesture on a knob-grid cell arms learn without colliding
  with peek / flip / dive. Needs a read of `docs/PARAM_PAGES.md` before deciding.
- **Master FX.** The mapping table as specified is per chain slot. Master FX is
  excluded from User Presets by deliberate design, in one helper. Same question here,
  same answer, or not — undecided.
- **Channel semantics for `channel = All (-1)` slots.** A mapping's channel field
  versus the slot's receive channel: which wins, and is a per-mapping channel even
  meaningful on an All slot.
