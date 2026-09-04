# Param Pages — the knob grid, its widgets and its gestures

Split out of `CLAUDE.md`, which keeps a summary and points here.

Covers `src/shared/param_pages/` (the page planner, widgets, knob engine,
animation store), the knob-grid draw paths in `src/shadow/shadow_ui.js`, and
`shadow_ui_param_pages.mjs`. Read this before changing any of them.

### Chain editor knob feedback is a CARD

Touching a knob in the chain editor raises a bordered card
(`src/shared/param_pages/knob_card.mjs`) showing the four cells of that knob's
row, drawn with the knob grid's own widgets via `drawKnobRow` at a 29px cell
instead of the grid's 32. Touch raises it, release drops it; a turn with no
touch raises it too and decays after ~700ms, so a cap sensor that misses cannot
strand the feature. With no component selected the slot's global mappings serve
a name and a value but no type metadata, so that case gets a header-only card.

The card consumes no TURN. A jog-click while it is up is swallowed — it fires
the parameter if that parameter is a trigger, and otherwise does nothing.

It used to be dismiss-and-descend: the click fell through and opened the
focused component. That was deliberate and it was wrong. The card is a panel
over the diagram and the component behind it is only incidentally selected, so
descending acted on something the user could not see — *"when the overlay is
active clicking shouldn't take you into the module, it's a hidden element that
it's still selected"*. Releasing the knob drops the card, so there is already a
way out that does not also do something.

**The 1px black gap between the border and the header band is load-bearing.**
Both are white, so where they touch the border stops existing and the card reads
as a stripe across the diagram. **The divable brackets are load-bearing too** —
nothing dives from the card, so dropping `drawDivableMark` looks like an obvious
simplification, but `drawOpaqueBox` has no frame of its own and the brackets ARE
its frame. Both are asserted on the pixel buffer in
`tests/host/test_knob_card.sh`, with the outermost cell touched, because neither
is visible in code review.

**Every value is read on touch-down, never on the draw path** (`knobCardOpen` in
`shadow_ui.js`) — a read is ~2.8ms against a 1.68ms whole-page render. Two tests
pin it: `test_chain_knob_card_reads.sh` for the renderer, and
`test_chain_edit_read_budget.sh` for `drawChainEdit` itself. The latter LIFTS
`drawChainEdit` with `new Function` and a fixed dependency list, so the card
reaches it through a single `knobCardDrawState()` accessor — nine free
identifiers there is nine chances for a `typeof` guard to make the block
unreachable and leave the budget measured with the card switched off, which is
what happened the first time. Consequence of the read budget: a modulated
NEIGHBOUR does not animate while a knob is held; only the touched knob carries a
modulation mark, because that read is one `showKnobOverlay` already pays for.

`render_page_movy.mjs`'s cell geometry is a parameter (`GRID_GEOM`,
`drawKnobRow`'s optional `geom`), so the card and the grid share one row
renderer. `geom` is **all-or-nothing** — a partial `{cellW}` makes every cell
origin `NaN`, which reaches `line()`'s `for(;;)` and never satisfies its
equality break: a frozen `shadow_ui` tick. The default path is pinned
byte-identical against `tests/fixtures/movy-geom-baseline.txt`
(`UPDATE_GEOM_BASELINE=1` to refresh).

Preview it without deploying: `node tools/param-pages/preview_knob_card.mjs
<module-id> --knob N [--short] [--png DIR --scale 4]`.

### Every enum opens a LIST — except at TWO options, where the click FLIPS it

A picker over two items is a menu whose entire content is the value already
visible in the cell and the one other value there is, and it charges two
gestures for a state one gesture can describe. So a two-option enum WRITES THE
OTHER VALUE on click and never raises the list. Reported from the device
against Global Settings' Mirror Display and Move->Schwung — *"if an option has
two values, clicking it should change the option ... we dont need a whole menu
for two items"*.

**Deliberately NOT limited to booleans, and that is the interesting part.**
`drawnAsSwitch` splits Off/On (212 fleet cells) from a two-way CHOICE —
`Mix/Reverb`, `Saw/Square`, `Legato/Trig` (134 cells) — and that split is right
for the PEEK, which exists to show a word the cell has no room for. It is wrong
here: what the flip removes is the SECOND GESTURE, and a choice pays that
exactly as a boolean does. Two rules over the same population, disagreeing on
purpose.

`flipsOnClick` (`param_meta.mjs`) is ONE definition serving two questions that
must not disagree — what the click does (`page_controller.onClick`) and what
the footer promises while the knob is held (`CLK FLIP`, not `CLK OPEN`). The
divable/opaque pair beside it is written up as exactly that failure three times
over: a cell became a door and the footer had to be told separately, so it
advertised `CLK MENU` over a click that opened an editor. **The FLIP branch
must precede the divable OPEN branch** — a two-option enum is still divable, so
OPEN claims it otherwise.

**It requires `divable`, which is what keeps TRIGGERS out.** A trigger is a
two-option enum in the wire format (`["—","Rnd!"]`), so a predicate written on
the option count alone turns every momentary in the fleet into a latch —
euclidrum randomises a kit on the way past. Readouts are excluded the same way.

**THE FLIP IS THE GRID'S ANSWER. A LIST FOCUSES INSTEAD.** Both list surfaces
— `knobsAsList` (Param View: List, and whatever the screen reader forces) and
the hierarchy editor — put a two-option enum into EDIT MODE on click and let
the jog step it, exactly like a float row. The flip needs a knob under your
hand to be the saving it claims; a list has none, so a row that changed value
the instant you clicked it would be the one row on the page with no focus
state. Reported from the device: *"just show it focus and let jog change it.
then it's the same gesture for each row. otherwise it's invisible."*

`flipsOnClick` is still what both consult — it is the definition of "this enum
is a two-way", not of "flip". The grid flips that set and the list focuses it,
so the two can differ about what a two-way DOES without ever drifting about
WHICH params are two-way. In `page_controller` it is the term that WIDENS the
knobsAsList edit gate past `!divable`; the hierarchy editor restates the count
instead, because its meta is the RAW `chain_params` declaration (`type`, not
`kind`, and no `divable` at all) and the two exclusions `divable` carries are
its own two early returns immediately above.

`tests/host/test_two_option_enum_flip.sh` pins the grid half and the picker
skip; `test_list_layout_footer.sh` drives the focus for real, clicking a
two-option row and jogging it both ways — a footer assertion alone would pass
with EDIT advertised over a row the jog does nothing to. The flip test's
list-editor probe was anchored on the first `type === "enum"` in
`shadow_ui.js` first, which landed on `isTriggerEnumMeta` 1500 lines earlier
and stayed GREEN with the branch deleted.

### Two values: the BOXED one toggles, the SWITCH is clockwise-on

**The split is the WIDGET, not the semantics**, and that is the whole of the
rule. Both spellings are one control under the click and the dive; they part
company in `knobStep`, and nowhere else. `isTwoWayMeta` still answers true for
both — the switch branch simply sits ahead of it.

A **boxed** two-way — Mix/Reverb, Saw/Square, drawn as the enum SQUARE —
**TOGGLES on a detent whichever way it went.** The two options sit in the same
box in the same place, so the cell shows a STATE and names no direction. It
used to fall to the enum branch and CLAMP behind the four-detent gate, so at Mix
a left turn did nothing forever and a right turn took four detents to do
anything. Reported from the device: *"if there are only two, why not let it wrap
otherwise you have to know which way is off and which way is on, in which case
you need some knowledge you dont have."* There is no way to acquire it from a
box. Same argument that makes a trigger fire in either direction.

A **switch** — Off/On, or an int 0..1 — is **direction-absolute: clockwise on,
anticlockwise off.** A switch has a TRACK, and its knob sits at one end of it:
the form names the direction, and it is the same direction every physical switch
has ever had. So the picture makes a promise here that the boxed value never
makes, and the toggle broke it. Reported from the device: *"if it's on it should
stay on when turning it on."* The write is IDEMPOTENT, so there is no latch: a
dozen detents of one flick all say the same thing, and a flick that lands where
it already was is the intended no-op rather than a parity accident. An author
who declares the options backwards (`["On","Off"]`) still gets clockwise-on —
`switchOnValue` resolves the direction against the WORDS, not the position, the
same way the graphic does.

**So the TURN partition must equal the DRAW partition.** `detectSwitch` emits
`VIZ_SWITCH` for exactly `isBooleanMeta && !isTrigger`, and `knobStep`'s switch
branch guards on exactly that pair. `test_two_way_knob_toggle.sh` §6 asserts
them equal over the whole fleet fixture, in BOTH directions, because a drift
either way means a control makes a promise with its shape that the knob does
not keep — a track that points somewhere the knob will not go, or a box that
turns as though it had one.

