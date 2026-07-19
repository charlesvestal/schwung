# Design: a chain MIDI FX cleanly "owning" a Move native track (arp → Move)

Status: **design only** (Tier 2). Tier 1 — the arp-hang fix — shipped separately
(see "Tier 1" below). Date: 2026-07-15.

## Problem

Put an **arp** (or any generative MIDI FX) on a shadow chain slot with "MIDI out
to Move" (Pre mode) on, hold pads, and you want **Move's native track to play the
arpeggiated pattern**. Today it can't — at best you get the held-pad *drone* plus
whatever arp pitches happen to differ from held pads. This is the long-deferred
"arp → Move" problem (see memory `midi_fx_to_move_status`: 7 echo-filter
approaches tried, arp deferred).

## Why it's hard — the coupling the routing trace exposed

Traced 2026-07-15 (agent trace over shim + shadow_midi + chain):

1. **The arp's *input* is Move's own output.** A pad press on a Move track is
   mapped pad→musical-note by Move's firmware and emitted as a **musical note on
   MIDI_OUT cable-2**, which the shim dispatches to the slot
   (`shadow_midi.c:866-951`, `shadow_dispatch_cable2_channeled_slots`). The chain
   never sees the raw pad (68-99) or cable 0.
2. **That same processing IS the drone.** Move's track sums the held pad note
   (cable-0 pad path) with anything the chain injects on cable-2
   (`ADDRESSING_MOVE_SYNTHS.md:27`, "additively"). So a held pad drones under the
   arp.
3. **Killing the drone feeds back.** Any note-off that stops Move's held-pad voice
   (cable-0 pad-off *or* cable-2 musical note-off) makes Move emit a **cable-2
   musical note-off** — indistinguishable from a real pad release — which
   re-enters the arp and drains its held notes. (This is exactly the Tier 1 bug,
   one layer up.)

So "kill the drone but keep the arp's held note" means distinguishing our-kill
from a-real-release when both are identical on the wire. Unresolvable head-on.

## Key existing primitive found

- **`host_pad_block()` / `shadow_control->pad_block`** already exists: the shim
  **zeroes pad notes 68-99 in MIDI_IN before Move's firmware reads them**
  (`schwung_shim.c:5926-5945`). This is a ready-made "suppress Move's native pad"
  switch — but note that with it on, Move never processes the pad, so it never
  emits the musical note the slot currently relies on for input.
- **Cable-0 pitched injections don't reach track instruments**
  (`shadow_midi.c:658-670`): "cable 0 is reserved for Move's pad/button prefix
  protocol and won't reach track instruments for pitched notes." So we can't just
  inject arp *notes* on cable 0 to play them.

## Proposed architecture — decouple input from the echo

> **pad_block ON** for the owning slot (Move never drones, never emits cable-2
> pad output) **+ feed the arp from the raw cable-0 pads** (dispatch pads 68-99
> to the slot, mapped to pitch) **+ arp injects its pattern on cable-2** to drive
> Move's track.

Because the arp now listens to **cable 0** and Move's injection echoes are on
**cable 2**, the arp never hears its own echo — the whole echo-filter class of
bugs disappears (no refcounts, no drone, no stuck notes from feedback).

```
Pad (cable-0, 68-99) ──► [shim: pad_block ON]
   │                         └─► Move firmware: SUPPRESSED (no drone, no cable-2 out)
   └─► [new] dispatch raw pad to slot ──► pad→pitch map ──► arp held notes
                                                              │
                    arp pattern ──► inject cable-2 (musical pitch, recv ch) ──► Move track plays the ARP
                                                              │
                    Move echoes on cable-2 ──► arp doesn't listen to cable-2 ──► no feedback
```

## Work required

1. **Per-slot "own the track" mode.** A capability/flag on the slot (or the MIDI
   FX) that opts a slot into ownership. Only then do we pad_block + reroute.
   Additive Pre mode stays the default for chord/harmonizer.
2. **pad_block scoping.** Today `pad_block` is global (all pads 68-99). Ownership
   is per-track. Either make pad_block per-channel/per-track, or accept
   whole-surface blocking while an owning slot is active (simpler, more limiting).
3. **Cable-0 → slot dispatch (new plumbing).** Route raw pad note-ons/offs
   (68-99, cable 0) to the owning slot's MIDI FX as its note source, in addition
   to / instead of the cable-2 musical-note path.
4. **pad→pitch mapping.** Map pad number → musical pitch the way Move would
   (scale/key/octave). Options: read Move's current scale settings; or mirror the
   mapping; or expose the mapping from firmware if available. This is the fiddly,
   Move-settings-dependent part.
5. **Recording semantics.** Decide what Move records: the arp stream (probably
   yes, since Move's track is now driven by the arp) — confirm against #150's
   record-align work (`ec849342`).

## Alternatives considered (rejected)

- **Synthetic pad note-offs to kill the drone** (cable-0 or cable-2): rejected —
  Move responds with a cable-2 musical note-off identical to a real release →
  feeds back into the arp (the Tier 1 bug via a new door), plus stuck-note risk
  if Move then omits the real-release note-off, plus pad-state/LED/record desync.
- **Cable-2 note-off with a note-off refcount** (track injected offs, filter their
  echoes): survives the echo but still *silences* Move's held pad and, because
  the arp's note-ons for held pitches are skipped, never replays them → held
  pitch stays silent on Move. Doesn't achieve a clean arp.

## Tier 1 (shipped) — arp survives, additive

The immediate fix (in this same session): the Pre-mode inject skip was asymmetric
— it skipped note-**ONs** on held-pad pitches but injected their note-**OFFs**,
which were echoed back un-refcounted and drained the arp (plays once, stops).
Fix: only inject a note-OFF for a pitch we actually injected a note-ON for
(`chain_pre_tick_should_inject` in `chain_pre_inject.h`, unit-tested in
`tests/host/test_chain_pre_inject.c`). Result: arp **survives** with Move-out on,
behaving as documented "additive" (Move's track = held-pad drone + the arp's
non-held-pitch notes). Not a clean arp — that's what Tier 2 is for. Scoped to the
tick inject path; chord (note-driven) and clock-driven-generator paths unchanged.

Residual after Tier 1: a tick FX that emits a note-OFF for a pitch it never
injected a note-ON for (e.g. across an FX-swap state reset) would have that
note-OFF dropped → a possible stranded voice on Move. Rare; acceptable vs. the
arp-hang it replaces.
