# Move's copy/paste gestures

Move's own copy and paste for **steps**, **pages** and **clips** — what the
gestures are, what they do at the edges, and what is still unknown.

It exists because Schwung wants to carry a step's or a page's **automation
locks** along with the notes Move copies. That mirror can only be as good as
this model: a gesture we read differently from Move desyncs the locks from the
notes, silently, and the user sees automation on a step that has no note or a
note whose lock stayed behind.

**Everything below was measured by driving the gesture and diffing the clip's
notes in `Song.abl`** (startTime, noteNumber), on 2026-09-14. Nothing here is
inferred from Move's documentation or from how a similar instrument behaves.
Where something was NOT measured it says so, in "Not known" — that section is
the point of the document, not an apology.

> **SUPERSEDED IN PART — read this first.** A later sweep (firmware **2.1.0**,
> `docs/MOVE_UI_MAP.md`) found this model INCOMPLETE rather than wrong. Every
> sequence below was measured with Copy HELD, and it is correct for those. But:
>
> - **The armed source SURVIVES releasing Copy.** Copy down, press the source,
>   release Copy, then press a destination — it still pastes. So "Copy is held"
>   is not a precondition, only the way these measurements happened to be made.
> - **The armed source then PERSISTS INDEFINITELY** — across screens and across
>   the known-state reset. It was observed still armed minutes and dozens of
>   presses later, when an unrelated held step printed `Notes pasted` and wrote
>   a note nobody asked for. **`Copy + an empty step` clears it.** Treat an
>   armed source as a landmine: the next step press anywhere pastes.
> - **A second Copy press does NOT cancel**, despite Ableton's manual saying so.
> - **Range copy (hold Copy, press-and-hold start, press end) WRITES NOTHING** —
>   it is a SELECTION. The pairs model below would read it as a paste, which is
>   the one place this document could have caused a wrong edit.
>
> The manual describes firmware ~1.5.x; the device is 2.1.0. Where they differ,
> the device is the authority.

## The model

All three are the same shape, which Charles described and the measurements
confirm: **Copy is held, and step presses PAIR UP.**

```
Copy down
  press A      SOURCE          (latched, nothing happens yet)
  press B      PASTE A -> B
  press C      a NEW source
  press D      PASTE C -> D
Copy up
```

- **Step copy** — Copy held, in Note view. A and B are step buttons.
- **Page copy** — the same with **Loop (CC 58) also held**. A and B are pages.
- **Clip copy** — Copy, then touch a clip area on the same track.

### It is PAIRS, not "one source, many pastes"

`Copy ↓, s1, s2, s3, Copy ↑` put a note on **s2 only**. A model where every
press after the first pastes from the same source predicts s3 as well, and it
did not happen: s3 became a new source and Copy was released before its
destination, so it did nothing.

### Re-tapping the source does NOT cancel

`Copy ↓, s1, s1, s5, s6, Copy ↑` put a note on **s6**. So the self-tap pasted
s1 onto itself — a harmless no-op — and **consumed the pair**, leaving s5 to
start a fresh one. There is no cancel in this gesture: to abandon a source,
release Copy.

### An EMPTY source is a no-op, NOT a clear

`Copy ↓, s3 (empty), s5 (has a note), Copy ↑` left s5's note alone. Copying
nothing does not erase the destination.

**This one matters most for the mirror**: if an empty source cleared the
destination, the lock mirror would have to clear locks too, and getting that
backwards would delete automation the user never asked to lose.

## Facts the gestures depend on

- **Step buttons address the DISPLAYED page**, and the step strip's `bold<N>`
  is not a reliable read of which page that is — with `4pg bold1` the steps
  still addressed page 1. Determine it by adding a note and seeing where it
  lands, not by trusting the bold segment.
- **Steps are 0.25 quarters apart**, so step index N lands at `N * 0.25`
  within the displayed page.
- **Move writes `Song.abl` ~8–12 s after an edit** (measured; see
  `STEP_PLOCK_CLIP_PENDING` in `step_plock.h`). Every before/after diff has to
  wait that out, and a gesture's effect is invisible until it does.
- **Double Loop (Shift+Step 15) REPEATS the existing pages**: a 1–2 clip
  becomes 1–2–1–2. It does not make every page identical — that only looks
  true when you start from a single page, which is what misled the first
  version of this note.

## What this means for mirroring locks

The whole signature is in the MIDI stream — **CC 60 (Copy), CC 58 (Loop), and
step notes 16–31** — all of which the shim already sees in every view. So the
mirror can be **passive**: Move performs the note copy, we copy the locks.

It must NOT claim the buttons. Claiming Copy means Move never performs the
copy, and the user gets locks with no notes — worse than the feature's absence.

Note the tension to resolve first: the shim currently DOES claim Copy (and
Delete, and Undo) while a step is held and the Schwung grid is up, so on our
own grid Copy + step reaches a "Copying locks is not supported yet" notice
instead of Move. That claim exists because an unclaimed Copy once duplicated a
clip mid-gesture and every later p-lock addressed the duplicate. Mirroring on
our grid means lifting that claim, which reverses a deliberate decision taken
after a real incident.

The verbs needed already exist in some form: a step is a phase within
`LANE_MIN_POINT_BEATS` (the window `lanes:clear_point` and the p-lock share),
and shifting a span of points by a fixed offset is what `lane_double` does.

## Not known

Some of the original gaps have since been closed by the 2.1.0 sweep in
`docs/MOVE_UI_MAP.md` — the source surviving Copy's release, the absent cancel,
and range copy being a selection are all up in the banner. What follows is what
remains.

Untested, and a mirror should not assume any of it:

- **Paste onto an OCCUPIED step** — replace, merge, or add a second note? Every
  measured paste landed on an empty destination.
- **Copy released before a destination** — is the source forgotten, or still
  armed for the next Copy press?
- **Does page copy pair up the same way?** Only one page pair was driven.
- **Copy + a PAD** (a drum voice in Note view) — a distinct gesture, or nothing?
- **Cross-track and cross-clip** copying, beyond the clip copy already mirrored.
- **Undo** — does Move's Undo reverse a step/page paste, and would our mirror
  need to follow it?
- **Velocity, length and micro-timing** — the diff only compared startTime and
  noteNumber, so a paste may carry more than this document knows.

## Reproducing

`schwung-testd` plus `seq.py` (one connection, timed packets) drive the
gestures; `notes.py` dumps the clip's notes. Injected packets reach **Move**
without `inject_as_hardware`, which is all these gestures need — the flag is
only required when Schwung's own decoders must also see the press.

```
Copy down 0BB03C7F   Copy up 0BB03C00      (CC 60)
Loop down 0BB03A7F   Loop up 0BB03A00      (CC 58)
step N    0990<nn>77 / 0980<nn>00          (nn = 0x10 + N-1)
```

Wait ~15 s after each gesture before diffing, for the save.
