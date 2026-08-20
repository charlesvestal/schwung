/**
 * chain_model.mjs — a slot's signal chain as an ordered list.
 *
 * Pure: no device, no drawing, no globals. shadow_ui.js used to compute chain
 * order inline from a five-entry literal and 54 hardcoded fx1/fx2 references;
 * this owns it instead, so "what is in this chain and in what order" has one
 * answer that can be tested without a Move.
 *
 * NAMING IS UNCHANGED. Position is derived from the index, so a module keeps
 * its id (`fx3`) as it moves and LFO targets and knob mappings — which key on
 * the id — follow it without being re-pointed. No saved state migrates.
 */

/** Per-section cap. Static, so the audio path allocates once and the state
 *  size stays bounded; the CPU runs out long before eight FX anyway. */
export const MAX_FX = 8;
export const MAX_MIDI_FX = 8;

export function emptyChain() {
    return { midiFx: [], synth: null, fx: [] };
}

const clone = (c) => ({
    midiFx: c.midiFx.slice(), synth: c.synth, fx: c.fx.slice(),
});

/** "fx3" -> { section: "fx", index: 2 }, or null. */
export function parseId(id) {
    const m = /^(midi_fx|fx)(\d+)$/.exec(String(id || ""));
    if (!m) return null;
    return { section: m[1] === "midi_fx" ? "midiFx" : "fx", index: parseInt(m[2], 10) - 1 };
}

const idFor = (section, i) => (section === "midiFx" ? `midi_fx${i + 1}` : `fx${i + 1}`);

/**
 * The chain as positions, in signal order.
 *
 * Patch and Settings keep the extreme ends — outside the `+` boxes — so the
 * gesture that reaches them is the one it has always been.
 */
export function chainComponents(cfg) {
    const out = [{ id: "patch", kind: "patch", label: "Patch" }];
    out.push({ id: "add_midi", kind: "add", section: "midiFx", label: "+" });
    cfg.midiFx.forEach((m, i) => out.push({
        id: idFor("midiFx", i), kind: "module", section: "midiFx", index: i, module: m,
    }));
    out.push({ id: "synth", kind: "synth", label: "Synth", module: cfg.synth });
    cfg.fx.forEach((m, i) => out.push({
        id: idFor("fx", i), kind: "module", section: "fx", index: i, module: m,
    }));
    out.push({ id: "add_fx", kind: "add", section: "fx", label: "+" });
    out.push({ id: "settings", kind: "settings", label: "Settings" });
    return out;
}

const capOf = (section) => (section === "midiFx" ? MAX_MIDI_FX : MAX_FX);

/** Append at the OUTERMOST end of a section. Insert-in-the-middle is
 *  add-then-move; one operation, not two gestures. */
export function insertAt(cfg, section, module) {
    const next = clone(cfg);
    if (next[section].length >= capOf(section)) return next;
    next[section] = next[section].concat([module]);
    return next;
}

/** Replace the occupant. Nothing moves — see the swap/remove distinction. */
export function swapAt(cfg, id, module) {
    const at = parseId(id);
    const next = clone(cfg);
    if (!at || at.index >= next[at.section].length) return next;
    const list = next[at.section].slice();
    list[at.index] = module;
    next[at.section] = list;
    return next;
}

/** Take it out and CLOSE THE GAP. */
export function removeAt(cfg, id) {
    const at = parseId(id);
    const next = clone(cfg);
    if (!at || at.index >= next[at.section].length) return next;
    const list = next[at.section].slice();
    list.splice(at.index, 1);
    next[at.section] = list;
    return next;
}

/** Move within the module's OWN section. Crossing the synth would be a type
 *  change rather than a reorder, so it is not offered; the ends stop rather
 *  than wrap, because wrapping a signal chain means nothing. */
export function moveBy(cfg, id, delta) {
    const at = parseId(id);
    const next = clone(cfg);
    if (!at) return next;
    const list = next[at.section].slice();
    const to = at.index + delta;
    if (at.index >= list.length || to < 0 || to >= list.length) return next;
    const [m] = list.splice(at.index, 1);
    list.splice(to, 0, m);
    next[at.section] = list;
    return next;
}

/**
 * Which positions are on screen.
 *
 * HYBRID, per the design: while everything fits nothing scrolls and the synth
 * sits where the user drew it. Past that the window follows the SELECTION,
 * because a jog-driven UI must never be editing something off-screen.
 */
export function scrollWindow(total, selected, capacity) {
    if (total <= capacity) return { first: 0, count: total };
    let first = selected - Math.floor(capacity / 2);
    if (first + capacity > total) first = total - capacity;
    if (first < 0) first = 0;
    return { first, count: capacity };
}