**WRAPPING ALONE WOULD NOT DO for the boxed one, and that is the part worth
keeping.** With two values, "wrap" and "toggle on every detent" are the same
thing, and one flick of an encoder is a dozen detents — so a flick would land on
whichever value the detent count happened to be even or odd about.
`knob_engine.mjs` therefore pairs the toggle with a LATCH at
`TWO_WAY_GESTURE_GAP_MS`, the same number and the same rule as
`TRIGGER_KNOB_GESTURE_GAP_MS`: **one flick is one gesture.** And it is a latch
rather than a rate limit — the stamp is the last **DETENT**, so the clock runs
on STILLNESS. That distinction shipped wrong once already on the trigger and
was reported from hardware; `test_two_way_knob_toggle.sh` pins it as a
sequence, and pins the two constants EQUAL by number, because a user cannot
learn two flick lengths for two controls that look alike.

**It lives in the ENGINE, so every surface inherits it** — knob grid, knob
card, list edit mode, the hierarchy editor and the patches screen all reach
`knobStep`. A TRIGGER is excluded there by `access: "write"`, not by option
count: it is a two-option enum on the wire, and toggling it writes "do nothing"
on every other flick, which for euclidrum is the write that destroys a kit.
Three or more options are untouched — they keep the gate and they CLAMP, because
wrapping a 47-model list makes the end of it unreachable by feel.

Consequence worth knowing: the jog in list edit mode routes through
`knobEditStep` -> `onKnobTurn` -> `knobStep`, so it inherits the latch too. A
deliberate jog detent is 1:1 everywhere else, so flipping a two-way twice in
under ~270 ms from the jog is swallowed. Deliberate, and the cheapest place to
revisit if it ever reads wrong.

### A knob page drawn as a LIST has three states, and said none of them

`footerHints()` had no branch for `knobsAsList` at all and fell through to the
GRID's answer, `JOG PAGE / CLK MENU`, which is wrong outside the list, inside
it and while editing a row. With Param View on List — or the screen reader on,
which forces the layout — that is the only footer there is, and Global Settings
is driven entirely by the jog. Now `JOG PAGE / CLK ENTER`, then
`JOG SEL / CLK <row verb> / BACK OUT`, then `JOG ADJ / CLK DONE / BACK OUT`.

The row verb is the ROW's, mirroring `onClick`'s ladder: `FIRE` a trigger,
`EDIT` anything turnable that is not a longer enum (which now includes a
two-option one), `OPEN` anything else divable. A readout gets **no** click pair
— an absence is the truth and a verb would be a promise.

**It must precede the held-knob branch**, and not for tidiness: in this layout
`onClick` takes its param from the ROW CURSOR and overrides whatever knob is
under your hand, so the held-knob footer describes a cell the click will not
act on. Same promise-versus-behaviour bug that branch's own comments record
twice, reached from the other side. Pinned as an ordering, and the seek loops
in the test are BOUNDED because the row cursor clamps rather than wrapping.

Past two options, the list is unchanged. Any enum that declares `options` is
divable: hold its knob, click, pick from a scrolling list, Back cancels. The knob still steps it one detent at a time —
the list is the other half, for a Recv Ch with seventeen options or a Braids
model with forty-seven. `VIEWS.ENUM_PICKER`, `drawEnumPicker` in
`src/shadow/shadow_ui.js`, hints `JOG SEL` / `CLK SET` / `BACK EXIT`
(`enumPickerFooterHints` in `shadow_ui_param_pages.mjs` — the hint vocabulary is
a canon, so the wording is built there and not at the draw site).

**THE CELL MARKS DO NOT MEAN "DIVABLE."** Measured over the fleet: **967
divable cells on knob pages, 953 of them (99%) wearing NO mark at all** —
because almost every divable cell is an enum. Divability is a FOOTER fact:
hold the knob and it reads `CLK OPEN`. Marking 135 enums would erase what a
mark means.

The two marks split something narrower, cleanly, with zero overlap:

| mark | cells | turnable? | means |
|---|---|---|---|
| corner brackets | 7 | always | the knob works, AND it opens something |
| chevron box | 7 | never | there is no knob here — only a door |

So **the chevron is not a mark, it is the WIDGET**: an opaque cell has no
value-shape to draw, so `drawOpaqueBox`'s notched frame with a chevron in its
broken edge is what that cell looks like. The brackets are an annotation on a
working widget, and in practice mean exactly one thing — a **ranged
`wav_position`**, a number a knob turns that also has a waveform editor behind
it.

That is why they must never be unified. Bracketing the opaque cells puts two
frames on one rect (a doubled border) and still leaves 953 enums unmarked, so
it unifies nothing; putting the chevron on every divable cell puts it on 953
enums. Reported as *"is it confusing we have brackets and carats that both mean
divable"* — the answer is that they never meant the same thing, but the flag
name said they did.

Hence the naming, which is the fix: `meta.opaque_type` is a fact about the
DECLARATION, and `alsoOpens()` in `param_meta.mjs` is the bracket rule,
single-sourced. It used to be open-coded as `divable_mark && kind !==
KIND_OPAQUE` at each draw site — three terms of subtlety repeated per caller,
and one site is the per-cell mark while the other is a whole viz group's, so
they drifted the moment either was touched. `alsoOpens` also requires
`divable`, so a read-only declaration cannot wear a mark promising a door that
`onClick` will refuse to open (21 fleet params either way, so provably a
no-op today — it is there so the mark cannot start lying).

**The picker wears the movy chrome from BOTH entry points** — the knob grid, and
a jog-click on an enum row in the hierarchy list editor — and reuses the one
shared `drawMenuList`. Following the caller's chrome instead would be a
`cameFromGrid` branch inside a shared draw, which is the exact thing
`chain_editor_chrome.mjs` records the module picker doing before ("the module
select here is different than the module select in slots", reported from the
device). Entry-point chrome is that branch coming back.

**The list rect starts at y=9, not `MENU_LIST_Y`.** `MENU_LIST_Y` (10) leaves
44px, which at a 9px line is FOUR options where the old chrome showed FIVE. 9 is
safe only because this header is not inverted — the glyphs stop at row 5, so the
selected row's highlight at row 8 still has air above it. **A menu page cannot
do the same: its bank bar owns row 7.** `tests/host/test_enum_picker_chrome.sh`
pins it as `CAPACITY === OLD_CAPACITY` and `clipped() === 0`, because the device
clips silently and losing the last option to a band drawn over it is a failure
this codebase has already had.

Nothing is written on the way in or while scrolling, so Back is a real cancel
and the draw path costs no IPC. The grid path keeps its controller alive and
commits through `controller.commitEnum` — that is what makes the picker work on
Slot Settings and Master FX Settings, which are synthesised contracts with no
`ui_hierarchy` to enter, and it keeps the slot io's own mappings (Fwd's offset,
MPE's compound write) applied rather than bypassed.

### A READOUT wears a DOTTED FRAME, and it is a STROKE, not a widget

`access: "read"` is telemetry — a reading the module reports, not a control.
The input layer has honoured it for a long time: `isReadoutParam` in
`shadow_ui.js` shows the value when you turn the knob and writes nothing, a
click opens no picker, and `param_meta.mjs` sets `meta.readOnly` from `access`
with `isDivable` / `isTurnable` both excluding it.

**The DRAW layer did not.** `render_page_movy.mjs` never consulted
`meta.readOnly`, so a readout was pixel-identical to the same parameter you can
change — a dial or a number you could reasonably expect to turn. Reported from
the device as a control that "does not seem to do anything", which was an
accurate reading of the picture.

**The rule is "a readout is dotted"; WHERE THE STROKE LIVES is the widget's
business.** A dial and a big number have no frame of their own, so
`drawReadoutFrame` adds one on the cell rect — `cellLeft + 1`, `cellW - 2`,
`BOX_H`, the SAME rect the divable brackets use, so the two marks are one frame
drawn two ways rather than two frames at two insets. The enum square already
has a frame, so `drawEnumSquare` **dots the one it has**. `drawReadoutMark`,
called from `drawKnobRow` for any uncovered read-only cell, is the one place
that decides which — and it is exported, so the widget sheet draws the mark
through the same rule instead of restating it.

**ONE DOTTED RECTANGLE PER CELL, NEVER TWO.** Two would be two ideas, not one,
and on a full-width square they would be a pixel apart.

- **It changes only the STROKE.** The value never moves — an added frame is
  strictly additive, a dotted stroke strictly subtractive (the dotted pixels are
  a SUBSET of the solid ones they replace). The cell keeps its shape and its
  contents; only the line around them says "not editable".
- **Dotted on the CHECKER lattice in ABSOLUTE screen coordinates**, not stepped
  by 2 from the frame's own origin — the same rule as every dithered fill in
  this subsystem. Two neighbouring frames then share one phase (4K EQ has five
  readouts in a row when its peaks are paginated), and the corners fall out for
  free: a rect-relative step lands a dot on three corners and a gap on the
  fourth, depending on the parity of `w` and `h`.
- **The inset is what keeps neighbours apart.** At `cellLeft + 1` two adjacent
  readouts keep a 2px gap; on the cell rect itself they would butt into one
  continuous rule.
