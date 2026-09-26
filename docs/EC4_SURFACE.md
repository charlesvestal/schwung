# Faderfox EC4 as an external surface

Global Settings → Surfaces → **Ext Surface = EC4**. The EC4 runs the same
navigation, knob pages and Mixer as the E16 (`docs/E16_REMOTE.md`), laid out
for a device whose screen is text: sixteen 4-character names and a 4×20
overlay. Code: `src/shared/ec4_surface.mjs` (the surface),
`src/shared/ec4_protocol.mjs` (the wire, and the setup Schwung installs).

## What it shares with the E16

`src/shared/surface_core.mjs` is every surface's: the ONE focus (slot, module,
page — each module remembers its page and each slot its module; Follow Focus
parks and restores it), the surface's own page controller bound to that focus
(including `noteParamWrite`), the knob feel (device pulses → Move detents,
choices by angle, and the Mixer through the knob engine), presence (seek /
keepalive / loss with the device's probe injected), SysEx reassembly and the
12-packet wire limit. The E16 and the EC4 files hold only what the device can
show and how it is found. The host (`shadow_ui.js`) keeps one registry,
`externalSurfaces()`, and one Mixer io object, `surfaceMixerIo`.

**One Shift grammar on both devices:** a TAP (released within 250 ms, nothing
done under it) switches Module ↔ Mixer; a HOLD is the device's own modifier
layer (the slot map on the E16, the alternate names here) and, in the Mixer,
pan / solo / 100%.

## What the EC4 needs

**One setup holding Schwung's map.** The EC4 has no remote mode: what an
encoder sends is whatever the loaded setup says. Schwung installs a setup whose
every group sends the E16's remote-mode map — CC 1-16 relative, notes 0-15,
channel 1 — so the E16 input decoder and the shim's claim
(`src/host/e16_claim.h`) serve it with no host change. Encoder names are
`----`, the EC4's marker for a cell the host may write.

**Installing it — Global Settings → Surfaces → EC4 Setup**, with the EC4 on
Move's USB-A. Nothing else on the EC4 changes:

1. On the EC4, select the setup to replace. The screen shows its number;
   **click**.
2. Put the EC4 in receive mode: FUNC + encoder 4 (setup mode), then encoder 14.
   **Click** to send (~7 s). Keep Move's transport stopped while it goes.
3. **Click** when it says done. Move now looks for that setup, and Ext Surface
   is EC4. The setup keeps its old **name** — a single-setup transfer carries
   none — so rename it on the EC4 if you want it labelled; Move finds it by
   number.

The transfer is Faderfox's single-setup download (type 2): ~14 KB, sixty-one
64-byte pages each with a checksum, byte-identical to what the EC4 sends for
its own "Send current setup". Measured: the EC4 writes it into its **current**
setup, whatever the addresses in it say, and leaves every other setup — and
that setup's name — as they were. A damaged page is refused by the EC4
("Receive Error") rather than stored.

The setup is looked for by number; `EC4 Setup` records it in
`/data/UserData/schwung/ec4_setup` (1-16, default 13).

**Press SHIFT + NAME once on the EC4.** Otherwise it blanks a name cell while
its encoder turns — its own value display, which has nothing to show for a
relative encoder.

The EC4 is Schwung's only while it reports that setup. Any other setup is the
user's own: nothing is written to it, and its encoders are ignored. Switching
back rewrites the screen.

## Surface Nav: two layouts on every surface

Global Settings → Surfaces → **Surface Nav** picks how the knobs navigate, per
device (the row edits whichever device Ext Surface names):

- **Knobs** (the EC4's default, described below): eight parameters and eight
  labelled navigation knobs. Nothing is hidden behind a gesture.
- **Map** (the E16's default): all sixteen knobs are parameters, two pages at
  once. Hold Shift for the slot map (drawn here as sixteen names, the current
  slot marked `>`), press a module under it to jump; Shift + turn pages the
  pair, one step per 30° on the EC4. A page change shows the page list on
  the overlay, since the names cannot carry the two page titles.

A Shift **tap** is the Mixer in both. The layouts live in
`src/shared/layout_map.mjs` and `layout_knobs.mjs` and never draw: each
describes a screen (`layout_common.mjs`), which the EC4 shows as names plus
the overlay and the E16 draws in pixels. Each device's choice is saved in
`shadow_config.json` (`external_surface_nav`).

## Layout (Knobs)

```
 MODULE                                      MIXER
 | CUTO | RESO | DRIV | ENVA |  page 1-4     | VOL  VOL  VOL  VOL  |  push: mute
 | ATTA | DECA | SUST | REL  |  page 5-8     | SndA SndA SndA SndA |  push: off / back
 | <PG  | MAIN |  2/5 | PG>  |  pages        | SndB SndB SndB SndB |
 | SL 1 | OBXD | VOL  | PAN  |  focus        | RtnA RtnB Capt Filt |
```

- **One page at a time** on the top eight knobs, the same page Move shows on
  its own eight. Push = the grid's click.
- **Row 3**: `<PG` / `PG>` pushes step a page; turning any of the four
  scrolls pages and shows the list on the overlay.
- **Row 4**: turn `SL` for the slot, the module cell for the slot's module
  (MIDI FX, synth, audio FX, in chain order); `VOL` is the focused slot's
  level (push: mute) and `PAN` its pan (push: centre).
- **Shift tap** (released within 250 ms, nothing touched) switches Module ↔
  Mixer. **Shift held** is the alternate layer — on the Mixer, pan on the
  level knobs, solo on their pushes, 100% on the send and return pushes — and
  the names show it once it is a hold. The Shift key and Shift + push arrive
  as SysEx reports; Shift + turn is the plain CC.

## The overlay

A reading while a control moves, gone 1.5 s after it stops:

```
  [Filter] >> Cutoff
 
       [1.2 kHz]
 ██████████
```

Row 1 is `[page] >> parameter (ABBR)`, centred; on a long line the
abbreviation goes first, then the page shortens to four characters, and only
then is the parameter name cut. Row 4 is a value bar of full blocks (20 cells,
from the centre for a bipolar value) — or, for an enum, a toggle or a narrow
int, the options in a row with the current one centred in brackets. The page
knob shows the page list the same way; the slot and module knobs show nothing,
since their cells already say where you are.

## How the knobs feel

A Move knob sends ~210 detents a rotation (counted); the EC4 ~72 pulses
(Faderfox's firmware notes; the generated setup has acceleration off). Pulses
are scaled to Move detents once, and everything continuous — page knobs, the
Mixer, VOL and PAN — goes through the same knob engine as a module's page, so
a rotation covers the same ground on either. The Mixer model's own fixed steps
are driven from that engine rather than directly.

Choices are not scaled that way: at Move's ratio an enum would step every ~1.4
pulses. Enums, toggles, narrow ints and the page / module selectors step once
per 30° of the EC4's own rotation, the slot selector once per 60°.

The scale is a file, for matching by hand (EC4 pulses per Move detent):

```bash
ssh ableton@move.local "echo 0.4 > /data/UserData/schwung/ec4_knob_scale"
```

## On the wire

Every message is at most 12 USB-MIDI packets. Move splices its own MIDI into
a SysEx that spans SPI frames (`docs/E16_REMOTE.md`, "The garbling"), and on
the EC4 a 206-byte write lost three cells in its middle while 26-byte writes
arrived whole. So text goes out in runs of at most seven characters, diffed
against what the device was last told, and a slow round-robin restates the
screen so a damaged message heals. Every message is acknowledged by the EC4;
a device silent for 5 s is treated as gone.
