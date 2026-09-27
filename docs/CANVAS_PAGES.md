# Enterable canvas pages

Part one is implemented (the fullscreen dive). Part two is a proposal.

## The problem

A `type: "canvas"` param is a screen the module draws. The host gives it the jog
wheel and the knobs, and keeps two controls for itself:

```js
if (view === VIEWS.CANVAS && (status & 0xF0) === 0xB0) {
    if (d1 === MoveMainButton && d2 > 0) { closeCanvasPreview(false); return; }  // click
    if (d1 === MoveBack      && d2 > 0) { closeCanvasPreview(true);  return; }  // Back
}
```

Both are stolen ahead of `dispatchCanvasMidi`, so neither ever reaches the
module. That is right for what a canvas is today — a visualiser you glance at
and leave. A scope, a meter, a waveform. Jog to scrub it, two ways out, nothing
to enter.

It is wrong for anything **nested**. A file browser needs "enter this folder". A
settings menu needs "open this submenu". The only gesture that means enter is
the one the host spends on leave, so a canvas cannot express a hierarchy at all
— the single largest thing a module-drawn screen might want to be.

Observed, on a drum module whose sample browser is a canvas: clicking `..`
leaves the browser. Clicking a folder leaves the browser. Every click is the
close gesture. The module is drawing a tree it has no way to let you walk.

## Two cases, not one

A canvas param reaches the screen two ways, and they are **not** symmetric. An
earlier draft of this document treated them as one thing and was wrong.

| | `as_page: true` | no `as_page` (a cell) |
|---|---|---|
| how you get there | paging, like any other page | click the cell, or knob touch+click |
| what is underneath | the level's page, with its knobs live | nothing — the canvas owns the screen |
| what it is today | a `PAGE_KNOBS` page whose body the module paints | a fullscreen `VIEWS.CANVAS` |

Everything below follows from that second row.

## Part one — the fullscreen dive (implemented)

There is no page underneath and nothing else on screen, so there is no other
owner for any control. The module already receives every CC except the click and
Back. `enterable: true` hands those over too:

- **the click** becomes an ordinary CC at the module's `onMidi`. Declining to
  steal is the whole implementation.
- **Back becomes a contract**:

```js
handleBack(ctx) === true   // "I went up a level"    -> stay inside
anything else              // "I am at my top level" -> the host closes
```

So a module implements a way **up**, never a way **out**, and the host does what
it would have done anyway once the module runs out of levels. A hook that throws
is disabled and returns undefined, so a script that dies mid-navigation cannot
hold the button.

No failsafe gesture, deliberately. A module that wrongly claims Back forever
holds it on its own screen only — changing track, swapping the module and
leaving the editor all take the user out without consulting the canvas.

Default false, so every canvas that exists today is untouched.

## Part two — an `as_page` canvas is a DOOR (proposed)

The host already has this concept, and the earlier draft of this document
reinvented it badly. `page_controller.mjs`:

```js
function isDoor(p) {
    if (p && p.kind === PAGE_KNOBS) return knobsAsList(p);
    return !!(p && (p.kind === PAGE_MENU || p.kind === PAGE_PRESET
                    || p.kind === PAGE_ITEMS));
}
```

A door is a page you **enter**: plain click enters it, Back leaves it, the
bracket frame says it can be entered, and Shift+click reaches the section picker
from inside one — which is what keeps a door from being a trap, everywhere, with
one gesture. All six behaviours follow from that single function.

An `as_page` canvas is a `PAGE_KNOBS` page the module paints. It is not a door
today, so a click on it dives the cell under your hand and the module gets
nothing.

**The proposal is one clause**: an `enterable` canvas page is a door. Click,
brackets, Shift+click escape and the announcements all come along unchanged.
While entered the module receives the jog and the click through `onMidi`,
exactly as a dived canvas does.

**Back is the one thing that does not come along unchanged, and it is the whole
point.** A door's Back exits the door. For a canvas door that is the original
bug wearing a different hat: Back in a file browser would leave the browser
instead of going up a folder. So Back inside an entered canvas door takes the
SAME contract part one defines —

```js
handleBack(ctx) === true   // "I went up a level"    -> stay in the door
anything else              // "I am at my top level" -> exit the door
```

— which is also what makes the two halves one feature rather than two. The
module writes one `handleBack` and it means the same thing however the user got
there. A door's own exit is simply what happens when the module runs out of
levels, so nothing about the door rule is special-cased; it is reached one press
later than before.

⚠ Part one implements this contract in `shadow_ui.js`, for `VIEWS.CANVAS`. Part
two needs the same offer in `page_controller`'s door exit, reading the overlay
the page already holds. Two call sites, one contract — and if they ever disagree
about what `false` means, that is the bug to look for first.

The reasoning `isDoor` already gives for a knobs page is the reasoning here:

> On the grid there is nothing to enter: the jog pages, the eight knobs are the
> controls, and a click dives the cell under your hand. Drawn as rows there IS a
> cursor.

A canvas page has a cursor too. It is the module's.

### The knobs stay with the level

Entered or not. `onKnobTurn` has no entered-state gate, so this is what every
door does today, and a canvas door doing something else would be the invisible
mode the door rule exists to prevent.

It does mean a knob nudged while you are navigating edits a param you cannot
currently see. That is already true of every menu, preset browser and items list,
so it is not a new hazard — but if it ever needs addressing it should be
addressed for **doors**, not for canvases alone.

A `claims_knobs` declaration — `true` for all eight, or `[1]` for some — would
let a page take them where that is genuinely wanted. Deferred: nothing needs it
yet, and the sample browser this work started from wants none of it.

### `enterable` and `preset_browser` are mutually exclusive

A `preset_browser: true` canvas merges into the level's preset browser, which is
a `PAGE_PRESET` — **already a door**, with every control spoken for: the wheel
browses, the knobs stay on the level (so the sound is still editable while you
browse, which is the point), and click and Back are its own enter and exit.

There is nothing left for an entered canvas to own, so a param declaring both is
a contradiction rather than a preference. The module-json check should reject it
rather than silently pick a winner.

This is what monksynth's `big_face` is, and what bouba-kiki's `shape` is not —
worth knowing before touching either.

## What this replaces

`canvas_takes_click` — a fork-only flag handing the jog click to a canvas that
asks for it. It solves part one by negotiation rather than by declaration, has no
affordance and no exit contract, and is superseded. Nothing in the fleet used it.

## Canvas users today

All five, because "who does this affect" was guessed twice before it was checked:

| module | shape | knobs today |
|---|---|---|
| bouba-kiki `shape` | `as_page` | the level's |
| monksynth `big_face` | `as_page` + `preset_browser` | the level's |
| DR32 `sample` | dive | all eight (wants none) |
| palette-move `editor` | dive, a `bank_editor` | all eight, actively used |
| widget-test `detail` | dive, upstream's fixture | all eight |

Part one changes none of them: the flag defaults to false. Part two changes none
of them either — neither `as_page` module declares `enterable`, and monksynth
could not.
