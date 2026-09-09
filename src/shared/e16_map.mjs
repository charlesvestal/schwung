/*
 * e16_map.mjs -- the pure model behind the OXI E16's Shift-held MAP.
 *
 * The E16 is a 4x4 grid of push/turn encoders. Holding Shift turns it into a
 * map of the chain: the top row (cells 0-3) is always the four slots, and the
 * bottom three rows (cells 4-15) are the SELECTED slot's occupied components,
 * in chain order (MIDI FX, then the synth, then audio FX) -- so a player can
 * jump anywhere in the chain without ever touching the Move.
 *
 * This module only computes the 16 cells from a chain shape; it draws
 * nothing and owns no device state, for the same reason chain_model.mjs and
 * bus_model.mjs are pure -- every rule here has to run under node in
 * tests/host, off the device.
 *
 * ================= HOLES ARE NOT DESTINATIONS ================================
 *
 * A chain position can be empty (no MIDI FX loaded in that slot, no synth,
 * an unused audio FX slot). An empty position cannot be edited, so offering
 * it as a lit button offers a hole: press it and there is nothing to land
 * on. buildMap therefore compacts each slot's occupied components before
 * laying them into cells -- an empty position produces NO cell (null), it
 * does not produce a dark placeholder holding its spot.
 *
 * ================= OVERFLOW =================================================
 *
 * The real per-slot caps are 8 MIDI FX + 1 synth + 8 audio FX = 17 possible
 * components, against 12 cells below the slot row. A normal rig can exceed
 * 12 occupied components, so overflow is reachable and must not silently
 * drop the tail: buildMap paginates at 12 per page and reports pageCount so
 * the caller can offer a next-page gesture rather than lose components off
 * the edge.
 *
 * ================= STABILITY IS BEHAVIOURAL, NOT COSMETIC ===================
 *
 * Recomputing the layout every frame would move buttons under the user's
 * fingers while they are still navigating -- a component added to a slot
 * they are not looking at should not reflow the cell they are about to
 * press. shapeSignature() gives the caller a cheap value to compare so it
 * can skip the rebuild when the shape has not actually changed; buildMap
 * itself takes no cache and does no memoisation -- callers own that, this
 * module just has to be safe to call on every shape check.
 *
 * ================= COMPONENT ID SCHEME =======================================
 *
 * Cell.component identifies a chain position so the surface can address it
 * later (set_param routing, chain edits) without re-deriving position from
 * the cell's on-screen order:
 *
 *   "slot"          the top-row slot cells (component is unset; slot cells
 *                    are identified by their `slot` field alone)
 *   "midi_fxN"       Nth MIDI FX position, 1-indexed by position in
 *                    chain.slots[s].midiFx (not by occupied order -- an
 *                    empty slot 2 still leaves slot 3 addressed as
 *                    "midi_fx3")
 *   "synth"          the slot's single sound generator
 *   "fxN"            Nth audio FX position, 1-indexed the same way
 *   "busN"           Nth bus, 1-indexed the same way, only when
 *                     showBuses is set
 *
 * 1-indexing (not 0) matches the chain host's own param-key convention
 * (`fx1`.."fx8`, see docs/CHAIN.md) so a caller can string-concat straight
 * into a param key without an off-by-one translation layer.
 */

const CELLS_PER_PAGE = 12;
const SLOT_CELLS = 4;

/* A slot object may omit any of midiFx/synth/fx/buses entirely (a bare `{}`
 * is valid input -- an unpopulated slot) so every read below defaults to
 * empty rather than throwing. */
function slotList(slot, key) {
  return Array.isArray(slot && slot[key]) ? slot[key] : [];
}

function slotSynth(slot) {
  return slot && slot.synth ? slot.synth : null;
}

/* Walks one array-valued component list (midiFx / fx / buses) and emits one
 * entry per OCCUPIED position -- a null or falsy entry is a hole and is
 * skipped outright, never emitted as a placeholder. */
function occupiedFromList(list, kind, idPrefix, slotIndex) {
  const out = [];
  for (let i = 0; i < list.length; i++) {
    const value = list[i];
    if (!value) continue; // hole -- not a destination
    out.push({
      kind,
      label: String(value),
      slot: slotIndex,
      component: idPrefix + (i + 1),
    });
  }
  return out;
}

/* The ordered list of a slot's occupied components: MIDI FX, then synth,
 * then audio FX -- signal-chain order, matching the [Input] -> [MIDI FX] ->
 * [Sound Generator] -> [Audio FX] -> [Output] pipeline. */
function occupiedComponents(slot, slotIndex) {
  const out = [];
  out.push(...occupiedFromList(slotList(slot, 'midiFx'), 'midi_fx', 'midi_fx', slotIndex));
  const synth = slotSynth(slot);
  if (synth) {
    out.push({ kind: 'synth', label: String(synth), slot: slotIndex, component: 'synth' });
  }
  out.push(...occupiedFromList(slotList(slot, 'fx'), 'fx', 'fx', slotIndex));
  return out;
}

function occupiedBuses(slot, slotIndex) {
  return occupiedFromList(slotList(slot, 'buses'), 'bus', 'bus', slotIndex);
}

/*
 * buildMap(chain, { slot, page, showBuses }) -> { cells, pageCount }
 *
 * chain.slots is a 4-element array; each slot may be `{}` or omit any of
 * `midiFx` (array), `synth` (string|null) and `fx` (array) -- and `buses`
 * (array) when showBuses is used.
 *
 * cells is always 16 entries: 0-3 are the slot row, 4-15 are either the
 * selected slot's occupied components (default) or its occupied buses
 * (showBuses: true), sliced to the requested page. A cell beyond the
 * occupied content, or a hole in the source list, is `null`.
 */
export function buildMap(chain, opts = {}) {
  const slots = Array.isArray(chain && chain.slots) ? chain.slots : [];
  const currentSlot = opts.slot | 0;
  const page = opts.page | 0;
  const showBuses = !!opts.showBuses;

  const cells = new Array(16).fill(null);

  for (let s = 0; s < SLOT_CELLS; s++) {
    cells[s] = {
      kind: 'slot',
      label: 'Slot ' + (s + 1),
      slot: s,
      current: s === currentSlot,
    };
  }

  const selected = slots[currentSlot];
  const lower = showBuses
    ? occupiedBuses(selected, currentSlot)
    : occupiedComponents(selected, currentSlot);

  const pageCount = Math.max(1, Math.ceil(lower.length / CELLS_PER_PAGE));
  const start = page * CELLS_PER_PAGE;
  const pageItems = lower.slice(start, start + CELLS_PER_PAGE);

  for (let i = 0; i < pageItems.length; i++) {
    cells[SLOT_CELLS + i] = pageItems[i];
  }

  return { cells, pageCount };
}

/*
 * shapeSignature(chain) -> string
 *
 * A cheap-to-compare value that changes exactly when the occupied-component
 * shape of any slot changes (a component loaded/removed, a slot's synth
 * swapped) -- it does NOT change on selection, page, or a component's own
 * parameter values, none of which affect what buildMap lays out. Callers
 * compare this against their last-seen value and skip the rebuild (and the
 * cell reflow that would move buttons under a navigating finger) when it is
 * unchanged.
 */
export function shapeSignature(chain) {
  const slots = Array.isArray(chain && chain.slots) ? chain.slots : [];
  const parts = slots.map((slot) => JSON.stringify({
    midiFx: slotList(slot, 'midiFx'),
    synth: slotSynth(slot),
    fx: slotList(slot, 'fx'),
    buses: slotList(slot, 'buses'),
  }));
  return parts.join('|');
}
