# Page slide transition — design

2026-08-27. Branch `page-slide-transition`.

Jogging between pages of the param-pages knob grid cuts instantly from one
page to the next. This adds a horizontal slide, and makes the incoming page
arrive with its values already populated rather than filling in while you
watch.

## What moves

Three bands, from `list_geometry.mjs`: header rows 0–6, the bank bar at row 7,
the body rows 9–54, the footer rows 57–63.

**The body (rows 9–54) slides one screen width. The page NAME slides within
its own right-hand header column (~58px) over the same duration. The module
title, the bank bar and the footer are all FIXED.**

This was decided twice, and the second decision came from looking at the
filmed frames rather than at a description.

The first version slid the whole header with the body, on the argument that
the page name lives there and is part of the page. What that actually
produces is this:

```
LFO 1 T1 > OSIRUS      <- "LFO 1" is page A's name leaving,
                          "T1 > OSIRUS" is page B's TITLE arriving
```

The title is identical on every page of a module. Sliding it out and sliding
a byte-identical copy back in is motion that carries no information — it
makes the header wobble while saying nothing. Only the page name changes, so
only the page name moves.

**The name gets a SHORTER travel than the body, and that is not a
decoration.** It is right-aligned, so a full-width travel takes it off the
left edge early and brings the incoming one in late: at the chosen 90ms, that
is roughly two of the five frames with no page name on screen at all. Sliding
it within its own column means something is always legible there. Body and
name move together over one duration; the name simply has less ground to
cover.

**Clipping the name needs a FILLED ERASE, not an overdraw — and the first
version of this document got that wrong.**

It claimed the outgoing name's left edge would be clipped by drawing the
fixed module title over it, "the same trick the rest of the feature uses".
It is not the same trick and it does not work. `drawHeader`'s un-inverted
branch paints **no background**: it prints glyphs and nothing else (the
*inverted* branch is the one that fills). Overdrawing therefore clips only
where the title happens to have ink, and a title is text with gaps between
its letters — a name sliding leftward would show through them.

The rest of the feature's clipping is real: `js_display_set_pixel`
(`src/host/js_display.c`) *discards* writes outside the buffer. That is a
genuine clip. Painting something on top is a different mechanism wearing the
same name, and it only clips if the thing on top is opaque.

So the header column gets an explicit `fillRect(…, 0)` over the title's span
before the title is printed. On a 1-bit panel that is one cheap rect.

**The column is not a fixed width, either.** `drawHeader` measures the RIGHT
side first and gives the left the remainder, floored at `HEADER_MIN_LEFT`
(70px) — so both the column's width and its x-origin depend on the specific
page name. Two different names produce two different columns, and names
sliding within different geometries do not abut. The transition must fix ONE
column geometry for its whole duration: the destination name's, so the layout
the user ends on is the correct one.

The bank bar is fixed because it is the page *indicator*. It cannot travel
with the page it indicates without being unreadable for the whole transition,
which is the one element whose job is to answer "where am I".

## Mechanism

Everything both renderers draw funnels through an injected `ctx` with five
methods — `fillRect`, `print`, `textWidth`, `line`, `drawArc` — and
`js_display_set_pixel` (`src/host/js_display.c`) already discards anything
outside the 128x64 buffer.

So the slide is: **wrap `ctx` in a translating proxy and render both pages,
the outgoing at `dx = -N`, the incoming at `dx = 128 - N`.**

No clip rectangle, no C change, no offscreen framebuffer, no JS blit. That
holds because of two properties which must not quietly stop being true:

- **The two pages never overlap.** Outgoing owns `[-N, 128-N)`, incoming owns
  `[128-N, 256-N)`. They abut exactly and the screen bounds do the rest.
- **The body's ink stops at row 54.** `ROW1_Y` 33 + `LBL1_Y` 48 + a 7-row
  label ends at 54; `RULE_Y` 55 belongs to the footer, which is suppressed on
  the sliding passes.

