# Co-run

Co-run lets an overtake tool share Move's control surface with a second UI for
the duration of a single user-driven session — e.g. a sequencer keeps the
pads + steps + transport while the Schwung chain editor takes the OLED + jog,
or while the Move firmware's native preset/synth editor takes its knobs.

Two co-run targets ship today:

| Target                    | Peer UI                                 |
| ------------------------- | --------------------------------------- |
| `CORUN_TARGET_CHAIN_EDIT` | Schwung shadow_ui's chain-slot editor   |
| `CORUN_TARGET_MOVE_NATIVE`| Move firmware's native preset / synth editor |

Both targets share the same default split, encoded as `CORUN_KEEP_DEFAULT =
PADS | STEPS | TRANSPORT | MENU` — the tool keeps those control-surface
groups and cedes everything else (OLED, jog, track buttons, knobs 71-78,
master CC 79, Shift, touch notes 0-9) to the peer. Back is framework-reserved
as the exit gesture by default; see [Exit gesture](#exit-gesture). Tools can
override by passing an explicit `keep_mask` to `shadow_corun_begin()`.

## Exit gesture

**Back exits co-run by default.** This matches the rest of Move's "Back pops
one layer" semantics. The framework intercepts Back from the user, calls
`shadow_corun_end()`, and never lets the event reach either the tool or the
peer. (`menu` keeps its existing duties outside co-run: tap-dismisses shadow
UI; Shift+Vol+Menu opens Master FX. While a co-run is active, Menu is
forwarded to the tool by default via `CORUN_KEEP_DEFAULT` so the tool can use
it as its own affordance.)

### Opting out — `CORUN_KEEP_BACK`

Tools that need Back free for in-session peer navigation (sub-view pop in
the chain editor, native back-navigation inside Move firmware) set
`CORUN_KEEP_BACK` in `keep_mask`:

```js
shadow_corun_begin(CORUN_TARGET_CHAIN_EDIT, slot,
                   CORUN_KEEP_DEFAULT | CORUN_KEEP_BACK);
```

When this bit is set, the framework leaves Back alone: Back routes per the
normal `keep_mask` rules (cedes to peer unless `CORUN_GRP_BACK` is also kept).
For `CORUN_TARGET_CHAIN_EDIT`, shadow_ui's own Back handler still ends the
session when the chain editor is at its top-level view (`CHAIN_EDIT`) — it
owns the view stack and can tell, so it provides Charles's "Back exits at
top, navigates within" UX for free even under the opt-out. For other
targets like `CORUN_TARGET_MOVE_NATIVE`, the peer UI's depth isn't
observable from the framework, so the tool is responsible for its own
exit gesture (typically Menu, which is tool-routed under the default
keep-mask) and for calling `shadow_corun_end()` itself.

`CORUN_KEEP_BACK` lives in the high half of `keep_mask` (bit 15) so it doesn't
collide with any `CORUN_GRP_*` bit.

## JS API

Exposed on the global object in shadow_ui's JS runtime:

```js
shadow_corun_begin(target, id, keep_mask)
  // target: CORUN_TARGET_CHAIN_EDIT | CORUN_TARGET_MOVE_NATIVE
  // id:     chain slot 0-3 (CHAIN_EDIT) or tool track 0-7 (MOVE_NATIVE)
  // keep_mask: bitfield of CORUN_GRP_* the tool KEEPS; 0 = default split

shadow_corun_end()
  // Tear down co-run. Called by the framework on Back, or by the tool to
  // exit programmatically (e.g. on track-mode change).

shadow_corun_state()
  // Returns { target, id, keep_mask } or null when no co-run is active.
  // Tools poll this each frame to detect framework-driven exit and to
  // reconcile their own mirror state.
```

Enum constants are registered as JS globals: `CORUN_TARGET_NONE`,
`CORUN_TARGET_CHAIN_EDIT`, `CORUN_TARGET_MOVE_NATIVE`, plus
`CORUN_GRP_OLED` ... `CORUN_GRP_TOUCH`, `CORUN_KEEP_DEFAULT`, and
`CORUN_KEEP_BACK` (matching `shadow_constants.h`).

### Capability gate

Tools that want to ship from a single branch against both stock and patched
Schwung should gate the user-facing entry on the API's presence:

```js
const corunAvailable = typeof shadow_corun_begin === "function";
```

## Move-firmware coupling

`CORUN_TARGET_MOVE_NATIVE` runs as a pure shim-level split: Move firmware
reads the shadow MIDI_IN buffer directly (separate process), so there's no
JS-side intercept or host-API swap to manage. `shadow_swap_display`
early-returns when `shadow_display_owner == DISPLAY_OWNER_MOVE_FIRMWARE`,
handing the OLED to Move's framebuffer while `shadow_display_mode` stays
armed.

**Why the bypass exists:** the obvious alternative — setting
`shadow_display_mode = 0` to expose Move's framebuffer — also disables the
MIDI filter at the `sh_midi` sync site, which would let the tool's pads,
step buttons, and transport leak through to Move firmware. Splitting
"session active" (`shadow_display_mode`) from "who renders"
(`shadow_display_owner`) lets us yield the OLED without tearing down input
gating.

One Move-specific accommodation lives in the shim: **CC 71-78 detents are
coalesced per audio frame** before being forwarded to Move firmware. Move
spends ~900µs per knob CC on a synth-param write plus OLED redraw; multiple
detents in a single frame stack their cost and overrun the SPI frame budget,
manifesting as sequencer stutter while the user spins a knob. The shim sums
incoming detents per knob within each frame and emits one consolidated CC,
clamped to the one-byte signed delta range (±63 — leftover spills naturally
to the next frame's accumulator). Tools that keep `CORUN_GRP_KNOBS` in their
own `keep_mask` are unaffected (knob CCs never reach Move in that case).

Per-frame collapse is the framework's contract. Tools that generate unusually
heavy concurrent MIDI traffic (e.g. simultaneous pad fire, step LEDs, and
automation lanes during transport) may still see residual stutter on very
fast knob spins because the per-frame consolidated CC plus the rest of the
tool's MIDI is enough to pressure Move's SPI window. That's a tool-side
characteristic worth documenting in the consumer's manual; pushing the
coalesce to multi-frame intervals trades knob latency for a problem most
tools won't hit.

### LED ownership

For symmetry with input routing, Move's LED writes are gated by `keep_mask`
during `CORUN_TARGET_MOVE_NATIVE`: Move's outbound CC / note-on / note-off
LED messages for any surface group the tool **keeps** (per `keep_mask`) are
stripped before reaching hardware, so the tool's own LED rendering on those
surfaces stays uncontested. Surfaces the tool **cedes** pass through —
Move's LEDs reach the buttons / pads / knob rings directly. Sysex LED writes
aren't classified by group and pass through unchanged; the framework leaves
sysex (and the palette entries it carries for knob-ring + master colors,
idx 71-79) alone.

## Single source of truth

A single predicate, `corun_event_owner(ctrl, type, d1) -> {TOOL, PEER, BOTH,
NONE}` in `shadow_constants.h`, decides which side any given control-surface
event belongs to right now. Both the sh_midi let-through filter (forward to
Move) and the forward-to-shadow_ui suppress filter (forward to tool) call this
helper and switch on the result, so the two routes cannot drift apart. Adding
a new target = extend this function; no mirror checks elsewhere.
