# The CC Map — assigning MIDI CC to parameters

Split out of `CLAUDE.md`, which points here.

Covers the per-slot and Master FX CC maps, the card that edits them, how an
assignment is stored and replayed, and the numbers that cannot be used.

## The map is what a user chose, not what a machine guessed

The first version assigned a CC to **every** parameter automatically, in
declaration order. It was worse than nothing on both counts:

- One 97-parameter synth consumed every usable address, so an FX behind it in
  the same slot got none at all — and that FX is often exactly what you want a
  knob on.
- The numbers you did want were buried among ninety you did not, and
  declaration order bears no relation to the pages a user sees. CC 12 landed on
  a hidden note-map enum while the first knob on screen sat at CC 40.

So nothing is assigned until someone assigns it. Every controllable parameter is
listed with `--` until it is given a number, by jogging one or by learning one
from a controller. The map lists what CAN be controlled; the user says what
matters.

That decision also removed the address shortage that justified per-component
MIDI channels: with only what you actually use assigned, 110 numbers per slot is
not a constraint anybody reaches.

## Two maps, two owners

|  | Slot | Master FX |
|---|---|---|
| Table lives in | the chain DSP (`auto_cc_t`) | the host (`mfx_cc_t`) |
| Parameter range | read from `chain_params` | **travels with the assignment** |
| Persisted in | the slot patch (`cc_overrides`) | `shadow_config.json` |

Master FX is not a chain — each position is a standalone audio-FX plugin — so
none of the chain machinery applies and the table is kept host-side. The
parameter's range is sent **with** the assignment rather than looked up when a
CC arrives: the alternative is a second JSON parser on a path that also has to
stay cheap enough for the MIDI callback, and the UI has already parsed
`chain_params` to draw the page.

## Order and naming come from the module's own layout

Rows follow `ui_pages` / `ui_hierarchy`: each `knobs` array is a page, in the
order the module draws them, and the nearest `name`/`label` before it is that
page's name. So an EQ that declares four parameters called `Gain` reads as
`Band 1 Gain` … `Band 4 Gain`, with no module-specific knowledge anywhere.

**A module may publish its layout under either key.** `ui_hierarchy` is the
documented one; `ui_pages` is what several modules actually export. Asking for
only one returns nothing and the caller cannot tell an absent layout from a
differently-spelled one — which showed up as an EQ listing four identical rows.
Both the chain and the Master FX read path try both.

The same applies to `chain_params`: `module.json` may declare none because the
module serves its contract from the DSP at runtime. Every component kind —
synth, audio FX, MIDI FX — needs the fallback, and the FX ones must ask **after**
`fx_count` includes the slot, or the refresh rejects it and the list is silently
empty.

Names are title-cased, but only words of four letters or more: `LF`, `HF`,
`LMF` and `HPF` are acronyms, and `Lmf Gain` is worse than the shouting it
replaced. Anything already mixed-case is left alone.

## Numbers that cannot be assigned

`chain_cc_reserved()` — 18 of 128, one definition asked by the assign path, the
learn path and the card's jog:

| | |
|---|---|
| `0`, `32` | bank select MSB/LSB — a 14-bit pair, not a control |
| `71`–`78` | Move's own chain knobs, relative |
| `102`–`109` | the same eight knobs, absolute |

The knob ranges are refused **outright**, not merely while a slot has knob
mappings. Both legacy blocks run before the map in `chain_midi.c`, so a
parameter assigned there works right up until somebody assigns a chain knob and
then silently stops. A number that works until something unrelated changes is
worse than one that cannot be picked.

Learn refuses a reserved number, **stays armed**, and reports which one. Refusing
in silence looks exactly like the controller not being heard, which is the
failure this path exists to make visible.

## The card

Clicking a row raises a card over the list — the same shape as the knob card.
Jog sets the CC, click arms learn, Back closes and disarms.

It is an **overlay, not a modal**. A confirm raised from the knob grid renders
nowhere and cannot be answered; that is the documented reason Save / Save As /
Delete hand off to the list view (`docs/SHADOW_UI.md`). And it is drawn last and
therefore **fed input first** — the grid claims the jog in its own early-out, so
a card fed after it draws perfectly and answers nothing.

Assigning **swaps** with whoever held that number, so nothing silently loses its
address. The exception is a parameter that had none: there is nothing to swap
back, so the previous holder loses it — the honest outcome when every number is
spoken for.

Leaving by any route disarms learn, or the next CC lands on a parameter the user
is no longer looking at.

## Clear

A trailing **Clear all** row per module, shown only when something is assigned,
behind the same confirm as a single clear. It is a row rather than the Delete
button because the shim forwards only jog, jog-click, back, tracks, knobs and
mute to the UI — Delete and Copy stay with Move firmware and never arrive.
Forwarding CC 119 would work, but Move's own delete still fires, and losing a
clip because you meant to clear a CC is the worse trade.

## Persistence, and the way it went wrong twice

Assignments are keyed by **component + parameter**, never by position: a module
update that inserts a parameter would otherwise move somebody's mapping onto a
different control. They are replayed on top after every rebuild.

Two traps, both of which cost real user data on the device:

- **The replay must be on the `load_file` path.** A set restore calls
  `set_param("load_file", <autosave>)`, not the patch-index load. Hooking only
  the latter meant assignments saved correctly and came back to nothing on every
  reboot.
- **An empty live map must never overwrite a saved one.** A restore that has not
  happened looks exactly like a user with no assignments, and the next autosave
  writes that emptiness over the file. Both maps now refuse to save an empty map
  over a non-empty one — the same protection the empty-slot branch already gave a
  chain.

## Gates

`cc_control` per slot, `cc_control:<component>` per part, and one for Master FX.
All default on, absence in an existing patch means on, and each covers **both**
directions — incoming CC and the learn echo. Toggling a component deliberately
does **not** rebuild the table, so addresses stay where they were learned.

## External CC reaching Master FX

`shadow_master_fx_forward_midi` had two callers — the MIDI_OUT echo path and the
MPE/THRU passthrough — and neither carries ordinary cable-2 input. Master FX
heard Move's own pads and **nothing from any controller**. It is now also called
from the shim's cable-2 scan, which runs every frame and is gated neither on
overtake mode nor on a slot matching the channel. **CC only**: notes already
arrive by the pad route, and forwarding them twice would double-trigger a module
like ducker.

## Request codes

`req_type` is **2 for GET and 1 for SET**. Reading that backwards makes a read
handler fire on writes and no write handler fire at all, which presents as a
write that reports success and changes nothing.