Both sliding passes suppress the bank bar and the footer; both are redrawn
fixed, unproxied, after the composite.

Suppression is explicit, not incidental. `renderPageMovy` calls `drawBankBar`
unconditionally and `drawFooter` when `o.footer` is set, so it gains one
option — `o.bankBar === false` skips the bar — and the sliding passes pass
`footer: null`. Passing `pageCount: 1` would also make `drawBankBar` return
early, and is rejected: it lies to the renderer about the page set to obtain a
drawing side effect.

Cost is two page renders per frame. A page render is 1.68ms, so ~3.4ms against
a ~18ms tick.

### Two hazards worth naming

**`line` and `drawArc` are optional.** Both renderers guard with `typeof
ctx.line === "function"` and fall back to a JS Bresenham
(`render_page_movy.mjs:659`, `viz_draw.mjs:162`). A proxy that omits them
still draws correctly through the fallback; a proxy that forwards them
*without* translating draws silently in the wrong place. Neither failure
raises. The test therefore asserts translation **per method**, not presence of
a method.

**`fb.clipped()` stops being a correctness probe on transition frames.** The
filming harness counts out-of-bounds writes, and a transition frame produces
many by design. Existing scenes keep asserting `clipped() === 0`; transition
scenes must not, and correspondingly lose that check against real overflow.

## The refactor `render()` needs

`render()` in `page_controller.mjs` draws *the current page*: it reads
`page()` and `s.pageIndex` in roughly six places across the four page kinds
(knobs, menu, items, preset). Drawing the outgoing page means drawing an index
that is no longer current.

Extract **`drawPage(ctx, index, { title, footer, chrome })`** from the existing
`LAYOUT_MOVY || LAYOUT_LIST` branch, covering all four kinds. Per-page state
(menu cursor, items list, preset browser cursor) is already keyed by page name
in `s`, so drawing a non-current index needs no new state.

`render()` becomes:

- no transition → `drawPage(ctx, s.pageIndex, …)`
- transition → two body passes at `chrome: false, header: false`, then the
  fixed chrome: the two page NAMES at column-relative offsets, the module
  title drawn over them, then the bank bar and the footer.

The order inside the chrome pass is load-bearing. The names are drawn first
and the title over them, because the title's opaque redraw is what clips the
outgoing name's left edge — there is no clip rectangle anywhere in this
feature.

Overlays — hint, section picker, knob card, enum peek — are drawn after the
composite and never slide.

**Scope is `LAYOUT_MOVY` and `LAYOUT_LIST` only.** `LAYOUT_DIAL`
(`render_page.mjs`) keeps snapping: it is not the device grid, and suppressing
its chrome is separate work with no user visible today.

**Accepted discontinuity:** `onJog` clears `s.menuEntered` before the slide
starts, so a menu page you page away from draws un-entered while it leaves.
The transition record captures `menuEnteredAtStart` so this stays fixable, but
it ships as-is.

## Transition state

```js
s.transition = { fromIndex, toIndex, startMs, menuEnteredAtStart }
```

Started by `onJog` and by `goToPage` whenever the page index actually changes
— so plain jog, Shift+jog level steps, and section-picker jumps all slide.
Direction is the sign of the index delta; **distance is always one screen
width**, regardless of how many pages a jump crossed.

**Chase, not queue.** A jog arriving mid-slide rewrites `toIndex` and rebases
`startMs` so the offset continues from where it currently is. Spinning the
encoder fast reads as continuous motion and never falls behind it; only ever
two pages are on screen at once. Queueing each detent's full slide would put
the screen seconds behind a fast spin.

The clock comes from the controller's existing `now()`, the same source
`s.anim` already uses. The grid already calls `ctx.requestRedraw()` every tick
and `MOVY_REDRAW_MIN_MS` is 0, so no redraw gating change is needed.

## Value prefetch