- **The square dots its own stroke because an outer frame DID NOT WORK there,
  and the measurement is the argument.** `drawEnumSquare`'s frame occupies the
  same rows as the cell rect, so the wider the value the more of an outer frame
  the box absorbs. Differing pixels against an identical editable twin:

  | value | box | outer frame | dotted stroke |
  |---|---|---|---|
  | `G MAJ` | 28px | **17** | **39** |
  | `SAW` | 23px | 23 | 36 |
  | `ON` | 17px | 27 | 26 |

  17 pixels spread down two 15-row columns is not legible — rendered side by
  side the two cells were indistinguishable — and it is weakest **exactly where
  the feature is for**: two of the three affected fleet modules are enums, and
  `keydetect`'s values are musical keys (`C MAJ`, `A MIN`), always full width.
  Dotting the stroke inverts the gradient, because a wider box has more
  perimeter to dot. `test_readout_frame.sh` pins that direction — a full-width
  readout must differ MORE than a narrow one — so a revert to the outer frame
  fails rather than merely looking worse.
- **An OPAQUE cell is excluded, and for a sharper reason than the brackets'
  exclusion.** `drawOpaqueBox` draws its own notched frame on the IDENTICAL
  rect, so the dots do not double a border — they land invisibly on top of one,
  and the only place they show is the five-row CUT in its right edge where the
  chevron sits. Rendered, a read-only filepath was an ordinary opaque box with
  two stray pixels in its door: worse than no mark, since it degrades the one
  widget that says which direction its door goes — and dotting *that* frame
  would blunt the same thing. No fleet module declares one.
- **A readout gains no affordance.** `alsoOpens` requires `divable`, which
  excludes `readOnly`, so a readout can never also wear the corner brackets,
  and the footer never promises `CLK OPEN` for one.
- **A readout inside a viz graphic is NOT framed.** Uncovered cells only, for
  the same reason the door mark is: a cell inside a picture is not standing on
  its own. No fleet module puts a read-only param in a group; if one appears,
  mark the SPAN once in the viz loop, never the members.

Rejected, so nobody re-litigates them: an **inverted slab** (inversion already
means *a finger is on this knob* in the label band and *this is the selection*
in a list — a third meaning makes all three ambiguous); **corner brackets only**
(already spoken for, and they make the opposite claim); and a **real meter** for
4K EQ's four peak floats, deliberately deferred — a stereo peak meter is its own
design job, and one dotted treatment covers every readout in the fleet rather
than one module's four.

Pinned by `tests/host/test_readout_frame.sh`, which drives `drawKnobRow` (not
`drawKnobWidget` plus its own frame call — that probe would pass with the branch
deleted) and asserts, per widget kind, that the readout differs from its
editable twin; that an added frame is a strict SUPERSET and a dotted stroke a
strict SUBSET, so nothing moved; that no pixel lands past `BOX_H` or on the cell
edge; that a full-width square wears one rectangle and not two; that the mark
STRENGTHENS with the box; and that the dot phase follows the screen rather than
the rect. Three mutations are caught with three different messages — removing
the `drawKnobRow` branch, reverting the square's stroke to solid, and dropping
the enum exclusion so both rectangles draw.
`tests/fixtures/movy-geom-baseline.txt` moved exactly two pages — `keydetect:0`
and `gesture-test:0`, the only fixture pages that plan a read-only param.

### A filepath param opens a browser, and the knob scrolls THAT too

Diving a `filepath` param from the grid — mrsample's Sample cell is the case —
lands in `VIEWS.FILEPATH_BROWSER`, and the gesture that got you there leaves
your hand on the knob. It now scrolls the file list through the same
`listKnobStep` accumulator the enum picker uses, and a knob TOUCH raises
nothing over it, for the same reason: the card covers the rows being scrolled.

**This was not a missing affordance, it was a write.** With no route, the turn
fell through `adjustKnobAndShow` (which returns false — `buildKnobContextForKnob`
matches no view here) to `handleKnobTurn`, which writes `knob_N_adjust` into the
**selected slot's global knob mapping**. Behind a full-screen browser, so the
only visible symptom was the legacy "Knob 1" overlay drawn over the file list —
which reads as a cosmetic glitch and is not one. Identical in kind to the bug
the picker's branch fixed, and the filepath browser was the last list dived into
from the grid that still leaked.

`filepathBrowserJog(delta)` is shared by the jog case and the knob branch on
purpose: the live-preview arm and the `announceMenuItem` live in it, and two
copies is how a knob ends up scrolling without auditioning or speaking.
`tests/host/test_filepath_browser_knob.sh` pins the routing order from source
*and* lifts the helper to drive it — scroll, audition, announce, clamp, and
clearing the pending audition when the highlight lands on a directory.

Still unrouted, and each is its own decision: `TOOL_FILE_BROWSER`,
`KNOB_PARAM_PICKER`, `LFO_TARGET_*` and `DYNAMIC_PARAM_PICKER` are all lists
whose knob turns still reach `handleKnobTurn`.

### `level_walk.mjs` is the walk, and it has two consumers now

The tree traversal, the prefix rules and the level-naming rules moved out of
`page_plan.mjs` into `param_pages/level_walk.mjs` when the LFO target picker
started grouping by the same levels (`docs/SHADOW_UI.md`). `planPages` behaves
identically — `makeLevelWalker` is the old `visit`, verbatim, and seven tests
catch a mutation of its prefix rule.

Keep it that way: **a second copy of these rules would drift in silence**,
because no screen shows a grid page title beside the picker's row for the same
level. The one sanctioned divergence is the root's name — the walker calls its
root "Main", and the picker overrides that with the mode's own name when
`modes` gives it more than one root.

### The knob grid is the DEFAULT param view, and it reflows to stay drawable

`paramViewGlobal` defaults to 1 (the grid). The hierarchy list is still there
under Global Settings → Display → Param View, and it remains the better view for
the 11 modules that publish no `ui_hierarchy` at all — a knob grid over a flat
paginated param list is worse than a list of them.

`param_view.json` is written **only by the toggle**. That is what lets the
default change at all: a device that never touched the setting has no file and
follows the new default, and one where the user explicitly chose List keeps
List. Save it anywhere else — init, a load, an autosave — and every existing
install is pinned to whatever it booted with, forever.
`tests/host/test_param_view_default.sh` asserts the call COUNT, because a
second call site *is* the whole failure.

**A graphic must sit inside ONE ROW.** Row 0's knobs draw at y=10 with their
LABELS at y=25..32 and row 1 starts at y=33, so a shape spanning both would
draw straight through the label band. That is geometry, not a tunable.

The consequence was not acceptable: 26 fleet groups were rejected for LAYOUT
alone — the ADSR on the Main page of obxd, hush1, minijv, moog, surge, rex and
osirus, plus twelve surge LFO pages. An author writing attack/decay/sustain/
release in the obvious order lands on slots 3..6 and gets four separate dials.
`planPages` now moves such a block into a row (`alignGroupsToRows`), 24 pages
across the fleet.

Three rules keep that from being vandalism:

- **it is a permutation WITHIN a page.** No knob is pushed to another page and
  no orphan page holding one control is created. Max group span is 4 and a row
  is 4 wide, so a group always fits.
- **row two is preferred, but only for a block that must move.** "Always put
  the envelope on row two" is wrong: 29 envelopes already sit inside row one
  and draw correctly, many on pages that exist FOR that envelope
  (obxd/Filter Env, hera/Envelope, tablor/Env) where row two would leave the
  top half empty. An always-rule makes 29 pages worse to fix 24. For a block
  that IS straddling, moving it DOWN leaves the head of the page alone —
  minijv keeps `macro_cutoff` on knob 1, where a nearest-fit rule pushed it
  to knob 5.
- **the real detector confirms the result**, and a move that loses a group
  that already drew is rejected.

An earlier version scored by keys covered with no cost bound and did what that
invites: schwung-filter moved cutoff from knob 1 to knob 6 — five knobs
displaced on a FILTER module — to pull one `mode` key into a group that already
drew. It was also 37ms on minijv, twelve times the rest of the plan. Driving
the search from the counterfactual "what would group if the row rule were
lifted?" is both correct and 6.5ms.

**A detector role is OPTIONAL or REQUIRED, and the difference is a whole
group.** `detectFilter` built its slot run from cutoff, resonance AND whichever
of mode/slope it found, then required the lot to be contiguous — so a Mode knob
parked at the far end of the page deleted the corroborated pair. Optionals are
now dropped when they do not fit; `detectEnvelope` takes the longest adjacent
RUN rather than demanding every role found be adjacent.

**`present` is filtered by ROLE and must never be assumed to contain any
particular one.** `drawPartialEnv` computed its attack rise unconditionally, so
surge's twelve hold/sustain/release LFO pages — no attack at all — produced NaN
coordinates, and NaN reaches `line()`'s `for(;;)` whose equality break is never
satisfied. A HANG, not a wrong picture, and unreachable until alignment made
those pages drawable.

### A turn PEEKS the list; a cell that is already big does not

Turning a divable enum raises its option list over the grid for ~700ms
(`ENUM_PEEK_MS`), header `TURNING`, footer `TURN SET`. It is the same screen
the picker draws (`enum_list.mjs`) with the opposite commit semantics: the
detent has ALREADY written, so there is nothing to confirm and nothing to
cancel. It never calls `setView` — a Back that "cancelled" it would be a lie.

