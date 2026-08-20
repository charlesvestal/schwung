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

/**
 * SHALLOW on purpose. The lists are copied, but the module objects inside them
 * are SHARED with the input config -- `next.fx[0] === cfg.fx[0]`. Replace a
 * module (replaceAt) rather than mutating one: writing to `module.params` on a
 * returned config writes through to the config it came from, and to every
 * other config derived from the same original.
 */
const clone = (c) => ({
    midiFx: c.midiFx.slice(), synth: c.synth, fx: c.fx.slice(),
});

/**
 * "fx3" -> { section: "fx", index: 2 }, or null.
 *
 * Ids are ONE-BASED, and the lower bound is load-bearing rather than
 * defensive: a zero index becomes -1, which splice() counts from the END of
 * the list, so removeAt("fx0") would delete the user's LAST effect instead of
 * doing nothing. Ids are model-generated today, but they also arrive parsed
 * back out of param keys and persisted state, so a malformed one is not
 * hypothetical. Reject it here, once, rather than at each of the three call
 * sites that splice.
 */
export function parseId(id) {
    const m = /^(midi_fx|fx)(\d+)$/.exec(String(id || ""));
    if (!m) return null;
    const n = parseInt(m[2], 10);
    if (n < 1) return null;
    return { section: m[1] === "midi_fx" ? "midiFx" : "fx", index: n - 1 };
}

const idFor = (section, i) => (section === "midiFx" ? `midi_fx${i + 1}` : `fx${i + 1}`);
const labelFor = (section, i) => (section === "midiFx" ? `MIDI FX ${i + 1}` : `FX ${i + 1}`);

/**
 * The chain as positions, in signal order.
 *
 * Patch and Settings keep the extreme ends — outside the `+` boxes — so the
 * gesture that reaches them is the one it has always been.
 *
 * EVERY position carries a label, modules included, so a renderer never has to
 * ask which kind it is holding before it can draw a name, and the number in
 * "FX 2" is derived here once rather than at each call site.
 */
export function chainComponents(cfg) {
    const out = [{ id: "patch", kind: "patch", label: "Patch" }];
    out.push({ id: "add_midi", kind: "add", section: "midiFx", label: "+" });
    cfg.midiFx.forEach((m, i) => out.push({
        id: idFor("midiFx", i), kind: "module", section: "midiFx", index: i,
        label: labelFor("midiFx", i), module: m,
    }));
    out.push({ id: "synth", kind: "synth", label: "Synth", module: cfg.synth });
    cfg.fx.forEach((m, i) => out.push({
        id: idFor("fx", i), kind: "module", section: "fx", index: i,
        label: labelFor("fx", i), module: m,
    }));
    out.push({ id: "add_fx", kind: "add", section: "fx", label: "+" });
    out.push({ id: "settings", kind: "settings", label: "Settings" });
    return out;
}

/**
 * Where a position sits in chainComponents, or -1.
 *
 * The UI selection is an index into that list and EVERY mutation shifts it:
 * removeAt("fx1") shortens the chain, so a selection that pointed at Settings
 * now points at the `+` box. Callers re-anchor by id through here rather than
 * carrying an index across an edit.
 */
export function indexOfId(cfg, id) {
    return chainComponents(cfg).findIndex((p) => p.id === id);
}

const capOf = (section) => (section === "midiFx" ? MAX_MIDI_FX : MAX_FX);
const isSection = (section) => section === "midiFx" || section === "fx";

/** Append at the OUTERMOST end of a section. There is deliberately no
 *  insert-at-a-position: inserting in the middle is append-then-move, one
 *  operation rather than two gestures. `section` is "midiFx" or "fx". */
export function appendTo(cfg, section, module) {
    const next = clone(cfg);
    /* An unknown section is inert, like every other bad input here. It used to
       throw, which on the shadow UI tick reaches the user as "UI error,
       recovering" rather than as nothing happening. */
    if (!isSection(section)) return next;
    if (next[section].length >= capOf(section)) return next;
    next[section] = next[section].concat([module]);
    return next;
}

/**
 * Replace the occupant of a position. NOTHING MOVES: replacing fx1 while fx2
 * exists leaves fx2 exactly where it was, and the list keeps its length.
 *
 * The user-facing word for this is "swap", but in list vocabulary a swap
 * exchanges two positions — which is what moveBy does internally — so the
 * identifier says what it does instead. This and removeAt sit one entry apart
 * in the same picker, and getting them the wrong way round would resequence a
 * patch the user only meant to retouch.
 */
export function replaceAt(cfg, id, module) {
    const at = parseId(id);
    const next = clone(cfg);
    if (!at || at.index >= next[at.section].length) return next;
    const list = next[at.section].slice();
    list[at.index] = module;
    next[at.section] = list;
    return next;
}

/**
 * Take a module out and CLOSE THE GAP.
 *
 * This is a behaviour change from the fixed-shape chain, where clearing fx1
 * left an empty fx1 sitting in front of fx2. Now fx2 becomes fx1 and the chain
 * gets shorter, so a removal renumbers everything downstream of it.
 */
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
    /* A selection carried across an edit that shortened the chain can point
       past the end. Clamping it to a real position states that contract where
       `selected` arrives; the two clamps below already force the same result,
       so this changes no output — it is here so the invariant does not have to
       be re-derived from them. */
    const sel = Math.min(Math.max(selected, 0), total - 1);
    let first = sel - Math.floor(capacity / 2);
    if (first + capacity > total) first = total - capacity;
    if (first < 0) first = 0;
    return { first, count: capacity };
}