`tick()`'s round-robin walks `page.keys`, plus one stop for the preset name,
plus one per viz extra key — one parameter read per tick, ~2.8ms each.

Add a **neighbour lane**: once a full pass has completed on the current page
and nothing is settling, spend that tick's stop on a key belonging to page ±1
that is not yet in `s.values`.

Bounded by construction — only uncached keys, only two neighbour pages, and it
goes quiet once they are warm. It costs nothing at jog time. If you outrun it
(spinning fast through pages never visited), the page fills mid-slide, which is
today's behaviour rather than a regression.

Rejected: a blocking prefetch at jog time. Up to eight uncached keys is ~22ms
of dead time on a page's first visit — a visible hitch on the exact gesture
this feature exists to smooth.

**Stale values are accepted deliberately.** A value cached a minute ago on
another page is corrected by the ordinary rotation within ~150ms of arrival,
and a plausible stale number beats a blank.

`s.values` is keyed by param key and already shared across pages, so no cache
restructuring is required.

## Motion parameters — DECIDED

**90ms, eased (exponential ease-out).** `SLIDE_MS = 90`,
`advanceScroll = advanceEased`. Five real frames.

Chosen from the filmed GIFs after two rounds. The first round offered
160/200/280ms; all read as slower than wanted. 90ms is deliberately at the
floor of what the panel can draw — "just enough to get a feel for what's
happening".

**`ms` MEANS THE SETTLE TIME, and it did not at first.** `advanceEased`
originally used `tau = ms / 3`, which makes `ms` mean 95% of the travel — so
a nominal 200ms actually settled at 364ms and a nominal 90 at 164. The
filming exposed it by measuring settle times instead of trusting labels. The
honest tau is derived from the snap threshold: arrival is when the remaining
distance reaches `SNAP_PAGES`, so `T = tau · ln(1 / SNAP_PAGES)`. The divisor
is derived from `SNAP_PAGES` in code rather than written as a literal,
because the two are coupled and a change to the snap threshold must not
silently change what every duration means.

A measured trade-off worth recording: at a 90ms budget, **linear uses the
time better than eased does**. Eased front-loads 52% of the travel into the
first frame and spends the remaining four on the last 20%, so only one frame
is a genuine both-pages picture; linear's even 17–34px steps put both pages
substantially on screen for three of its five frames. Eased was chosen anyway,
with that known.

### How the duration was filmed

Duration and easing were **decided from filmed GIFs, not from this document**.

The film must be honest about a hardware constraint recorded in
`shadow_ui_param_pages.mjs`: this device's clock is quantized to roughly
11–12ms, and the grid ticks at ~55Hz. A 128px slide over 200ms is about 11 real
frames of ~12px each. Filming at a smooth 30fps against a continuous clock
would flatter the motion into something the hardware cannot produce.

So the transition scene added to `tools/param-pages/movie.mjs` films at the
device's actual tick cadence with the clock quantized to the device quantum.
An unquantized 30fps film of the same transition is the wrong probe, and would
report green on a motion that stutters on hardware.

## Testing

- **Proxy**: each forwarded method translates its x coordinates. Asserted per
  method — a forgotten `line` is silent, and a forwarded-but-untranslated one
  is silent in the other direction.
- **`drawPage` at a non-current index** draws that page's content, not the
  current page's.
- **Composite**: a mid-transition frame differs from both endpoint frames, and
  the bank bar row and the footer rows are byte-identical to the un-animated
  frame of the destination page.
- **Chase**: a second jog mid-slide retargets `toIndex` without the rendered
  offset jumping backwards.
- **Prefetch**: the neighbour lane issues reads only for uncached neighbour
  keys and stops once they are warm — asserted as a read **count**, because
  "the values are there" passes just as well with a lane that reads every tick
  forever.

Frame assertions go on the pixel buffer via the existing harness, not on draw
calls: on a 1-bit screen a stroke, a dither and a highlight all light the same
pixel, so a draw-call assertion can pass while the picture is wrong.