Three things take it down: the timeout, turning a NEIGHBOUR (left up it would
describe a knob your hand has left), and Back. **Back closes the peek and stops
there** — it used to fall through to the view exit and throw you out of the
module, which is a wildly disproportionate answer to a panel about to vanish on
its own. It is a layer like the picker and the entered menu, and Back takes one
at a time. `dismissPeek` goes through `enumPeek()` so an EXPIRED peek is not a
layer: swallowing one press is a layer, swallowing two is a trap, and this
screen has no other way out.

**`render()` is not the whole draw — the peek is `renderOverlays()`.** Nothing
in `src/shared/param_pages/` clears the screen; grep it. That is the contract
`render(ctx, {rect, bands})` depends on — a consumer hosting a page inside its
own chrome must get the body alone — and it is why a FULL-SCREEN overlay cannot
live inside `render()`. So the peek is a second call the frame owner makes:

```javascript
clear_screen();
controller.render(ctx, { title });
controller.renderOverlays(ctx, { clearScreen: clear_screen });
```

Pass no `clearScreen` and it declines to draw rather than interleaving its list
with the grid underneath, which is what an embedded consumer wants.

**A module binding the controller from its own `ui_chain.js` owes that second
call**, and until 2026-09 nothing said so: the draw lived in
`shadow_ui_param_pages.mjs`, so for every other consumer the controller tracked
a peek on each enum detent that was painted nowhere, while `applyInput`
dutifully routed Back to `dismissPeek()` to take down a panel nobody could see.
Silent, no error, not visible from the API. CW-78 and 6W6 both shipped that way
— correct integrations in every other respect, which is the point: the
obligation had to become a FUNCTION rather than a paragraph.
`tests/host/test_enum_peek.sh` now draws through a real framebuffer, because
the rows go through `menu_layout` (global `print`) while the header is a pixel
font that never calls `print` at all — a recording `print()` reports a
headerless screen as complete.

**A parameter drawn across MORE THAN ONE CELL does not peek** (`drawnWide`).
The peek exists because a 30px cell cannot show a list; once the picture has
the room, a panel over the top hides the rest of the row to show nothing new.
Not hypothetical — 12 enum cells in the fleet sit inside a wide graphic, every
one a filter type or an LFO shape, where turning the knob already redraws the
curve better than a list of words can.

**Nor does a SWITCH** (`drawnAsSwitch`) — a separate predicate on purpose:
`drawnWide` is about a graphic having enough ROOM, this is about the graphic
already BEING the list. A switch draws both of its states (the track is one and
its inversion is the other, which is why `drawSwitch` exists instead of a
two-item enum square), so a full-screen Off/On says what the cell already says,
on the control most likely to be flipped repeatedly.

**Suppressed on the WIDGET, never on the option count.** "Two options" is the
obvious test and is wrong for **134 cells** in the fleet — every two-way CHOICE
that is not a boolean: `Mix/Reverb`, `Saw/Square`, `Legato/Trig`, `Time/Rate`,
`Bipolar/Unipolar`. Those draw as enum squares showing ONE word, so the other
word is exactly what a peek is for. 212 are switches and stop; 134 keep peeking.
`tests/host/test_enum_peek.sh` pins both sides.

Known and not fixed: 933 of 958 enum cells peek, and the peek is instant while
the enum square's resize and the waveform morph take ~100ms — so those two
animations are covered by the list at the moment they play. A short delay before
raising the peek would let a single detent show the morph while a sustained
scroll still gets the names.

### The sample cell draws the file it HAS, or nothing

The envelope is the file's real peaks (`wav_peaks.mjs`, streamed and bounded,
advanced from the tick — never from the draw path). When there are none there
is no envelope, just the baseline, the cursor and the brackets.

There used to be a fallback shape, `sin(t*PI)*(0.55+0.35*sin(t*23))`, drawn
whenever the peaks were missing. It is the tri-state read rule in a different
costume — **a read that did not answer must never become a picture** — and it
cost the flagship granular module a waveform for a sample that was never
loaded. granny declares `sample_path` in its hierarchy and on NO knobs list, so
every page carrying `position` searched the page, found no file and drew the
synthetic one.

So `detectSample` resolves the file from the whole contract, not from the page,
and returns it as `extraKeys`. Those are **not** `keys`: keys claim cells, and
an off-page key has no cell to claim. The controller reads them as one extra
stop in the value rotation, the same bargain the preset-name read takes.

**`gatherGroupMembers` seats scattered members together** so the picture gets
the width its controls warrant. `alignGroupsToRows` rescues a group that is
already contiguous but straddles the row break; this is the other half, for
members that are simply not next to each other. It carries the same guarantees,
because it is the same kind of reorder behind an author's back: WHICH keys are
on the page never changes, the result stays inside ONE ROW, and the real
detector verifies the outcome. Measured over the fleet fixture **3 of 489 pages
move** — granny/root 1→2, granny/main 1→2, mrsample/sample 1→3 — and that
narrowness is the feature. A pass that re-seated every page would be a layout
engine, which is a much larger decision. `tests/host/test_viz_gather.sh` pins
the count.

Spray is claimable for that reason. The old rule — it modifies the cursor
rather than being a position, so it never takes a cell — described the
parameter correctly and the layout wrongly: the fences drew on `position`'s
cell while spray sat elsewhere with an arc that looked unrelated. Adjacency
keeps it safe; where the two are apart the run rule still gives span 1.

(A module may declare the same marker on two levels — granny declares
`position` on both `root` and `main` — and the graphic then appears on both.
That is the contract, not the detector.)

### The sample graphic is ONE door, and the FILE is not part of it

Four reports in a row, each falsifying the fix for the one before. The end
state is small; the path to it is the part worth keeping.

1. *"empty sample selection is indistinguishable from the spray control"* —
   `sample_path` was **swallowed** by granny's waveform. `divable_mark` excludes
   `KIND_OPAQUE` because `drawOpaqueBox` draws its own notched frame and
   chevron, but a viz group suppresses that widget entirely, so the cell had no
   frame, no chevron, no brackets and no filename.
2. *"shouldn't the whole thing be divable?"* — `spray` opened nothing, so one
   picture had a door on the left third and nothing in the middle.
3. *"sample file isn't part of the continuum because it goes to a different
   editor"* and *"why is there a line that spans between them?"* — the real
   one, and it retires most of 1 and 2.

**The file no longer claims a cell** (`detectSample`). It is still
`roles.value` — the waveform is drawn FROM it, never ON it — and the value is
still read, because the page cursor walks `page.keys`, not `group.keys`.
Released, the cell draws as the ordinary opaque box: notched frame, chevron,
and **the filename**, which is information the graphic was throwing away. That
is the honest answer to report 1: the fix was never to bracket the cell, it was
to stop swallowing it.

**`spray` dives to the graphic's anchor** (`vizDiveTarget`), so a click
anywhere in the picture opens the waveform editor. Derived, never named — the
rule is "a member of the picture with nothing behind it", so the `self.divable`
bail is what keeps the filepath's own door. Scoped to `VIZ_SAMPLE`: an envelope
has no editor behind it, so a redirect there would invent a destination. ONE
definition, three consumers — the click, the footer hint and the brackets.

**One bracket per graphic**, drawn across the span in the viz loop; covered
cells take no per-cell mark. Rendered per-cell first and it read as three boxes
butted together (four on mrsample). Keyed on **mark-worthiness, not
`divable`** — every enum declaring options is divable, and keying off that
framed mrsample's Loop *switch*.

**`gatherGroupMembers` had to learn the same thing, and its failure was
silent.** `scattered` was built from every role, which after the change
overstated `wantSpan` by one — and since the widened result is verified against
that number by the real detector, the check could never pass, so the gather was
abandoned *entirely* and granny's "Main - 2" collapsed to a one-cell waveform
with the spray arc back three knobs away. Nothing about the failure said "the
file"; the group simply stopped widening. Caught only by the fleet snapshot.

Fleet effect is exactly two lines of `param_pages_viz.txt` (granny, mrsample)
and three pages of the pixel baseline.

