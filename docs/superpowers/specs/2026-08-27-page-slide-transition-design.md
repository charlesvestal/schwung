# Page slide transition — design

2026-08-27. Branch `page-slide-transition`.

Jogging between pages of the param-pages knob grid cuts instantly from one
page to the next. This adds a horizontal slide, and makes the incoming page
arrive with its values already populated rather than filling in while you
watch.

## What moves

Three bands, from `list_geometry.mjs`: header rows 0–6, the bank bar at row 7,
the body rows 9–54, the footer rows 57–63.

**Rows 0–54 slide as one unit, travelling one screen width (128px). The bank
bar and the footer stay fixed.**

The header travels with the body because the page name lives there and it is
part of the page. Sliding the name *alone* was rejected: it is right-aligned,
so a full-width travel takes it off the left edge early and brings the
incoming one in late, leaving roughly a third of the transition with no page
name at all. Giving it a narrower travel inside its own column fixes that and
costs a second moving region with its own geometry. Moving the whole header
instead means the module title occupies that space throughout, so nothing is
ever empty. The title is usually identical page to page, so it slides out and
an identical one slides in — that reads as the page moving, which is what is
happening.

The bank bar is the exception because it is the page *indicator*. It cannot
travel with the page it indicates without being unreadable for the whole
transition, which is the one element whose job is to answer "where am I".

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
- transition → `drawPage(proxy(ctx, -N), fromIndex, { chrome: false })`,
  `drawPage(proxy(ctx, 128-N), toIndex, { chrome: false })`, then the fixed
  bank bar and footer.

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

## Motion parameters

Duration and easing are **decided from filmed GIFs, not from this document**.

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