**`displayValue` now separates "no file" from "no answer".** Both were `"--"`,
which is the tri-state collapsed in the most visible place: an empty slot
looked identical to a slot whose name had not arrived. `""` → `NONE`, `null` →
`--`. It reads **NONE and not EMPTY**, which is the word that was asked for and
does not fit — 23px in the box's 4x5 face against a 21px budget, rendering as
`EMPT` with the chevron jammed against it. The budget exists so the value
clears the chevron; widening it for one word narrows that clearance on every
opaque cell in the fleet. `NONE` is 19px and is already this tree's word for an
empty selection (`none_label || "(none)"`, the preset row's `(none)`).

**A FILE-ONLY graphic with no file is not drawn; one with MARKERS still is.**
*"You should see the loaded break, but not an empty waveform"* was reported
against breakbeat, whose `A SMP`/`B SMP` cells are built from a filepath ALONE
— nothing loaded means nothing to draw, so they were a bracketed rectangle
containing nothing. Those are **suppressed** in `renderPageMovy`'s viz loop and
fall back to the opaque box reading `NONE`.

Suppressing *every* empty sample graphic was too broad, and granny is the case
that shows why: its graphic is `position` + `spray`, two real controls whose
picture is the track they act on. Empty, the two-cell widget is still the right
drawing — it is where the cursor and the fences live, and those values are
yours to set before a file is chosen. *"When no sample is loaded it should be
the empty two column widget."* So the test is **markers, not emptiness**.

`""` **only**. `null`/`undefined` is a read that has not landed, and
suppressing that changes the cell's whole WIDGET rather than its contents — a
knob would appear and be replaced by a waveform a frame later, on every page
entry. The file is frequently off-page (granny and mrdrums declare it on no
knob, so it arrives through the extra-key rotation), which makes the unanswered
window the common case, not an edge one. `drawSample` keeps the same early
return for callers that do not suppress.

`tests/host/test_sample_cell_doors.sh` pins the lot, and **three of its probes
were wrong first** — all three looked right, which is the point:

- the "no spanning line" probe measured ink at midY in the file cell. The
  chevron and the filename glyphs both live there. Then it measured the length
  of the run starting in the waveform — a spray fence breaks that around x=35,
  long before the boundary, so it **passed under the mutation**. What works is
  the single gutter column between the graphic and the box.
- "brackets vs frame" used the middle of the top edge. An arc knob's curve
  reaches the top of its cell too, so a bracketed KNOB failed for having a
  widget in it. The column just past the bracket arm is outside the knob (17px
  centred in 32) and on any frame that spans the cell.
- the framebuffer **must honour colour 0 as an erase**: brackets and the opaque
  frame occupy the identical rect, and only `notchCorners` distinguishes them.

### The wave editor draws the spray fences

Once a click on `spray` opens the fullscreen `wav_position` editor, that editor
has to show it — diving from a control onto a screen that does not contain it
is the blank-editor failure in miniature. The word `spray` previously appeared
nowhere in `shadow_ui.js`.

Same two dotted columns `viz_draw.mjs` draws in the cell, same semantics:
wrapping, and clamped to the file edges at `spray >= 0.5` (past that ±0.5
already reaches every frame, and a wrapped fence would crawl back *inward* as
the region grew). Drawn **before** the cursor so the solid cursor wins where
they coincide, and gated on the spray value alone rather than on `preview.ok` —
the cursor draws unconditionally, and two marks describing one playhead must
appear and vanish together.

**Read ONCE, on the way in** (`seedWavEditorSpray`, from
`beginHierarchyParamEdit`), then maintained from the writes — the editor frame
is already the most expensive screen here and a draw-path IPC read is ~2.8ms.
The knob write updates it, or the fences freeze at their entry value and read
as a dead knob. `isSprayMeta` is **imported** from `viz.mjs`: a second
predicate would disagree with the cell the user just clicked out of the first
time either is widened.

Pinned functionally in `test_shadow_param_editor_routing.sh`, which captures
`set_pixel` and separates dotted from solid. **The cursor sits at ratio 0
there** — no real WAV, so no duration to map a position against — which puts
the case squarely on the *wrapping* path, the arithmetic most likely to be
wrong. Driven at two spray values, because one is satisfied by a hard-coded
pair.

### Small ints are BIG NUMBERS, not framed ones

`shouldDrawBigNumber` / `bigNumberText` / `drawBigNumber` in
`render_page_movy.mjs`: an int with a declared range spanning ≤24 (≤48 if
bipolar) draws its value in the device 6x7 font instead of an arc, with a sign
only where the range has a negative side.

It used to draw inside the enum square's box. **The box is the ENUM
affordance** — every enum declaring options is divable, and the square plus its
corner brackets are what say a list is behind the cell. A small int has no list
and can never have one, so the frame advertised a door that does not open.

The span bound is load-bearing: an earlier version bounded at 128 and drew 1392
params big across 60 modules, including `volume [0..100]` and `tune [0..127]`,
which are sweeps where an arc is the honest picture.

### A momentary fires from the KNOB too, and it LATCHES per gesture

A trigger is `access: "write"` on an ordinary enum — there is no `trigger` type
— and it draws as a push button (`drawButton`), because the module reports a
constant idle spelling that is meaningless as a value (euclidrum's is an
em-dash the 5x7 atlas cannot draw at all).

Turning its knob used to do **nothing**: `isTurnable` is false for `writeOnly`,
so `onKnobTurn` swallowed the motion silently and the button did not even
flicker. The stated reason was that turning walks THROUGH the fire value — but
that is a fact about the enum STEPPER, not about the gesture. A momentary has
no value to walk past, so the only thing the refusal achieved was forcing the
hand off the knob and onto the jog. Now a **detent fires it, in EITHER
direction** — a direction-sensitive momentary would make half of every spin
read as a dead knob.

**A LATCH, not a rate limit, and that distinction IS the bug.** The first cut
was "at most once per 250ms", which still fires eight times across a two-second
spin — reported from the device as *"gesture test fires repeatedly on detent"*.
The docs already promised the right behaviour ("a whole flick of the encoder
counts as one press"), so the implementation was what disagreed.

The stamp is therefore the last **detent**, not the last fire: every detent
extends the gesture, and the latch clears once the knob has been still for
`TRIGGER_KNOB_GESTURE_GAP_MS` (270). Written *before* the early return, which is
what makes the clock run on stillness rather than on elapsed time.

It was 400 first and felt sluggish on hardware — *"the cooldown needs to be a
bit shorter, try 2/3 the length"*. The floor is set by the SLOWEST deliberate
turn that should still count as one gesture, so there is room to come down
further. The tests deliberately do **not** pin the value: they assert "clearly
inside" at 100ms and "clearly outside" at a second, so any gap from ~150 to
~900ms passes and a broken latch still fails. Pinning 300/500 meant retuning
the constant broke the suite for no behavioural reason.

**A RELEASE clears it immediately**, on both surfaces. The gap is only a
fallback for a cap sensor that never registered — letting go is the real
gesture boundary, and without it you fire, let go, take hold again and the next
detent is swallowed for up to 400ms, which reads as a broken control rather
than as a safety.

**The footer says `CLK FIRE` / `KNB FIRE`.** It said `CLK PUSH`, deliberately —
"name the GESTURE the picture is asking for", and the picture is a push button.
That held while the click was the only way to fire it, and stopped holding when
a detent started firing it too: you do not push a knob you are turning, so no
single gesture-name covers both keys and the honest word is the consequence.
Two pairs rather than a compound `CLK/KNB` key, which measures 3px narrower and
reads well but is new vocabulary — `FOOTER_CANON.keys` name a PHYSICAL control
and `test_footer_canon.sh` enforces it. `KNB PUSH` does **not** fit: the face is
proportional, PUSH is wider than FIRE, and the third pair was silently dropped.
"If it fits" had to be answered by rendering it.

**It is KNOB-ONLY.** A click is one gesture per press and may repeat as fast as
a finger can manage. One flick of an encoder is a dozen detents, and a trigger
is by definition something that DOES a thing: magneto's `["Play","Save"]` would
write a file per detent. Applied at the knob CALLER, never inside the fire, so
a click can never be gated by a knob's latch.

**Both knob surfaces do this and the constant is duplicated, so it is pinned
against drift.** `page_controller.mjs` (the knob grid) and `shadow_ui.js`
(chain editor knob card, Master FX, hierarchy list editor) drive the same
physical encoder against the same parameter, and which one is on screen is a
Param View setting the user can flip — two copies of the number is two
behaviours, noticed only as "it fires differently in List view", which nobody
would think to report as a constant.
`tests/host/test_knob_surfaces_access.sh` requires the two declarations to be
byte-identical, that the window is checked BEFORE the write, and that neither
click path mentions the constant at all. `tests/host/test_param_access.sh`
drives the real controller and asserts the SEQUENCE — one fire, then eight
swallowed detents, then a fire past the window, then the reverse direction,
then two ungated clicks — because each half passes a shorter test alone, and a
RATE LIMIT passes any test whose detents are spaced wider than its window. The
spin in the test is 2 seconds of detents 30ms apart for exactly that reason.

A **readout** (`access: "read"`) still refuses the turn: there is nothing to
set. Both guards must precede the enum stepper's value read, which is asserted
as a line ORDER.

### Knob ring LEDs, and giving them back

`knob_leds.mjs` paints CC 71-78 — knobs 1-4 white, 5-8 amber, brightness
tracking value, colour 0 reserved for "nothing is bound here". CC 71-78 carries
encoder rotation IN and the ring colour OUT; notes 0-7 are touch sensors, input
only.

**A ramp is one hue's `dark` → `dim` → full.** The palette header in
`constants.mjs` gives every hue those variants, and it is the authority —
picking constants by NAME produced `DarkBrown2 → Mustard → Ochre →
BrightOrange`, i.e. `#250E05 → #876700 → #491804 → #C93C00`, whose third step
is DARKER than its second: a sweep went dim, bright, dark, bright.
`tests/host/test_knob_leds.sh` parses the hex out of that header and requires
luminance to rise at every step, which is the assertion that catches it; the
older tests only checked that a sweep walks the ramp in the order it is
WRITTEN, which was true of the broken one too. Step boundaries are derived from
ramp length, never written beside it.

**Leaving the grid RESTORES the rings, it does not turn them off.** Move writes
an LED only when its value changes, so going dark left Move's own rings dark
indefinitely. `shadow_control_t.restore_knob_leds` (a JS-set edge the shim
consumes and clears) arms `led_queue_restore_move_sysex_leds()` — the same call
overtake exit makes.

**The colour is in the SYSEX, not the CC.** `move_cc_led_state[71..78]` looks
like the right cache and is not: Move drives the rings via
`F0 00 21 1D 01 01 3B <subcmd> <idx> <6 rgb bytes> F7`, and the CC packets are
latch triggers. Restoring the CC cache restored a latch or a zero and every
ring came back blank. (That sysex is also the way to drive true per-LED RGB —
brightness as `hue x value` rather than a walk through palette entries — but
the encoder `<idx>` mapping is recorded nowhere in this tree and
`led_queue_set_capture_enabled` has no caller and no dump path, so the restore
replays the whole surface instead.)

`invalidateLedCache()` is called with it: `input_filter`'s cache suppresses a
write matching what it believes the hardware shows, which is only sound while
it is the only writer — and the shim is about to repaint underneath it.
### A door you were SENT to opens; one you PAGED past stays shut

Preset browsers, items lists and menu pages are **doors**: the jog pages until
you click in. That rule is load-bearing — a preset browser auditions live, so
browsing past one must not audition every preset it goes by.

It does not apply to arrivals you asked for. **Choosing** a page enters it:
`navigate_to` after picking from a list, and naming a section in the jog-click
picker. Reported from the device both times — *"factory does dump me to
presets, but shouldn't presets be already active? I have to click into it"*, and
for airwindows, whose entire picker is Presets / Main / Jump to Category, two of
them doors. One deliberate gesture should not need a second to take effect.

The switch is `goToPage(index, { enterIfDoor: true })`, and **it belongs there,
not at the call sites**: with `remember` on, `restoreSection` can land you on a
different page of the section than the index passed in, so only `goToPage` knows
what you actually arrived at. Entering writes nothing — a browser auditions on
*turn* — so this hands over the jog without loading anything. Landing on a knob
grid is unchanged; there is nothing to enter.

`onJog` does not route through `goToPage`, which is what keeps paging inert.
`tests/host/test_param_pages_controller.sh` pins both halves, and mutating
`enterIfDoor` away in either direction fails it: dropping the picker opt-in
breaks the new case, and making *every* `goToPage` enter breaks the existing
"jog pages off an un-entered preset page".

**A `navigate_to` naming a level that plans BOTH pages means the browser.** obxd
is the case: its `banks` level names `root`, and root carries
`list_param`/`count_param` *and* `knobs`. The lookup used to filter to
`PAGE_KNOBS`, so choosing a bank landed on the sliders. Preferring the browser
rather than inventing a `navigate_to: {level, kind}` form is deliberate — only
three modules declare `navigate_to` at all, and new vocabulary repeats the
`options_as_string` lesson: documented for months, set by nobody.

### An editor inherits the KNOB ROW of the page you came from

A level's declared `knobs` array is not the order the user was just looking at.
The grid re-seats keys for LAYOUT — `gatherGroupMembers` pulls granny's `spray`
next to `position` so the waveform can span both cells — so diving into the
wave editor silently changed which physical knob was which, **one click
apart**. In the grid spray is knob 2; in the editor it was knob 4.

Reported as *"the editor should be using the same knobs as the entered page.
using main is confusing, it's a hidden order no one has reference to"* — and
that is the argument: the declared order is invisible, and the page on screen a
moment ago is the only reference a user has.

`hierEditorKnobsFromPage` captures the page's keys in `openParamEditorFromGrid`
(alongside `level`, and for the same reason — `exitParamPages` tears the
controller down). Taken **verbatim**: no visibility filter, because the grid
already applied one when it planned the page, and **no compaction**, because a
hole means "this knob does nothing" and closing it would shift every knob after
it — the same class of surprise this fixes. The level's own
`hierEditorAllKnobs.filter(...)` path does compact, which is a latent version
of the same bug for any level whose knobs are not all visible.

Gated on the level it was captured from, so navigating elsewhere inside the
editor hands the row back, and cleared in `exitHierarchyEditor` so it cannot
survive into a later list-originated session.

**But the entry performs a level hop of its own, and the first cut mistook that
for navigation.** granny's `root` lists only navigation entries, so `position`
is not in `root.params` and `openParamEditorFromGrid` relocates the editor to
`main` on the way in. The override applied at root and was discarded at main:

```
knobRow: level=root fromPage=root -> [position, spray, size_ms, ...]
knobRow: level=main fromPage=root -> [position, size_ms, density, spray, ...]
```

So the row is **rebound to the level actually landed on**, once the entry has
settled; anything after that point is the user moving, and the gate is right
for that. The knob-context cache keys on the LEVEL, which has not changed at
that moment, so the rebind must invalidate it or the stale row survives.

The original test could not see this: its fixture put the param in the page's
own level, so there was no hop. `position2` exists in that fixture purely to
create one.

**It took a hardware log to find, and that is the point.** Turning what looked
like the spray knob resolved to `synth:size_ms` in `adjustKnobAndShow`'s debug
line. The mapping was not observable from outside `shadow_ui.js` — the same
gap the routing comment blames for three shipped bugs — so `ctx.knobParamKey(i)`
now exposes it, and `test_shadow_param_editor_routing.sh` asserts the MAPPING
rather than the array.

### An editor returns to whoever OPENED it, through EVERY door

Diving into a parameter from the knob grid can land you in three different
places — the filepath browser, the canvas view, or the hierarchy editor with
the row opened (edit mode). Each of those has to hand the screen back to the
grid, and each has more than one way out. Miss one and the user comes back
somewhere they did not ask for, one Back away from where they were.

`closeOwnViewEditorToCaller()` is the single answer: it consults
`paramEditorOpenedFromGrid` and returns true if it handled the return. All the
exits go through it — `closeHierarchyFilepathBrowser`, `closeCanvasPreview`,
and **both** ways out of edit mode.

That last one is the trap. **Edit mode is not a view**, so it has no close
function to fix; it is the hierarchy editor with the row opened, and for a
float carrying a waveform strip that strip IS what a user calls "the wave
editor" (granny's `position`). Back out of it already returned to the grid;
the jog-click TOGGLE in `openHierarchyParamEditor` did not — so the gesture
that OPENS the editor was the one that could not close it back. Fixing the two
real views first changed nothing observable, which read as "not deployed".

`tests/host/test_editor_returns_to_caller.sh` drives all three under both flag
states. For the toggle it deliberately leaves the identifiers past the early
return undeclared, so falling through throws instead of passing quietly.

The LFO/knob-mapping target picker is **not** part of this: it is not opened
through `paramEditorOpenedFromGrid` and has its own `lfoTargetFromGrid` /
`returnToSlotGridFromLfoTarget`. Do not merge the two.

### Every scrolling list draws a SCROLLBAR, and no list draws arrows

One dotted column at `SCREEN_WIDTH - 2`, solid thumb, drawn by the exported
`drawScrollbar` in `menu_layout.mjs` — so every list in the tree has it: main
menu, settings, slots, patches, tools, store, chain views, the enum picker, the
hierarchy editor and the file browser. A list that fits its window draws nothing.

**`drawScrollbar` is exported because a LIST is not the only thing that
scrolls.** It lived inline in `drawMenuList`, and `scrollable_text.mjs` — the
help *detail*, one click in from the help *list* — went on drawing the arrows
this replaced. Same session, same jog, two idioms; reported from hardware as
*"we're using the wrong scrollbars"* on the new Module Help door. It takes the
window as `{topY, bottomY, rowHeight, rowInk, windowRows, total, startIdx}`
rather than a list, so text (10px pitch, 7px ink) and rows (9px pitch, derived
ink) get the identical bar. **Any new scrolling surface calls it; nothing draws
its own.** `drawArrowUp`/`drawArrowDown` remain exported for external modules,
and `tests/host/test_help_viewer_chrome.sh` fails if any shipped `src/shared` or
`src/shadow` draw path calls them again.

It **replaced** the up/down arrows rather than joining them. The arrows reported
"there is more, that way"; the thumb reports that plus HOW MUCH and WHERE, which
is the question a 47-model list actually raises. Keeping both draws one fact
twice — the same argument retired the file browser's own `13/30` header counter.

**It is also cheaper, and the shape of the saving is the point.** The arrows were
5px wide and touched exactly two rows, so the clearance was 10px charged per-row
to those two — a value on the first or last visible row was truncated to make
room for a glyph beside it, reported from the device twice. The bar is ONE column
spanning every row: 2px charged to all rows instead of 10px to two. Those rows
went from 108 to 125.

Three geometry rules, each of which was wrong first:

- **The thumb has a 2px floor.** At 47 items in 5 rows its true height is 1.4px,
  and a 1px thumb is indistinguishable from a tick of the track — position
  without extent, which is half the point.
- **The track covers the ROWS, not the rect.** The rect is 10..54 but the last
  row of glyphs ends at 52, so running to `resolvedBottomY` left the final dot
  two rows below anything it measured. Row ink is derived from the highlight
  (`highlightHeight - 2 * offset`), and measured on the WINDOW rather than the
  visible items — `keepOffLastRow` draws one row fewer at the end of a list, and
  a track that shortened as you reached the bottom would read as the list
  shrinking.
- **The selection highlight stops short of a `BAR_GUTTER`.** Full width it runs
  under the bar, and since the bar is drawn after the rows, white on white. Not
  merely invisible: nine rows of solid ink in the track column reads as a SECOND
  thumb, parked wherever the selection is. Nothing can be XOR-ed — the draw API
  is write-only — so the fix is geometric, and one constant serves both the
  highlight and the value edge.

`tests/host/test_list_scrollbar.sh` asserts the GEOMETRY (position advances,
both ends reached, a shorter list gives a TALLER thumb) rather than ink, and
pins the phantom-thumb case on pixels because the draw calls cannot see it.
`tests/host/test_help_viewer_chrome.sh` re-asserts the three rules through
`drawScrollbar` directly and through `drawScrollableText`, on a framebuffer —
including that nothing is lit in the old arrow column (122..125).

### Widget animation, and the wiring that carries it

`src/shared/param_pages/anim_state.mjs` is the per-key frame store: the page
renderer is stateless, so nothing in it can know what a value was a moment ago.
`observe(state, key, value, now, ms)` returns `{from, to, t, moving}`; a first
sighting is stamped already-past, because an arrival is not a change.

Animated today: the waveform morph (100ms), the enum square's resize, and the
trigger flash. Time is passed IN, never read — no `Date.now()` anywhere in the
renderer, which is what makes `tools/param-pages/movie.mjs` able to film a page
deterministically.

**THE SWITCH DOES NOT ANIMATE. IT TOGGLES.** It had a 160ms inverse fill — the
slug snapped, the track wiped — and it is gone, reported from hardware as
DISTRACTING. That is the argument that outranks the one which chose 160 over 70
and 260: a switch is the control you flip most often and least deliberately, so
motion under your hand every time is attention spent in the wrong place, and no
duration fixes a thing that should not move. Nothing is lost, because the two
states already differ by most of the widget's AREA — a flip is the loudest
change on the page even when it happens between two frames. `drawSwitch` keeps
`anim`/`nowMs` in its signature (`drawVizGroup` hands every widget the same
arguments) and deliberately ignores them, and both halves are pinned by
`tests/host/test_anim_wiring.sh`: the waveform must move part-way through a
change, and the switch must be settled on EVERY frame after a flip.

**THE STORE MUST BE PASSED FROM THE CONTROLLER, AND FOR MONTHS IT WAS NOT.**
Every widget guards on `anim && typeof nowMs === "number"`, so an undefined
store draws the settled frame forever — silently, and identically to a correct
render of a value that is not moving. `createAnimState` was written, exported,
unit-tested and never CALLED; every animation shipped inert. (The switch's fill
was one of them — so it was live for a matter of weeks before being removed.)

The same failure is recorded one field away at the same call site, for the
trigger flash: *the renderer tests hand these in directly, so they prove the
renderer and never the wiring*. A comment did not defend it.
`tests/host/test_anim_wiring.sh` does — it drives the real controller and
requires a frame 60ms into a flip to DIFFER from the settled one. Two ways it
passes vacuously: forgetting `setLayout(LAYOUT_MOVY)` (the default is
`LAYOUT_DIAL`, which has no animated widget at all), and asserting a particular
picture instead of a difference between two frames.

**A value ARRIVING is not a value changing, and 46 of 95 fleet modules
animated their first page in.** The read cursor serves one key per tick, so a
full page of 8 knobs spends ~9 ticks (~200ms) with `values[key]` undefined —
and every animated widget rendered that absence as a CONCRETE PLACEHOLDER:
`drawWaveform` resolved shape 0 and `drawEnumSquare` sized itself around
`"--"`. `observe` recorded the placeholder as the settled first sighting, so
the real value arrived as a TRANSITION — waveforms morphing and enum boxes
growing, out of values nobody had set. (`drawSwitch` was the third and the
loudest, reading NaN and drawing OFF; #323 cut its fill for unrelated reasons
while this was in flight, which is why the count is 46 and not the 51 measured
before it landed. The switch stays in the absence test`s fixture but is no
longer one of its subjects — a widget that cannot animate cannot demonstrate
an arrival.) This is the tri-state read rule ("A param read has THREE answers",
`CLAUDE.md`) one layer below where it is
usually enforced: a read that did not complete must not produce a plan, a
default or a cached verdict, and **a widget frame is all three**.

`observeLanded(state, key, raw, value, now, ms)` takes the RAW value alongside
the token being animated. **The two are separate arguments on purpose**: every
derivation here is TOTAL, so `"s" + shape` and a pixel width both
produce a perfectly ordinary token for an absent input — which is exactly how
the placeholder got in. Only the raw value still carries the absence, so only
it can be asked about it.

**Nothing is recorded while the value is absent**, rather than recorded and
suppressed: leaving the key out of the store makes the first real value a first
sighting, which `observe` already stamps as already-past. Recording it would
leave `from` pointing at a value that was never on screen, and the next genuine
change would animate out of it. `undefined` ONLY — the controller refuses to
cache `null` or `""` as a value, so an unanswered key is `undefined` and nothing
else, and widening to falsy would swallow `0`, a legitimate reading of every
switch, shape and enum in the fleet.

`tests/host/test_anim_absent_values.sh` asserts BOTH halves — an arrival draws
the settled frame immediately, AND a change after it still animates — because
the first alone passes with every animation deleted. **Its two probe defects are
the reusable part**: it ticked without DRAWING (the store only learns a value
when the renderer observes one, so it never showed the widgets the placeholder
and passed with the bug fully present), and its positive control turned a
two-option enum already at its top, so nothing moved.

### The neighbour lane warms page ±1, and it is NOT the same fix

`observeLanded` stops what arrives from moving; it does not make it arrive any
sooner. The rotation still serves one key per tick, so jogging to a cold page
means watching it populate a cell at a time. `neighbourPrefetch` spends a
CONDITIONAL extra rotation stop on one uncached key belonging to page ±1, so a
warm neighbourhood costs nothing at all and a cold one is bounded by the sixteen
keys either side of you. Ported from the `page-slide-transition` branch
(`2ba94c0b`), where it existed because a page whose cells fill in *while it
slides* is what the slide was added to avoid.

Held off for one full pass after a page change (`PREFETCH_HOLD_TICKS`, 12 — the
page you ARRIVED on owns the screen) and entirely while any key is settling (a
knob is under a finger). Both are HOLDS: the lane resumes on its own, and "no
reads happened" cannot distinguish a hold from a lane that is switched off,
which is why each is tested against a positive control.

**`fullKey` gained an optional PAGE argument for this** — it resolves a
child-level template against whichever page is passed, defaulting to the current
one, so a bare key would ask the wire about `synth:tune` for a neighbour serving
`synth:part2_tune`: a number read off the wrong parameter, cached under the bare
key, with nothing on screen to say so.

**The two fixes are independent, and the ablation matrix is the evidence** (a
jog onto a page carrying all three animated widgets):

| lane | observeLanded | warmed | ticks to fill | animates in |
|---|---|---|---|---|
| on | on | 3/3 | 0 | no |
| off | on | 0/3 | 3 | no |
| on | off | 3/3 | 0 | no |
| off | off | 0/3 | 3 | **yes** |

Either one alone suppresses the animation *on a jog* — but only the lane removes
the fill-in, and only `observeLanded` covers a component's FIRST page, which the
lane can never reach because nothing is adjacent to a page set that does not
exist yet. Keep both.

**The probe that produced that table was wrong first, in the now-familiar way:**
it jogged and snapshotted without TICKING, so no value ever *arrived* and the
animation axis could not move — every row read "settled", including the
all-disabled control. A matrix whose control cannot fail is not a matrix.

`tests/host/test_neighbour_prefetch.sh` asserts a read COUNT, because "the
values are there" passes just as well with a lane that reads every tick forever.
Four mutants killed on this branch: lane disabled, hold removed, child key
resolved against the wrong page, and the tri-state ignored so a failed read is
cached.

### The FIRST page is warmed synchronously, because nothing is adjacent to it

The lane cannot reach a component's first page — nothing is adjacent to a page
set that does not exist yet — so entry still filled one key per tick, ~9 ticks
(~150ms), with every cell drawing a confidently WRONG picture until its value
landed. Reported from the device: *"all of the controls up for a frame or so
with the wrong value before snapping to the right one"*.

**It snaps together rather than filling in cell by cell because of the viz
groups.** obxd's Main page draws a filter curve from four keys and an ADSR from
four more, so a graphic stays wrong until its LAST member arrives and then the
whole thing jumps. Rendered, frame 0 had the filter curve collapsed into the
bottom-left corner, the envelope a spike at the left edge, and Octave reading
`--`. Suppressing the ANIMATION did not stop the placeholder being DRAWN — same
rule, one layer up.

`warmCurrentPage()` reads the entered page's keys before the first frame. It is
called from the LOAD path, never from `tick()`: the controller is built during
input handling and the draw happens on a later frame, so a warm here lands
before anything is shown while a warm on the tick is always one frame late —
and one frame late is the whole bug. Measured over the fleet: **all 95 modules
now draw frame 0 identical to settled**, worst entry cost 8 reads ≈ 22 ms,
capped by the 8-knob page.

**It stops at the FIRST failed read, and that bound matters more than the warm.**
A module not serving yet — minijv and osirus are the slowest in the fleet —
costs one timeout instead of eight, and the rotation retries for free. Entry
stalling on eight dead reads is a worse failure than the flash this removes.

**IT RUNS ON EVERY PAGE CHANGE TOO, and the first cut did not.** That version
argued the lane already keeps neighbours warm so a jog finds them cached, and
blocking would "put a hitch on the exact gesture the lane exists to smooth."
Measured, that is false at any speed a hand actually jogs. The lane fires on ONE
stop of a ~10-stop rotation — one neighbour key per ~10 ticks, so eight keys is
~80 ticks plus the 12-tick hold. Against a 3 × 8-knob module, by dwell before
jogging on:

| dwell | known on arrival | fill-in |
|---|---|---|
| 200 ms | 1/8 | 153 ms |
| 500 ms | 3/8 | 153 ms |
| 1000 ms | 6/8 | 153 ms |
| 1500 ms | 8/8 | none |

So the lane only wins if you sit on a page for a second and a half. Reported
from the device as *"i still see it … just going from one page to another
slowly"* — precisely the 200–1000 ms band. The old objection is answered by the
measurement: the alternative is not a smooth gesture, it is 153 ms of WRONG
PICTURE, and ~22 ms of nothing is better. With the warm on the hop, every dwell
arrives 8/8 and settles in one frame, and the cost degrades gracefully — 22 ms
at a fast jog, 0 once the lane has kept up.

**That is what the lane is actually for**, and it is worth stating because the
first cut had it backwards: the lane does not make the page correct, the warm
does. The lane makes the warm FREE, turning a per-hop cost into an occasional
one. Neither is redundant.

`goToPage` gets it too — that is the path a far JUMP from the section picker
takes, where the lane has warmed nothing at all.

**`acceptValue` is the extraction that made this safe.** The tri-state here is
three rules deep and every one was a shipped bug — a failed read is not a value;
`""` is a MISS for a number or enum (`Number("") === 0` put a silent zero on the
slot-settings Volume knob); `""` is a VALUE for an opaque key (an empty filepath
is NONE). The rotation and the warm share it rather than each carrying a copy.
The condition re-plan lives in it too: a warm that stored a condition key without
replanning would leave the rotation reading the same value later, seeing no
change, and never revealing the pages that key gates.

`warmCurrentPage` therefore runs **two passes** — `acceptValue` can re-plan
underneath it, swapping the page being warmed for a different key set. The
second pass is free when nothing changed (every key is already cached, so it
makes no reads). The bound of 2 is DEFENCE, not behaviour: no fleet contract
reaches it, and raising it to 99 kills no test — recorded so the survival is not
read as a coverage hole, exactly like the prefetch's two guards.

`tests/host/test_page_entry_warm.sh` asserts **exactly** one read per key, not
"at least": the cached-key skip is invisible on the happy path, and without it a
plain page costs 16 reads — twice the entry budget, and a mutant that survived
the first version of this test.

### A module-supplied widget draws into a FRAME, and cannot name a screen pixel

A module can replace one cell's graphic with its own drawing. It declares the
kind on the existing `viz` field — `viz` is an object, so the namespace goes on
its `kind`: `viz: { kind: "custom:mymeter" }`, in exactly the shape a built-in is
declared, grouped or single. `custom:` is a **reserved prefix**; no built-in kind
may ever be named into it.

The implementation is a `drawCell` hook on the same `canvas.js` overlay that
already provides the fullscreen `draw` — one file, one author mental model, two
scales. `shadow_ui.js` registers it at the overlay load and clears the registry
in `resetCanvasState`, because the registry is process-global and shadow_ui is
long-lived: a widget left registered would outlive its module, and a later module
reusing the name would silently inherit the wrong art.

**The frame is not a rect the widget may reason about in screen terms.**
`frame_ctx.mjs` gives `(0,0)` as the box's top-left, `width`/`height` as the
box's, and no accessor that reaches absolute space. That is stronger than
clipping-as-safety-net, and it has to be, because the rect is unstable in ways an
author cannot see from one screenshot:

* `render_page.mjs:619` — `cellW = floor(rect.w / COLS)`, caller-dependent.
* `render_page.mjs:116` — `rowH` is **dynamic**, and `computeGeom` picks the whole
  render mode from it (dial → shrinking radius → bar-value → bar-label → bar-only).
* `render_page.mjs:671` — `Math.min(g.slotSpan, COLS - col)` silently **clamps** a
  two-slot group near the right edge.

Driving the real renderers across a rect sweep produces **sixteen distinct frame
sizes** — 32×14 through 32×26, plus 16×15, 24×15 and 25×15 from clamped spans.
`tests/host/test_widget_frame_matrix.sh` captures them from the renderers rather
than listing them, because `computeGeom`'s thresholds are module-private and a
written-down table would go stale silently and green.

**The frame ctx carries no `getParam`,** so the "nothing reads on the draw path"
rule holds by construction. `clipped()` counts attempted overflow rather than
absorbing it — the same bargain as `test_master_fx_diagram_fit.sh`, which exists
because a fixed-width row cannot report that it overflowed.

**The label is not the widget's.** Movy already splits `KW = 17` art from
`drawLabelCell`, so Schwung keeps drawing every label and a page mixing custom and
built-in cells stays consistent.

#### An unknown kind falls through by NOT CLAIMING, and that is the whole story

`collectDeclared` claims keys as it walks. A custom kind that is not registered
simply does not claim, so its keys stay in the detector pool and the built-in
draws. One branch covers four failures — an author typo, a widget whose script
failed to load, an **older host reading a newer module**, and a widget disabled
after throwing. They share a path, so forward compatibility cannot rot separately
from typo handling.

**The guard's placement is load-bearing, and the obvious placement is wrong.** A
group's `kind` may be declared on *any* member, so guarding in the shared walk
drops only the member carrying it: the remaining roles still form a group,
`inferKindFromRoles` still names it, and an `attack` declaring an unavailable
custom kind silently becomes a **three-cell envelope with its own key orphaned
beside it**. The singles guard therefore lives inside the `else if (v.kind)`
branch, and the group loop `continue`s so the whole group is abandoned at once.
A test that only asserts "some envelope exists" goes green over the broken
version — pin the span.

An unavailable kind is **not** recorded in `invalid`, and `validate_contract`
does not flag it: it is legal, being exactly what an older host sees. What it
does flag is a `custom:` kind on a module shipping no `canvas_script` — the
author forgot the file, and nothing else would ever say so. That check is silent
when `capabilities` is not supplied, because the caller cannot answer it.

#### One strike, and the full-page sprite cost

A throwing widget is disabled for the session and the throw is logged; the next
`resolveViz` stops claiming its keys and the built-in draws. Catching every frame
would flood the log and burn the budget forever; not catching would take the
shadow UI down for someone who merely installed a module.

For sprite art, **check the full-page cost, not the per-sprite one.** A binding
is ~490 ns, so a 17×15 knob box blitted per pixel is 255 calls ≈ 125 µs — which
sounds survivable against a 1.68 ms render until you notice a page holds **eight**
boxes, and a module shipping one custom widget ships eight. That is ~1 ms gone
before anything else draws. Run-length rows are ~45 calls ≈ 22 µs, so a page is
~180 µs. That is what makes RLE non-optional rather than a nicety.

1-bit art is **never fractionally scaled** — it dithers into mush. A sprite
carries the nominal frame it was drawn for, anchors 1:1 with integer scale only
on an exact multiple, and is refused when it does not fit, at which point the
built-in draws a correct picture instead of a smeared one.

#### A widget's lifetime is its COMPONENT's, and the obvious hook is the wrong one

Registration originally sat at the canvas overlay load, which reads as the
natural place: it is where a module's `canvas.js` is parsed and where its
`drawCell` first exists. It is wrong, and three ways at once.

`openCanvasPreview` is the only caller of that load, and it fires when the user
**clicks a `type: "canvas"` param**. So an in-grid widget did not appear on first
paint — only after the fullscreen canvas had been opened once. Both opening and
closing that canvas call `resetCanvasState`, so a `clearWidgets()` there made the
widget vanish again on the way out. And a module wanting *only* an in-grid
widget, with no canvas param at all, never registered anything.

`ensureComponentWidgets` runs where a component's `chain_params` become known,
reads `canvas.js` only when the contract actually declares a `custom:` kind, and
no-ops when the module has not changed. `resetCanvasState` must not touch the
registry.

**No source-level test can catch this**, which is the transferable part. The
lines were all present and correct; only their *call ordering* was wrong.
`tests/host/test_canvas_drawcell_wiring.sh` pins what a source test *can* see —
which function owns the lifetime and which must not touch it — and the behaviour
is covered by `tests/host/test_widget_module_poc.sh`, which starts from the
shipped `src/modules/tools/widget-test/{module.json,canvas.js}` rather than
calling `registerWidget()` directly. Every other widget test registers directly,
and that is exactly why none of them saw this.
