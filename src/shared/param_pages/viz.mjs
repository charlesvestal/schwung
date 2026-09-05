/**
 * viz.mjs — resolve a page's parameter groups to a graphic, if any.
 *
 * PURE, like the rest of the library: given a page's keys and a metaIndex, it
 * returns a list of resolved groups. It draws nothing (see viz_draw.mjs) and
 * reads no param. What varies between an author's intent and a guess is only
 * how a group was RESOLVED — that lives entirely in this file, so a renderer
 * never has to know or care which source won.
 *
 * Precedence (docs/MODULES.md "Parameter visualisations (viz)",
 * docs/plans/2026-07-26-param-pages-audit.md §13.5):
 *
 *   module chain_params `viz`  →  module layout file  →  host override  →  detector  →  none
 *
 * The module always wins over the host. A host override may correct a
 * detector but must never overrule an author who did the work.
 *
 * "module layout file" is the still-open, undesigned mechanism tracked in the
 * audit doc (§13.5 item 3) — zero fleet modules ship one today, so there is
 * nothing to read yet. The slot is left as a documented no-op rather than
 * invented here.
 *
 * Order matters for two reasons: it is the order detectors get first refusal
 * on unclaimed keys (a key already grouped by an earlier-firing detector is
 * never reconsidered by a later one), and it is the risk ordering agreed in
 * the audit — envelope first because a wrong ADSR shape is obvious on screen,
 * EQ last because its false positives are the hardest to spot.
 */

import { KIND_NUMBER, KIND_ENUM, KIND_OPAQUE, isTrigger } from "./param_meta.mjs";
import { isCustomKind, isWidgetAvailable } from "./widget_registry.mjs";

/* Matches render_page.mjs COLS. Not imported from there to avoid a cycle
 * (render_page imports this module to draw what it resolves). */
const ROW_WIDTH = 4;

export const VIZ_ENVELOPE = "envelope";
export const VIZ_FILTER = "filter";
export const VIZ_LFO = "lfo";
export const VIZ_WAVEFORM = "waveform";
export const VIZ_FADER = "fader";
export const VIZ_SWITCH = "switch";
export const VIZ_EQ = "eq";
export const VIZ_SAMPLE = "sample";

export const VIZ_SOURCE_DECLARED = "declared";
export const VIZ_SOURCE_OVERRIDE = "override";
export const VIZ_SOURCE_DETECTED = "detected";

/* Time order, which is also draw order: A H D S R. */
const ENVELOPE_ROLE_ORDER = ["attack", "hold", "decay", "sustain", "release"];
const FILTER_ROLE_ORDER = ["cutoff", "resonance", "mode", "slope"];
const LFO_ROLE_ORDER = ["shape", "rate", "depth", "phase"];
const EQ_ROLE_ORDER = ["low", "mid", "high"];

const isNumeric = (m) => !!m && m.kind === KIND_NUMBER;
const isEnum = (m) => !!m && m.kind === KIND_ENUM;

/* --------------------------------------------------------- shared helpers */

/** A row index (0 or 1) for a slot in an 8-knob page. */
function rowOf(slot) { return Math.floor(slot / ROW_WIDTH); }

/**
 * A candidate set of slot indices is drawable as one graphic only when it is
 * contiguous AND sits within a single row — a graphic cannot span the header
 * gap between row 0 and row 1, and a non-contiguous set (roles scattered
 * across a page) cannot be drawn as one shape at all.
 */
/* `ignoreRows` is used by alignGroupsToRows to ask the counterfactual "what
 * would group if the row constraint were lifted?" — it is never used to DRAW,
 * because a shape cannot span the row-0 label band. */
let IGNORE_ROWS = false;
function isAdjacentRun(slots) {
    if (slots.length === 0) return false;
    const sorted = [...slots].sort((a, b) => a - b);
    if (!IGNORE_ROWS && rowOf(sorted[0]) !== rowOf(sorted[sorted.length - 1])) return false;
    for (let i = 1; i < sorted.length; i++) if (sorted[i] !== sorted[i - 1] + 1) return false;
    return true;
}

function span(slots) {
    const sorted = [...slots].sort((a, b) => a - b);
    return { slotStart: sorted[0], slotSpan: sorted[sorted.length - 1] - sorted[0] + 1 };
}

/**
 * Movy's `isGainRange` (v0.27.0, MIT): a genuine EQ band gain is bipolar and
 * roughly symmetric — a wrong guess is a crossover frequency, a Q, or some
 * other positive-only range that merely has "gain" in its name. Ported as the
 * corroboration check it is, not the code (no source is vendored here).
 */
export function isGainRange(meta) {
    if (!meta || typeof meta.min !== "number" || typeof meta.max !== "number") return false;
    const { min, max } = meta;
    if (!(min < 0 && max > 0)) return false;
    const lo = Math.abs(min), hi = Math.abs(max);
    const bigger = Math.max(lo, hi), smaller = Math.min(lo, hi);
    if (smaller === 0) return false;
    /* "roughly symmetric" — within a factor of 3, not exact mirror. Fleet
     * gains run -12..+12, -15..+18, -6..+6 etc; a crossover or Q is never
     * this shape. */
    return bigger / smaller <= 3;
}

const WAVEFORM_NAMES = /\b(sine|sin|tri|triangle|saw|sawtooth|square|pulse|ramp|noise|random|s\s?&\s?h|sample\s?&?\s?hold)\b/i;
/* Exported so knob_engine.mjs turns exactly the controls detectSwitch draws as a
 * switch — the feel and the picture must agree on what a boolean is. */
export const BOOL_OPTION = /^(off|on|no|yes|0|1|false|true|disabled|enabled)$/i;

/**
 * A boolean, however its author spelled it.
 *
 * Two spellings mean the same control. `enum` with Off/On options is the one
 * this file recognised; `int` with min 0 and max 1 is the one it did not, and
 * that is 61 parameters across 11 modules — obxd alone declares 25 of them
 * (unison, osc1_saw, osc1_pulse...), dexed 8, and it is how ambiotica spells
 * its Tempo Sync. All of them drew as a NUMBER, which is the one widget that
 * tells you nothing: "1" does not say what the other state is, or that there
 * are only two.
 *
 * Deliberately NOT float 0..1 — that is a mix or an amount, the single most
 * common continuous range in the fleet, and treating it as a switch would
 * collapse hundreds of real dials into on/off.
 *
 * A range of exactly 1 is required rather than `max <= 1`, so a 0..0
 * degenerate declaration is left alone rather than drawn as a switch that
 * cannot move.
 */
export function isBooleanMeta(meta) {
    if (!meta) return false;
    const opts = Array.isArray(meta.options) ? meta.options.map(String) : null;
    if (opts && opts.length === 2)
        return opts.every((o) => BOOL_OPTION.test(o.trim()));
    if (opts) return false;      /* an options list of any other length is a list */
    const isInt = meta.type === "int" || meta.kind === "int";
    return !!isInt && meta.min === 0 && meta.max === 1;
}

/**
 * Strip the matched role word out of a key, leaving whatever names the
 * SUBSYSTEM it belongs to ("chorus_lfo_shape" minus "shape" -> "chorus_lfo").
 * Adjacent role-name regex matches plus contiguous slots is not enough
 * corroboration on its own — a page can legitimately place a chorus LFO
 * shape next to a delay's rate and depth, which reads like an LFO group by
 * vocabulary alone. Requiring every role's remainder to agree is what tells
 * "one LFO, four roles" apart from "four unrelated knobs that happen to sit
 * next to each other".
 */
function stemOf(key, wordRegex) {
    return key.toLowerCase()
        .replace(wordRegex, "_")
        .replace(/_+/g, "_")
        .replace(/^_|_$/g, "");
}

/** True when every item's stem (already attached as `.stem`) agrees. */
/*
 * A UNIT QUALIFIER is not a different subsystem.
 *
 * A rate is routinely published twice, once per time base -- `lfo_rate_div` for
 * the tempo-synced division and `lfo_rate_hz` for the free-running one, with a
 * `lfo_sync` enum choosing between them. That is an ordinary LFO, and the
 * trailing token is a unit, not a name.
 *
 * Exact stem equality could not see that. schwung-filter's LFO page carries
 * lfo_amount (stem "lfo") beside lfo_rate_div (stem "lfo_div"), the stems
 * disagreed, and the graphic was refused -- on a page whose every key begins
 * "lfo_", which is as unambiguous as this gets.
 *
 * So a stem may differ by trailing tokens drawn from a CLOSED list of units and
 * time bases. Deliberately not "one stem is a prefix of the other": that would
 * make "lfo" agree with "lfo_2", merging two different LFOs, which is the very
 * thing the stem test exists to prevent. An unknown extra token still
 * disagrees.
 */
const STEM_QUALIFIERS = new Set([
    "div", "hz", "ms", "s", "sec", "secs", "bpm", "sync", "synced", "free",
    "note", "beats", "bars", "time", "khz",
]);

function stemsAgree(items) {
    if (items.length < 2) return true;
    const base = items[0].stem;
    return items.every((i) => {
        if (i.stem === base) return true;
        const a = String(base).split("_").filter(Boolean);
        const b = String(i.stem).split("_").filter(Boolean);
        const [shortT, longT] = a.length <= b.length ? [a, b] : [b, a];
        for (let n = 0; n < shortT.length; n++) if (shortT[n] !== longT[n]) return false;
        /* Everything the longer stem adds must be a unit, not a name. */
        return longT.slice(shortT.length).every((t) => STEM_QUALIFIERS.has(t));
    });
}

/* --------------------------------------------------------------- declared */

/**
 * Declared groups from `meta.viz` on each of the page's keys. `param_meta.js`
 * already folds a chain_params/inline `viz` field straight through
 * `normalize()`, so no separate chainParams argument is needed here — the
 * metaIndex already carries it.
 */
function collectDeclared(keys, metaIndex, invalid) {
    const groups = new Map();   /* group id -> { kind, roles: {role: {key, slot}} } */
    const singles = [];         /* declared single-param kinds: waveform/fader/switch/sample */
    const excluded = new Set(); /* viz: false */

    keys.forEach((key, slot) => {
        if (!key) return;
        const meta = metaIndex.getOrGuess(key);
        const v = meta && meta.viz;
        if (v === false) { excluded.add(key); return; }
        if (!v || typeof v !== "object") return;

        if (v.group) {
            if (!groups.has(v.group)) groups.set(v.group, { kind: v.kind || null, roles: {}, groupId: v.group });
            const g = groups.get(v.group);
            /*
             * `span: false` — a role that lends the graphic its VALUE without
             * joining the run of cells it covers.
             *
             * An LFO polarity is the case this exists for: whether the wave
             * swings about its baseline or sits on it is the single most
             * legible thing the picture can say, but the control belongs with
             * the other setup switches, not among the four cells the wave is
             * drawn across. Counting it in the span would make the group
             * straddle the row boundary and the graphic would vanish entirely —
             * adjacency is a hard gate.
             *
             * Such a role is also NOT claimed, so its own cell still draws as
             * an ordinary control. It informs the picture; it is not part of it.
             */
            if (v.role) g.roles[v.role] = { key, slot, span: v.span !== false };
            if (v.kind && !g.kind) g.kind = v.kind;
        } else if (v.kind) {
            /*
             * A CUSTOM KIND CLAIMS NOTHING UNLESS IT CAN BE DRAWN.
             *
             * Returning here leaves the key in the detector pool, so an unknown
             * or disabled widget degrades to the built-in one rather than to a
             * hole. See widget_registry.mjs -- this is the fall-through for a
             * typo, a failed load, an older host and a one-strike disable, all
             * on one path.
             *
             * IT BELONGS HERE AND NOT IN THE SHARED WALK ABOVE. A group's kind
             * may be declared on ANY member, so guarding before the v.group
             * branch would drop only that member: the remaining roles would
             * still form a group, inferKindFromRoles would name it, and an
             * `attack` declaring an unavailable custom kind would silently
             * become a THREE-cell envelope with its own key orphaned beside it.
             * The group case is handled below, where the whole group can be
             * abandoned at once.
             */
            if (isCustomKind(v.kind) && !isWidgetAvailable(v.kind)) return;
            singles.push({ kind: v.kind, key, slot, extraKeys: declaredExtraKeys(v) });
        }
    });

    const out = [];
    for (const g of groups.values()) {
        /* Only SPANNING roles decide where the graphic sits and how wide it is,
         * and only they are claimed. A span:false role is read for its value. */
        const spanning = Object.values(g.roles).filter((r) => r.span !== false);
        const slots = spanning.map((r) => r.slot);
        const kind = g.kind || inferKindFromRoles(Object.keys(g.roles));
        if (!kind) continue;
        /* Abandon the WHOLE group when its custom widget cannot be drawn, so
         * every one of its keys reaches the detector together and is grouped
         * as the built-in the author was decorating. Not recorded in `invalid`:
         * an unavailable custom kind is LEGAL -- it is exactly what an older
         * host sees reading a newer module -- and flagging it would make every
         * forward-compatible module look broken. */
        if (isCustomKind(kind) && !isWidgetAvailable(kind)) continue;
        if (!slots.length) {
            invalid.push({ group: g.groupId, kind, reason: "no spanning roles" });
            continue;
        }
        if (!isAdjacentRun(slots)) {
            invalid.push({ group: g.groupId, kind, reason: "roles not adjacent on one row" });
            continue;
        }
        const built = {
            kind, group: g.groupId, roles: mapRoles(g.roles),
            keys: spanning.map((r) => r.key),
            ...span(slots), source: VIZ_SOURCE_DECLARED,
        };
        /* A group's kind may be declared on any member, so its extra keys may
         * be too — take the first member that names some. */
        for (const r of Object.values(g.roles)) {
            const ek = declaredExtraKeys(r.viz);
            if (ek) { built.extraKeys = ek; break; }
        }
        out.push(built);
    }
    for (const s of singles) {
        const g2 = {
            kind: s.kind, group: null, roles: { value: s.key }, keys: [s.key],
            slotStart: s.slot, slotSpan: 1, source: VIZ_SOURCE_DECLARED,
        };
        if (s.extraKeys && s.extraKeys.length) g2.extraKeys = s.extraKeys;
        out.push(g2);
    }
    return { groups: out, excluded };
}

/*
 * Off-page keys a MODULE says its widget needs.
 *
 * `extraKeys` already existed for exactly this shape, but only detectSample
 * could set it, for an off-page filepath. The general problem is the same one:
 * a widget's picture can depend on a value that has no cell on this page. Keys
 * claim cells; a key with no cell cannot be a key; so it rides here and the
 * controller adds it to the value rotation as one extra stop.
 *
 * Without it the only way to get a fact to a widget was to give it a knob --
 * which is how a module ended up with a read-only cell existing purely to carry
 * a value to the cell beside it, occupying one of eight slots on a 128x64
 * screen to draw something nobody needed to see.
 *
 * ONE READ PER STOP, so this is not free: declare what the picture needs and
 * nothing else. Capped, because a module asking for twenty keys would spend the
 * page's whole read budget on one cell and starve every other value on screen.
 */
const MAX_DECLARED_EXTRA_KEYS = 4;

function declaredExtraKeys(v) {
    const raw = v && (v.extra_keys || v.extraKeys);
    if (!Array.isArray(raw)) return null;
    const out = [];
    for (const k of raw) {
        if (typeof k !== "string" || !k) continue;
        if (out.indexOf(k) < 0) out.push(k);
        if (out.length >= MAX_DECLARED_EXTRA_KEYS) break;
    }
    return out.length ? out : null;
}

function mapRoles(roleMap) {
    const out = {};
    for (const [role, { key }] of Object.entries(roleMap)) out[role] = key;
    return out;
}

function inferKindFromRoles(roles) {
    const set = new Set(roles);
    if (ENVELOPE_ROLE_ORDER.some((r) => set.has(r))) return VIZ_ENVELOPE;
    if (set.has("cutoff") || set.has("resonance")) return VIZ_FILTER;
    if (set.has("rate") && set.has("depth")) return VIZ_LFO;
    if (EQ_ROLE_ORDER.some((r) => set.has(r))) return VIZ_EQ;
    return null;
}

/* -------------------------------------------------------------- detectors */

/**
 * Each detector receives the pool of still-unclaimed (slot, key, meta)
 * triples for one page, already sorted by slot, and returns zero or more
 * groups. Every candidate must pass a metadata check, not just a name match —
 * "corroborate with declared metadata, not vocabulary" is the rule the whole
 * detector layer exists to keep.
 */

const ROLE_WORD = {
    attack: /attack/, decay: /decay/, sustain: /sustain/, release: /release/,
    /*
     * HOLD is the one role that needs a boundary. The others are bare
     * substrings and can afford to be, but "threshold" ENDS IN "hold" -- and
     * gate declares `threshold` on knob 1 and `hold` on knob 3, so a bare
     * /hold/ would bind the hold role to the threshold, which is the first
     * match in the pool. The group would then be built out of the wrong knob
     * and drawn as a plateau whose height is a dB threshold.
     *
     * (^|_) matches `hold`, `gate_hold`, `lfo0_hold`; it does not match
     * `threshold`.
     */
    hold: /(^|_)hold/,
};

function detectEnvelope(pool) {
    const byRole = {};
    for (const item of pool) {
        const k = item.key.toLowerCase();
        if (!byRole.attack && ROLE_WORD.attack.test(k)) byRole.attack = { ...item, stem: stemOf(k, ROLE_WORD.attack) };
        else if (!byRole.decay && ROLE_WORD.decay.test(k)) byRole.decay = { ...item, stem: stemOf(k, ROLE_WORD.decay) };
        else if (!byRole.sustain && ROLE_WORD.sustain.test(k)) byRole.sustain = { ...item, stem: stemOf(k, ROLE_WORD.sustain) };
        else if (!byRole.release && ROLE_WORD.release.test(k)) byRole.release = { ...item, stem: stemOf(k, ROLE_WORD.release) };
        /*
         * Hold is accepted only if it is a NUMBER. Every other role is
         * numeric-checked after the fact, and failing that check rejects the
         * WHOLE envelope -- so a non-numeric hold would not merely be skipped,
         * it would delete an otherwise perfect ADSR. "arp_hold" (chordism,
         * osirus) is a switch, and it sits on pages that have real envelopes
         * on them.
         */
        else if (!byRole.hold && ROLE_WORD.hold.test(k) && isNumeric(item.meta)) byRole.hold = { ...item, stem: stemOf(k, ROLE_WORD.hold) };
    }
    let present = ENVELOPE_ROLE_ORDER.filter((r) => byRole[r]);
    if (present.length < 2) return [];
    /* Every role param must be a turnable number — an enum called "attack"
     * would not be a time or level. */
    if (!present.every((r) => isNumeric(byRole[r].meta))) return [];
    /* "f_attack"/"f_decay" belong together; "amp_attack"/"filter_decay" do
     * not, whatever the adjacency looks like. */
    if (!stemsAgree(present.map((r) => byRole[r]))) return [];

    /*
     * Take the longest ADJACENT RUN of roles, rather than requiring every role
     * found on the page to be adjacent.
     *
     * Requiring all of them means one stray role deletes a group that is
     * otherwise perfect. linein is the case: its Gate Settings page declares
     * threshold/attack/release/range on knobs, and `gate_hold` is undeclared,
     * so the planner appends it at the END. Slots [1, 4, 2] are not a run, and
     * an attack/release pair sitting side by side stopped being an envelope
     * because of a knob four positions away.
     *
     * Same shape as the optional-role bug in detectFilter, and worth fixing as
     * the general rule rather than as another special case: what corroborates
     * a group is roles that are TOGETHER, so the answer is to find them, not
     * to give up because something else also matched.
     */
    const bySlot = present.slice().sort((a, b) => byRole[a].slot - byRole[b].slot);
    let best = [], run = [];
    for (const r of bySlot) {
        if (run.length && byRole[r].slot !== byRole[run[run.length - 1]].slot + 1) run = [];
        run.push(r);
        if (run.length > best.length) best = run.slice();
    }
    if (best.length < 2) return [];
    /* Back to time order for the roles map and the draw. */
    present = ENVELOPE_ROLE_ORDER.filter((r) => best.includes(r));
    const slots = present.map((r) => byRole[r].slot);
    if (!isAdjacentRun(slots)) return [];
    return [{
        kind: VIZ_ENVELOPE, group: null,
        roles: Object.fromEntries(present.map((r) => [r, byRole[r].key])),
        keys: present.map((r) => byRole[r].key),
        ...span(slots), source: VIZ_SOURCE_DETECTED,
    }];
}

/* `_` is a \w character, so `\bword\b` never matches inside an
 * underscore-joined key ("lfo_rate" has no boundary before "rate"). Every
 * role-word regex here anchors on `(^|_)…($|_)` instead. */
const FILTER_WORD = {
    cutoff: /cutoff|cutof|frequency|freq/,
    resonance: /resonance|reso|(^|_)res($|_)|(^|_)q($|_)/,
    mode: /(^|_)(mode|type)($|_)/,
    slope: /(^|_)(slope|poles?)($|_)/,
};

function detectFilter(pool) {
    let cutoff = null, resonance = null, mode = null, slope = null;
    for (const item of pool) {
        const k = item.key.toLowerCase();
        if (!cutoff && FILTER_WORD.cutoff.test(k)) cutoff = { ...item, stem: stemOf(k, FILTER_WORD.cutoff) };
        else if (!resonance && FILTER_WORD.resonance.test(k)) resonance = { ...item, stem: stemOf(k, FILTER_WORD.resonance) };
        else if (!mode && FILTER_WORD.mode.test(k) && isEnum(item.meta)) mode = { ...item, stem: stemOf(k, FILTER_WORD.mode) };
        else if (!slope && FILTER_WORD.slope.test(k)) slope = { ...item, stem: stemOf(k, FILTER_WORD.slope) };
    }
    if (!cutoff || !resonance) return [];
    if (!isNumeric(cutoff.meta) || !isNumeric(resonance.meta)) return [];
    /* Cutoff and resonance sharing no stem ("hp_cutoff" next to a totally
     * unrelated "eq_resonance") is not a filter, just two knobs that landed
     * beside each other. */
    if (!stemsAgree([cutoff, resonance])) return [];

    const roles = { cutoff: cutoff.key, resonance: resonance.key };
    const items = [cutoff, resonance];
    if (!isAdjacentRun(items.map((i) => i.slot))) return [];

    /*
     * mode and slope are OPTIONAL, so a non-adjacent one is DROPPED -- it must
     * not disqualify the pair that does corroborate.
     *
     * It used to. schwung-filter puts cutoff and resonance on knobs 1 and 2
     * and its Mode enum on knob 8, so the slot run was [0, 1, 7], the
     * adjacency check failed, and the module whose entire purpose is a filter
     * drew two unrelated dials. 303, whose root page has no mode key at all,
     * grouped correctly off the identical cutoff/resonance pair -- so the
     * failure looked like something specific to schwung-filter rather than
     * what it was: an optional role behaving like a required one.
     *
     * Added one at a time in slot order, keeping each only while the run stays
     * contiguous, so a mode that IS adjacent still joins the group. */
    const optional = [];
    if (mode) optional.push(["mode", mode]);
    if (slope && (isNumeric(slope.meta) || isEnum(slope.meta))) optional.push(["slope", slope]);
    optional.sort((a, b) => a[1].slot - b[1].slot);
    for (const [role, item] of optional) {
        const widened = items.concat([item]);
        if (!isAdjacentRun(widened.map((i) => i.slot))) continue;
        /* No stem check on the optionals. It was tried and it is wrong here:
         * noisemaker names its pair bare ("cutoff", "resonance") and its mode
         * "filter_type", so the stems disagree and the mode was dropped from a
         * group it plainly belongs to. Adjacency is the corroboration -- a
         * mode sitting directly beside a cutoff/resonance pair is that pair's
         * mode. */
        roles[role] = item.key;
        items.push(item);
    }
    items.sort((a, b) => a.slot - b.slot);
    const slots = items.map((i) => i.slot);
    return [{
        kind: VIZ_FILTER, group: null, roles, keys: items.map((i) => i.key),
        ...span(slots), source: VIZ_SOURCE_DETECTED,
    }];
}

/*
 * `spd` is in here because a fleet module used it and lost its whole graphic.
 *
 * schwung-work names four LFOs vlfo1_spd / vlfo2_spd / flfo1_spd / flfo2_spd,
 * with depth, wave and phase all spelled in full beside them. No rate meant no
 * group, so four consecutive Modulation pages drew seven separate dials each
 * where an LFO belonged -- and nothing reported it, because a missing graphic
 * looks exactly like a module that never wanted one.
 *
 * Abbreviations are the rule in this vocabulary, not the exception: the cells
 * are five characters wide, so authors abbreviate at the source. `amt` is
 * already here for the same reason.
 */
/*
 * A DIGIT IS A BOUNDARY TOO, and an index may trail the role word.
 *
 * The boundary was underscore-or-end, which two common naming styles never
 * satisfy. minijv writes `nvram_tone_0_lfo1rate` -- the instance number runs
 * straight into the role with no separator -- and that alone is 20+ pages
 * drawing no LFO. obxd writes `lfo_amt1`, where the index trails the role.
 *
 * Allowing a DIGIT either side fixes both without opening the door to
 * substring matches: "generate" still does not match `rate`, because the
 * character before it is a letter.
 *
 * `magnitude` is surge's word for depth (lfo0_magnitude), `amplitude` is helm's
 * (mono_lfo_1_amplitude), and pmd/amd are dexed's -- pitch and amplitude mod
 * depth, the DX7's own names. Deliberately not bare `amp`, which is an
 * amplifier far more often than it is an LFO depth.
 */
const LFO_WORD = {
    shape: /shape|waveform|wave/,
    rate: /(^|_|\d)(rate|speed|spd|frequency|freq)(\d*)($|_)/,
    depth: /(^|_|\d)(depth|amount|amt|amplitude|magnitude|pmd|amd)(\d*)($|_)/,
    phase: /(^|_|\d)phase(\d*)($|_)/,
};

function detectLfo(pool) {
    let shape = null, depth = null, phase = null;
    /*
     * ONE LFO, TWO RATES -- take the one that can be drawn.
     *
     * A tempo-syncable LFO publishes its rate twice: `lfo_rate_div` as an enum
     * of musical divisions, `lfo_rate_hz` as a float, with a sync switch
     * choosing between them. Both match the rate vocabulary, and taking the
     * first match took the ENUM -- which is not numeric, so the group was
     * rejected two lines later while a perfectly good numeric rate sat in the
     * next slot. schwung-filter's whole LFO page drew as five loose dials
     * because of it.
     *
     * So rate is a CANDIDATE LIST, and the numeric ones are preferred. The
     * division enum is still the better thing to show in the cell itself; it
     * just cannot be the value a rate LINE is drawn from.
     */
    const rateCandidates = [];
    for (const item of pool) {
        const k = item.key.toLowerCase();
        if (!shape && LFO_WORD.shape.test(k) && isEnum(item.meta) &&
            (item.meta.options || []).some((o) => WAVEFORM_NAMES.test(String(o)))) shape = { ...item, stem: stemOf(k, LFO_WORD.shape) };
        else if (LFO_WORD.rate.test(k)) rateCandidates.push({ ...item, stem: stemOf(k, LFO_WORD.rate) });
        else if (!depth && LFO_WORD.depth.test(k)) depth = { ...item, stem: stemOf(k, LFO_WORD.depth) };
        else if (!phase && LFO_WORD.phase.test(k)) phase = { ...item, stem: stemOf(k, LFO_WORD.phase) };
    }
    /* Prefer a numeric rate; fall back to the first candidate so the failure
     * below still reports "not numeric" rather than "no rate at all". */
    const rate = rateCandidates.find((c) => isNumeric(c.meta)) || rateCandidates[0] || null;
    if (!rate || !depth) return [];
    if (!isNumeric(rate.meta) || !isNumeric(depth.meta)) return [];
    /* "delay_rate" next to "chorus_depth" reads like an LFO by vocabulary
     * alone; requiring the same stem is what tells one LFO's rate+depth
     * apart from two different subsystems' knobs that happen to sit next to
     * each other on the page. */
    if (!stemsAgree([rate, depth])) return [];
    /* Also needs the LFO's own name in the neighbourhood (shape present, or
     * "lfo" literally in rate/depth's key) — otherwise "rate" + "depth" alone
     * is indistinguishable from an envelope follower or any other modulator. */
    const hasLfoContext = !!shape || /lfo/.test(rate.key.toLowerCase()) || /lfo/.test(depth.key.toLowerCase());
    if (!hasLfoContext) return [];
    const roles = { rate: rate.key, depth: depth.key };
    const items = [rate, depth];
    /* stemsAgree, not ===, for the same reason the rate/depth test uses it: a
     * shape named lfo_shape (stem "lfo") belongs to a rate named lfo_rate_hz
     * (stem "lfo_hz"). Exact equality dropped the waveform out of its own LFO
     * whenever the rate carried a unit. */
    if (shape && stemsAgree([shape, rate])) { roles.shape = shape.key; items.push(shape); }
    if (phase && isNumeric(phase.meta) && stemOf(phase.key.toLowerCase(), LFO_WORD.phase) === rate.stem) {
        roles.phase = phase.key; items.push(phase);
    }
    items.sort((a, b) => a.slot - b.slot);
    const slots = items.map((i) => i.slot);
    if (!isAdjacentRun(slots)) return [];
    return [{
        kind: VIZ_LFO, group: null, roles, keys: items.map((i) => i.key),
        ...span(slots), source: VIZ_SOURCE_DETECTED,
    }];
}

function detectWaveform(pool) {
    const out = [];
    for (const item of pool) {
        const k = item.key.toLowerCase();
        if (!isEnum(item.meta)) continue;
        if (!/(wave|shape|osc.*type|osc.*wave)/.test(k)) continue;
        const opts = item.meta.options || [];
        const matches = opts.filter((o) => WAVEFORM_NAMES.test(String(o))).length;
        if (matches < 2) continue;   /* one match could be coincidence ("Ramp" mode) */
        out.push({
            kind: VIZ_WAVEFORM, group: null, roles: { value: item.key }, keys: [item.key],
            slotStart: item.slot, slotSpan: 1, source: VIZ_SOURCE_DETECTED,
        });
    }
    return out;
}

/* A band-gain qualifier — reserved for the EQ detector so a lone "gain" does
 * not get claimed by the fader detector before three of them can be seen
 * together. Tokenised on "_" rather than \b, which does not see a word
 * boundary either side of an underscore. */
const EQ_BAND_TOKENS = new Set(["low", "lo", "mid", "midrange", "high", "hi", "band", "treble", "bass"]);
function isEqBandIsh(key) {
    const tokens = key.split(/[_\-]+/);
    return tokens.includes("gain") && tokens.some((t) => EQ_BAND_TOKENS.has(t));
}

/**
 * A dB LEVEL: negative because silence is, not because it is bipolar.
 *
 * The fader detector rejects anything with a negative minimum, on the reasoning
 * that a bipolar range is a pan or a trim rather than a volume. That is right
 * for pushnpull's `volume` (-1..1, unit %, which is a balance) and it is exactly
 * wrong for a dB fader, where the whole scale runs from a silence floor up to
 * some headroom -- the most fader-shaped control there is. usefulity's Gain
 * (-100..35) and ottx's in/out gain (-60..30 dB) drew as plain arc knobs
 * because of it.
 *
 * Three conditions, and the second and third are what keep this from swallowing
 * the trims:
 *
 *   dB-scaled     declared `unit: "dB"`, or a `_db` suffix for the modules that
 *                 put the unit in the name instead. A % or a raw ratio is not
 *                 a dB level however it is named.
 *   deep floor    min <= -30. A silence floor, not a cut. 4k-eq's band gains
 *                 are dB and bipolar at +-15, and they are trims around unity;
 *                 they should form an EQ, not four separate faders.
 *   asymmetric    max < -min. A level has far more attenuation than boost. A
 *                 symmetric +-48 is a trim no matter how deep it goes, which
 *                 is what keeps surge's level_pfg out.
 *
 * Measured on the 100-module fleet: 14 params are named like a fader and
 * rejected for being negative. This admits exactly 3 of them -- usefulity's
 * gain_db and ottx's two -- and leaves the 11 trims, band gains and the one
 * balance alone.
 */
export function isDbLevel(meta, key) {
    if (!meta || typeof meta.min !== "number" || typeof meta.max !== "number") return false;
    const inDb = String(meta.unit || "").toLowerCase() === "db" || /_db($|_)/.test(String(key || ""));
    if (!inDb) return false;
    if (meta.min > -30) return false;
    return meta.max < -meta.min;
}

function detectFader(pool) {
    const out = [];
    for (const item of pool) {
        const k = item.key.toLowerCase();
        if (!isNumeric(item.meta)) continue;
        if (isEqBandIsh(k)) continue;
        /*
         * `output` and `input` count only as the WHOLE key.
         *
         * tapescam declares its trim as plain `output`, float 0..1, and drew an
         * arc -- the vocabulary below had no word for it. But the containment
         * test that would fix it also catches magneto's `input_pan`, which is a
         * PAN: bipolar in meaning, and a fader would be a lie about it. So the
         * bare word qualifies and a compound does not; a compound that really
         * is a level almost always says so (`output_level`, `input_gain`) and
         * matches on that instead.
         */
        /*
         * A module's own IN and OUT are levels whatever their polarity.
         *
         * Matched on the key OR the declared name, because the name is
         * sometimes the only place it is said: schwung-airwindows keys every
         * control `param_N` and names this one "Output", and superboom spells
         * its key `inputGain` in camelCase.
         *
         * And bipolar does not disqualify them, which is the one place this
         * departs from the trim rule below. 4k-eq's IN and OUT are +-12 dB and
         * pushnpull's output is -24..+12: those are the signal path's level,
         * and a fader is the honest picture of one. An EQ BAND gain at +-15 is
         * a different thing and still takes the arc, because `lf_gain` is not
         * a bare in or out.
         */
        const nameBare = /^(output|input|out|in)$/i.test(String(item.meta.name || "").replace(/[\s.]/g, ""));
        const bareIO = /^(output|input|out|in)$/.test(k) || nameBare;
        if (!bareIO && !/(^|_)(gain|volume|vol|level|amp)($|_)/.test(k)) continue;
        /* A fader is a unipolar level; a bipolar range is a pan or a trim,
         * not a volume, whatever the name says -- EXCEPT in dB, where a level
         * is negative by nature and the fader is the canonical picture of it.
         * See isDbLevel. */
        if (!bareIO && typeof item.meta.min === "number" && item.meta.min < 0 &&
            !isDbLevel(item.meta, k)) continue;
        out.push({
            kind: VIZ_FADER, group: null, roles: { value: item.key }, keys: [item.key],
            slotStart: item.slot, slotSpan: 1, source: VIZ_SOURCE_DETECTED,
        });
    }
    return out;
}

function detectSwitch(pool) {
    const out = [];
    for (const item of pool) {
        if (!isBooleanMeta(item.meta)) continue;
        /*
         * A TRIGGER IS NOT A SWITCH, however boolean it looks.
         *
         * A write-only param declares that writing DOES something and the value
         * means nothing. Palette spells its five randomisers as enums of
         * ["0","1"] and Spectra spells its four as int 0..1, so both satisfy
         * isBooleanMeta exactly -- and a switch graphic drew over them, showing
         * a latched on/off position for a value the module never reports.
         *
         * drawKnobWidget already gets this right: widgetKindFor puts writeOnly
         * ahead of the enum branch and returns a button. But A CELL A VIZ GROUP
         * COVERS NEVER REACHES drawKnobWidget, so the graphic silently overruled
         * the widget. Declaring access:"write" then appeared to do nothing at
         * all -- the module was correct, the widget rule was correct, and the
         * screen still showed a switch.
         */
        if (isTrigger(item.meta)) continue;
        out.push({
            kind: VIZ_SWITCH, group: null, roles: { value: item.key }, keys: [item.key],
            slotStart: item.slot, slotSpan: 1, source: VIZ_SOURCE_DETECTED,
        });
    }
    return out;
}

/*
 * The band words require a separator, which is what keeps "lowpass" and
 * "highpass" out of the EQ detector. The `[lmh]gain` forms are the exception
 * that has to be spelled out: ottx declares lgain / mgain / hgain, adjacent
 * and all -30..30 dB — a textbook three-band EQ that matched none of the
 * patterns because there is no separator to anchor on.
 *
 * Anchored whole-string and restricted to the single letter plus "gain", so it
 * cannot reach for "lfo_gain" or "make_gain".
 */
const EQ_BAND_WORD = {
    low: /(^|_)(low|lo|bass)($|_)|^lgain$/,
    mid: /(^|_)mid($|_)|^mgain$/,
    high: /(^|_)(high|hi|treble)($|_)|^hgain$/,
};

function detectEq(pool) {
    let low = null, mid = null, high = null;
    for (const item of pool) {
        const k = item.key.toLowerCase();
        if (!/gain/.test(k) && !EQ_BAND_WORD.low.test(k) && !EQ_BAND_WORD.mid.test(k) && !EQ_BAND_WORD.high.test(k)) continue;
        if (!isGainRange(item.meta)) continue;
        if (!low && EQ_BAND_WORD.low.test(k)) low = { ...item, stem: stemOf(k, EQ_BAND_WORD.low) };
        else if (!mid && EQ_BAND_WORD.mid.test(k)) mid = { ...item, stem: stemOf(k, EQ_BAND_WORD.mid) };
        else if (!high && EQ_BAND_WORD.high.test(k)) high = { ...item, stem: stemOf(k, EQ_BAND_WORD.high) };
    }
    const present = [["low", low], ["mid", mid], ["high", high]].filter(([, v]) => v);
    if (present.length < 2) return [];
    /* "low_gain" / "mid_gain" / "high_gain" all reduce to the stem "gain";
     * "lo_boost" beside an unrelated "mid_pan" would not, and is rejected. */
    if (!stemsAgree(present.map(([, v]) => v))) return [];
    const roles = Object.fromEntries(present.map(([r, v]) => [r, v.key]));
    const items = present.map(([, v]) => v).sort((a, b) => a.slot - b.slot);
    const slots = items.map((i) => i.slot);
    if (!isAdjacentRun(slots)) return [];
    return [{
        kind: VIZ_EQ, group: null, roles, keys: items.map((i) => i.key),
        ...span(slots), source: VIZ_SOURCE_DETECTED,
    }];
}

/*
 * Ported from schwung-movy src/model/wav-viz.ts detectWavViz, with permission.
 *
 * ANCHORED ON THE MARKER, NOT THE FILE. The first version required a filepath
 * param on the same page, so a page of nothing but Start / Loop Start / Loop
 * End — the page that needs the picture MOST, because three separate knobs
 * cannot show that a loop sits inside the region that plays — drew nothing at
 * all. The marker is what indexes into a sample, so the marker is the anchor.
 */

/** The file a marker says it indexes, or null. */
function markerFileKey(meta) {
    return (meta && (meta.filepath_param || meta.filepathParam)) || null;
}

/*
 * A marker is either TYPED `wav_position`, or a plain number that NAMES the
 * file it indexes. mrsample types its Start and Loop Start as floats and
 * declares `filepath_param: sample_path`; that declaration is the module
 * telling us the knob is a position into that sample, and it is a stronger
 * signal than a type string it never set.
 */
function isMarkerMeta(meta) {
    if (!meta) return false;
    if (meta.type === "wav_position") return true;
    if (meta.type !== "float" && meta.type !== "int") return false;
    return !!markerFileKey(meta);
}

/*
 * Loop bounds draw as BRACKETS rather than as a cursor, so they have to be
 * told apart from the playback position — by name, like everything else here
 * infers. "to" is anchored because it is two letters and would otherwise match
 * inside any word ("automation", "photo").
 */
const MARKER_LOOP_WORD = /loop/;
const MARKER_END_WORD = /end|stop|finish|(^|[\s_])to($|[\s_])/;
function markerKind(key, meta) {
    const t = (String(key) + " " + String((meta && meta.name) || "")).toLowerCase();
    if (!MARKER_LOOP_WORD.test(t)) return "position";
    return MARKER_END_WORD.test(t) ? "loopEnd" : "loopStart";
}

/*
 * The granular read spread, matched on the EXACT key.
 *
 * That narrowness is the design, not an oversight. "Spread", "Scatter" and
 * "Diffuse" are all over the fleet and not one of them is a read-position
 * spread: granny's OWN `spread` is stereo width between voices,
 * fizzik/nusaw/freak spread is stereo, chordism's is chord voicing,
 * cloudseed's diffusion is a reverb control. Matching any of them would draw a
 * region on the sample that the DSP never reads a grain from.
 *
 * EXPORTED because the fullscreen wav_position editor draws the same fences
 * at its own scale and has to find the same parameter. A second predicate
 * there would be a second answer to "what is a spray", and the two would
 * disagree the first time either is widened -- the grid would fence a control
 * the editor ignored, or vice versa, on the same screen the user just came
 * from.
 */
export function isSprayMeta(key, meta) {
    return String(key).toLowerCase() === "spray"
        && !!meta && meta.type === "float" && meta.min === 0 && meta.max === 1;
}

function detectSample(pool, metaIndex) {
    /* Prefer a playback cursor as the anchor; fall back to any marker, so an
     * all-loop page still gets the graphic. */
    let anchor = null;
    for (const item of pool) {
        if (!isMarkerMeta(item.meta)) continue;
        if (markerKind(item.key, item.meta) === "position") { anchor = item; break; }
    }
    if (!anchor) {
        for (const item of pool) {
            if (isMarkerMeta(item.meta)) { anchor = item; break; }
        }
    }
    /*
     * NO MARKER, BUT A FILE: draw the waveform alone.
     *
     * Deferred from the marker-anchoring change, deliberately. While the
     * envelope was SYNTHETIC this cell was a fabricated picture of a file --
     * it looked like the sample's shape and was not one -- so breakbeat,
     * gesture-test and mrdrums' Pad Settings were better off showing their
     * filename. Now that the peaks are real, a waveform with no cursor is
     * genuine information about what is loaded, so they get it back.
     */
    if (!anchor) {
        /* EVERY file, not the first: breakbeat loads two samples side by side
         * (A_sample_path and B_sample_path) and each is its own picture. */
        const out = [];
        for (const item of pool) {
            if (item.meta.type !== "filepath" && item.meta.type !== "file") continue;
            out.push({
                kind: VIZ_SAMPLE, group: null, roles: { value: item.key },
                keys: [item.key], slotStart: item.slot, slotSpan: 1,
                source: VIZ_SOURCE_DETECTED,
            });
        }
        return out;
    }

    /*
     * Prefer the module's OWN declaration of which file this marker indexes
     * over "the first file param on the page" — a page holding both a preset
     * path and a sample path would otherwise be a coin toss.
     */
    let fileItem = null;
    const declaredFile = markerFileKey(anchor.meta);
    for (const item of pool) {
        if (declaredFile) { if (item.key === declaredFile) { fileItem = item; break; } }
        else if (item.meta.type === "filepath" || item.meta.type === "file") { fileItem = item; break; }
    }

    /*
     * THE FILE NEED NOT BE ON THE PAGE. Searching only the pool was the whole
     * granny bug: `sample_path` is declared, is type filepath, and is on NO
     * knobs list — it is reached through the hierarchy — so every page that
     * carries `position` found no file and drew a sample it could not name.
     *
     * A file that is off-page is still the file this marker indexes, so it
     * informs the picture. It does NOT join `keys`: keys claim cells, and a
     * key that is not on the page has no cell to claim. It rides in
     * `extraKeys` instead, which the controller adds to the value rotation as
     * one extra stop — the same idiom the preset-name read already uses.
     */
    let offPageFile = null;
    if (!fileItem && metaIndex && Array.isArray(metaIndex.keys)) {
        for (const k of metaIndex.keys) {
            if (declaredFile) { if (k === declaredFile) { offPageFile = k; break; } continue; }
            const m = metaIndex.getOrGuess(k);
            if (m && (m.type === "filepath" || m.type === "file")) { offPageFile = k; break; }
        }
    }

    /*
     * Every other marker on the SAME sample joins the graphic — by the
     * module's own `view_group` when it declares one, otherwise by naming the
     * same file. They belong on one picture: three separate knobs cannot show
     * that a loop sits inside the region that plays.
     */
    const group = anchor.meta.view_group || anchor.meta.viewGroup || null;
    const fileKey = fileItem ? fileItem.key : (declaredFile || offPageFile);
    const members = [anchor];
    for (const item of pool) {
        if (item === anchor || !isMarkerMeta(item.meta)) continue;
        const itemGroup = item.meta.view_group || item.meta.viewGroup || null;
        const sameGroup = !!group && itemGroup === group;
        const sameFile = !!fileKey && markerFileKey(item.meta) === fileKey;
        if (sameGroup || sameFile) members.push(item);
    }
    /* Only a PLAYBACK cursor has a spread — a loop bound does not. */
    let sprayItem = null;
    if (markerKind(anchor.key, anchor.meta) === "position") {
        for (const item of pool) {
            if (item !== anchor && isSprayMeta(item.key, item.meta)) { sprayItem = item; break; }
        }
    }

    /*
     * ROLES COME FROM EVERY MEMBER; ONLY THE ADJACENT RUN CLAIMS CELLS.
     *
     * These are two different questions and collapsing them broke both real
     * modules on the fleet. A graphic must be CONTIGUOUS — it cannot be drawn
     * across a foreign cell or across the row-0 label band — but what it can
     * SHOW is not limited that way: a loop bound or a spread is read out of the
     * value, not out of the neighbouring pixels.
     *
     * mrsample is the case for the bounds: it declares view_group "loop" on
     * sample_start, loop_start and loop_end, exactly as intended — and puts
     * `loop_mode` between them. Trimming the roles to the run left the flagship
     * loop-bracket module with no brackets.
     *
     * granny is the case for the spray, and worse: `spray` sits three knobs
     * from `position`, so the fences would never have drawn on the ONE module
     * in the fleet that has a spray at all.
     *
     * A member outside the run keeps its own knob and also informs the picture.
     * That redundancy is fine, and better than the alternatives: you can still
     * turn the knob and read its number, and the graphic still tells you where
     * the region sits.
     */
    const roles = {};
    if (fileItem) roles.value = fileItem.key;
    else if (offPageFile) roles.value = offPageFile;
    if (sprayItem) roles.spray = sprayItem.key;
    for (const it of members) {
        const kind = markerKind(it.key, it.meta);
        if (!roles[kind]) roles[kind] = it.key;   /* first one wins per role */
    }

    /*
     * The CELLS: the longest adjacent run containing the anchor, the same rule
     * detectEnvelope uses.
     *
     * THE SPRAY IS A CANDIDATE, which it did not used to be. The old reasoning
     * — it modifies the cursor rather than being a position, so it has nothing
     * to draw in a cell of its own — described the parameter correctly and the
     * LAYOUT wrongly. It drew spray's fences onto a cell belonging to
     * `position` while spray itself kept an unrelated-looking arc three knobs
     * away, which is what it looks like from the device: "spray is unrelated
     * but there".
     *
     * Being a candidate costs nothing where the spray is not already adjacent
     * to the cursor — the run rule still gives span 1, as it did for granny
     * before the gather pass. It is what lets `gatherGroupMembers` widen the
     * graphic once the two are seated together, exactly like the four knobs of
     * an ADSR sharing one envelope.
     *
     * THE FILE IS NOT A CANDIDATE, though it is still `roles.value` — the
     * picture is drawn FROM it, not ON it.
     *
     * It used to claim a cell, on the same "give the picture width" reasoning
     * that admits the spray, and the two are not alike. Everything else in
     * this graphic is a position WITHIN the sample and dives to the waveform
     * editor; the filepath is how you REPLACE the sample and dives to the file
     * browser. Claiming it drew one continuous waveform across a boundary
     * where the behaviour changes — reported from the device twice in a row,
     * *"sample file isn't part of the continuum because it goes to a different
     * editor"* and then *"why is there a line that spans between them?"*
     *
     * Released, the cell draws as the ordinary opaque box: notched frame,
     * chevron, and THE FILENAME — which is information the graphic was
     * throwing away. That is also the real answer to the report this whole
     * thread started from ("empty sample selection is indistinguishable from
     * the spray control"): the honest fix was never to bracket the cell, it
     * was to stop swallowing it.
     *
     * `roles.value` is untouched, so the waveform still comes from this file,
     * and the value is still read: the page cursor walks page.keys, not
     * group.keys. `extraKeys` remains for an OFF-page file, which is the case
     * it was built for.
     *
     * Unrelated to the no-anchor branch above, where a filepath with no marker
     * anywhere near it draws its own waveform in its own cell. That is one
     * cell, one door, and no boundary to cross.
     */
    const claimable = [sprayItem, ...members].filter(Boolean)
        .slice().sort((a, b) => a.slot - b.slot);
    let run = [], best = [];
    for (const it of claimable) {
        if (run.length && !isAdjacentRun([...run.map((r) => r.slot), it.slot])) run = [];
        run.push(it);
        if (run.indexOf(anchor) >= 0 && run.length > best.length) best = run.slice();
    }
    if (best.indexOf(anchor) < 0) best = [anchor];

    const g = {
        kind: VIZ_SAMPLE, group: null, roles, keys: best.map((it) => it.key),
        ...span(best.map((it) => it.slot)), source: VIZ_SOURCE_DETECTED,
    };
    /* Off-page only. An on-page file is already in the rotation via keys, and
     * asking for it twice would spend a read per frame to learn nothing. */
    if (offPageFile) g.extraKeys = [offPageFile];
    return [g];
}

/* Priority order — see the module doc comment. Each function returns the
 * groups it fires; every key any of them returns is removed from the pool
 * before the next detector runs. */
const DETECTORS = [
    detectEnvelope, detectFilter, detectLfo, detectWaveform,
    detectFader, detectSwitch, detectEq,
    detectSample,
];

/**
 * THE PICTURE IS THE DOOR: what a cell dives into when it has no door of its own.
 *
 * granny draws `position`, `spray` and `sample_path` as ONE waveform strip.
 * Two of those cells open something — the wave editor and the file browser —
 * and `spray` opened nothing, because it is a plain float and nothing is
 * declared behind it. Asked from the device: "shouldn't the whole thing be
 * divable?" It should: the user is looking at one picture, and a click on the
 * left third of it doing something while the middle does nothing is the
 * picture disagreeing with itself.
 *
 * So a cell that is part of a sample graphic and has no door of its own dives
 * to the graphic's own anchor — the position marker, whose editor draws that
 * same waveform full-screen with the same fences.
 *
 * DERIVED, never named. The rule is "a member of the picture with nothing
 * behind it", not "spray": nothing here mentions the key, so a future member
 * seated into a sample graphic inherits this without a second edit, and a
 * member that HAS its own door (the filepath) keeps it — the `self.divable`
 * bail is what protects the file browser from being swallowed by the editor.
 *
 * Scoped to VIZ_SAMPLE alone. An envelope or a filter curve is a picture of
 * several knobs with no editor behind it, so there is nothing to dive to and
 * this would invent a destination.
 *
 * ONE definition for three consumers — the click (page_controller), the
 * footer hint (shadow_ui_param_pages) and the corner brackets
 * (render_page_movy). Three copies of "is this cell a door" is how a footer
 * comes to promise CLK OPEN over a click that does nothing, which is the
 * mismatch the hint site already carries a comment about.
 *
 * @returns {string|null} the key to open instead, or null for "no redirect".
 */
export function vizDiveTarget(groups, key, metaIndex) {
    if (!key || !metaIndex || !groups) return null;
    const self = metaIndex.getOrGuess(key);
    if (!self || self.divable) return null;
    for (const g of groups) {
        if (!g || g.kind !== VIZ_SAMPLE) continue;
        if (!Array.isArray(g.keys) || g.keys.indexOf(key) < 0) continue;
        const anchor = g.roles && g.roles.position;
        if (!anchor || anchor === key) continue;
        const am = metaIndex.getOrGuess(anchor);
        if (am && am.divable) return anchor;
    }
    return null;
}

/* ---------------------------------------------------------------- resolve */

/**
 * @param {object}   o
 * @param {Array}    o.keys        page.keys — up to 8, may contain nulls
 * @param {object}   o.metaIndex   from param_meta.buildMetaIndex
 * @param {Function} [o.overrides] (key) => vizObj | false | null — host
 *                                 correction, checked after declared and
 *                                 before the detector runs for that key.
 * @returns {{groups: Array, invalid: Array}} groups sorted by slotStart.
 *   `invalid` lists declared groups that could not be drawn (e.g. roles not
 *   adjacent) — validate.mjs surfaces these so an author can see why nothing
 *   appeared.
 */
export function resolveViz({ keys, metaIndex, overrides, ignoreRows } = {}) {
    if (!keys || !metaIndex) return { groups: [], invalid: [] };
    IGNORE_ROWS = !!ignoreRows;
    try {
        return resolveVizInner({ keys, metaIndex, overrides });
    } finally {
        IGNORE_ROWS = false;
    }
}

function resolveVizInner({ keys, metaIndex, overrides }) {
    const invalid = [];
    const { groups: declared, excluded } = collectDeclared(keys, metaIndex, invalid);

    const claimed = new Set();
    for (const g of declared) for (const k of g.keys) claimed.add(k);

    const out = [...declared];

    /* Host override: a key not already declared can be forced into a group or
     * kind by the host, exactly as if the module had declared it — this is
     * the mechanism that corrects a wrong detector guess in the field. */
    if (typeof overrides === "function") {
        const overridden = new Map();
        keys.forEach((key, slot) => {
            if (!key || claimed.has(key) || excluded.has(key)) return;
            const v = overrides(key);
            if (v === false) { excluded.add(key); return; }
            if (!v || typeof v !== "object") return;
            if (v.group) {
                if (!overridden.has(v.group)) overridden.set(v.group, { kind: v.kind || null, roles: {} });
                const g = overridden.get(v.group);
                if (v.role) g.roles[v.role] = { key, slot };
                if (v.kind && !g.kind) g.kind = v.kind;
            } else if (v.kind) {
                claimed.add(key);
                out.push({
                    kind: v.kind, group: null, roles: { value: key }, keys: [key],
                    slotStart: slot, slotSpan: 1, source: VIZ_SOURCE_OVERRIDE,
                });
            }
        });
        for (const [groupId, g] of overridden) {
            const slots = Object.values(g.roles).map((r) => r.slot);
            const kind = g.kind || inferKindFromRoles(Object.keys(g.roles));
            if (!kind || !isAdjacentRun(slots)) continue;
            for (const r of Object.values(g.roles)) claimed.add(r.key);
            out.push({
                kind, group: groupId, roles: mapRoles(g.roles),
                keys: Object.values(g.roles).map((r) => r.key),
                ...span(slots), source: VIZ_SOURCE_OVERRIDE,
            });
        }
    }

    /* Detector pool: every slot not yet claimed or excluded. */
    let pool = [];
    keys.forEach((key, slot) => {
        if (!key || claimed.has(key) || excluded.has(key)) return;
        pool.push({ key, slot, meta: metaIndex.getOrGuess(key) });
    });

    for (const detector of DETECTORS) {
        if (pool.length === 0) break;
        const fired = detector(pool, metaIndex);
        for (const g of fired) {
            /* A slot the group needs may have been claimed by an earlier
             * detector this same pass (unlikely given disjoint role
             * vocabularies, but keys are never drawn twice). */
            if (g.keys.some((k) => claimed.has(k))) continue;
            for (const k of g.keys) claimed.add(k);
            out.push(g);
        }
        pool = pool.filter((item) => !claimed.has(item.key));
    }

    out.sort((a, b) => a.slotStart - b.slotStart);
    return { groups: out, invalid };
}

/**
 * Nudge a page's knob order so a group that is one slot from being drawable
 * becomes drawable.
 *
 * A graphic must sit inside ONE ROW (isAdjacentRun) because row 0's knobs are
 * drawn at y=10 with their LABELS at y=25..32 and row 1 starts at y=33 — a
 * shape spanning both would draw straight through the label band. That is a
 * real constraint, not a tunable one.
 *
 * The consequence, measured on the 95-module fleet: 26 groups are rejected for
 * LAYOUT alone, and they are the flagship case — the ADSR on the Main page of
 * obxd, hush1, minijv, moog, surge, rex, osirus, helm, braids and sfz, plus
 * twelve surge LFO pages. An author writing `attack, decay, sustain, release`
 * in the obvious order lands on slots 3..6 and gets four separate dials.
 *
 * WHAT THIS DOES NOT DO is as important as what it does:
 *
 *   * it never changes WHICH keys are on the page, so no knob is pushed to
 *     another page and no orphan page is created. Max group span is 4 and a
 *     row is 4 wide, so a group always fits somewhere in the 8 — measured, 25
 *     of the 26 displace exactly ONE knob and one displaces two.
 *   * it never reorders for cosmetics. The move must strictly increase the
 *     number of keys covered by a group; a page whose groups already draw is
 *     returned untouched.
 *   * it preserves relative order. The candidate is a BLOCK MOVE, so the
 *     author's sequence survives apart from the block that moved.
 *
 * It is still us overruling a hand-written layout, so page_plan records it and
 * validate_contract surfaces it — an author who wonders why their cutoff moved
 * gets an answer instead of a mystery.
 *
 * @param {Array} keys       page.keys, up to KNOBS_PER_PAGE, may contain nulls
 * @param {object} metaIndex from param_meta.buildMetaIndex
 * @returns {{keys: Array, moved: boolean, from: number, to: number, span: number}}
 */
/* Aligning a block inside a row of four never needs more than three shifts, so
 * anything beyond that is not alignment. */
export const MAX_ALIGN_DISPLACE = 3;

export function alignGroupsToRows(keys, metaIndex) {
    const none = { keys, moved: false, from: -1, to: -1, span: 0 };
    if (!keys || !metaIndex || keys.length <= ROW_WIDTH) return none;

    const sig = (gs) => new Set(gs.map((g) => g.keys.join("\u0000")));
    const drawn = sig(resolveViz({ keys, metaIndex }).groups || []);
    /* The counterfactual: what would group if a shape could span the rows. */
    const wanted = (resolveViz({ keys, metaIndex, ignoreRows: true }).groups || [])
        .filter((g) => !drawn.has(g.keys.join("\u0000")))
        /* A non-contiguous candidate is not a layout problem, it is a
         * different page; only a run that straddles the row break is
         * rescuable by moving it. */
        .filter((g) => g.slotSpan === g.keys.length && g.slotSpan <= ROW_WIDTH)
        .sort((a, b) => b.slotSpan - a.slotSpan);
    if (wanted.length === 0) return none;

    const move = (arr, from, span, to) => {
        const block = arr.slice(from, from + span);
        const rest = arr.slice(0, from).concat(arr.slice(from + span));
        return rest.slice(0, to).concat(block, rest.slice(to));
    };

    for (const g of wanted) {
        /*
         * ROW TWO IS PREFERRED, BUT ONLY FOR A BLOCK THAT HAS TO MOVE.
         *
         * "Always put the envelope on row two" is tempting and wrong: 29
         * envelopes in the fleet already sit inside row one and draw
         * correctly, and many of them are on pages that exist FOR that
         * envelope -- obxd/Filter Env, hush1/Amp Envelope, hera/Envelope,
         * tablor/Env -- where slots 0..3 is exactly where it belongs and row
         * one would otherwise be empty. An always-rule makes 29 pages worse to
         * fix 24.
         *
         * For a block that is straddling and must move regardless, row two is
         * the better destination: it keeps whatever the author put FIRST
         * (cutoff and resonance, almost always) on knobs 1 and 2. minijv is
         * the case — its ADSR sits at slots 2..5, and moving it down keeps
         * macro_cutoff on knob 1, where a nearest-fit search moved it to
         * knob 5.
         */
        const targets = [ROW_WIDTH, 2 * ROW_WIDTH - g.slotSpan, 0, ROW_WIDTH - g.slotSpan];
        for (const to of targets) {
            if (to < 0 || to + g.slotSpan > keys.length) continue;
            if (rowOf(to) !== rowOf(to + g.slotSpan - 1)) continue;
            if (Math.abs(to - g.slotStart) > MAX_ALIGN_DISPLACE) continue;
            const cand = move(keys, g.slotStart, g.slotSpan, to);
            /* Verify against the REAL detector. The counterfactual said this
             * group wants to exist; only the real pass can say it now does,
             * and that it did not cost an existing one. */
            const after = sig(resolveViz({ keys: cand, metaIndex }).groups || []);
            if (!after.has(g.keys.join("\u0000"))) continue;
            let lost = false;
            for (const d of drawn) if (!after.has(d)) { lost = true; break; }
            if (lost) continue;
            return { keys: cand, moved: true, from: g.slotStart, to, span: g.slotSpan };
        }
    }
    return none;
}

/**
 * Seat a graphic's scattered members together so the picture gets the width
 * its controls warrant.
 *
 * `alignGroupsToRows` rescues a group that is ALREADY contiguous but straddles
 * the row break. This is the other half: members that are on the page, belong
 * to the same picture, and are simply not next to each other. granny is the
 * case — `spray` sits three knobs from `position`, so the fences drew on a
 * 30px cell while the knob controlling them sat elsewhere looking unrelated.
 *
 * Measured over the 95-module fleet, exactly three pages change:
 *
 *     granny   / root     span 1 -> 2   (position, spray)
 *     granny   / main     span 1 -> 2
 *     mrsample / sample   span 1 -> 3   (sample_start, loop_start, loop_end)
 *
 * The other six sample groups have nothing scattered and are untouched. That
 * narrowness is the point: this is not a layout engine that re-seats every
 * page, it is a nudge for the pages whose author wrote the members apart.
 *
 * The guarantees are `alignGroupsToRows`'s, deliberately, because they are the
 * ones that make a reorder safe to perform behind an author's back:
 *
 *   * WHICH keys are on the page never changes, so no knob is pushed to
 *     another page and no orphan page appears;
 *   * the result must stay inside ONE ROW, because a shape spanning the break
 *     would draw through the label band;
 *   * the REAL detector verifies the outcome — the widened group must exist
 *     afterwards, and no group that already drew may be lost.
 *
 * @param {Array<string|null>} keys  the page's 8 knob slots
 * @param {object} metaIndex
 * @returns {{keys: Array, moved: boolean, span: number}}
 */
export function gatherGroupMembers(keys, metaIndex) {
    const none = { keys, moved: false, span: 0 };
    if (!keys || !metaIndex) return none;

    const sigOf = (gs) => new Set(gs.map((g) => g.keys.join(" ")));
    const before = resolveViz({ keys, metaIndex }).groups || [];
    const drawn = sigOf(before);

    for (const g of before) {
        /* Only a graphic whose members can be READ from off-cell has anything
         * to gather; that is the sample cell today. An envelope's roles are
         * its cells by construction. */
        if (g.kind !== VIZ_SAMPLE) continue;

        /* On this page, belongs to this picture, is not already a cell of it.
         *
         * NOT the FILE, which is a role but never a cell of a marker's graphic
         * (see detectSample). Leaving it in overstated `wantSpan` by one, and
         * since the widened result is verified against that number by the real
         * detector at the bottom of this loop, the check could never be
         * satisfied — so the gather was abandoned entirely and the members it
         * WOULD have seated stayed scattered. granny's "Main - 2" collapsed
         * from a two-cell waveform to a one-cell one with the spray arc back
         * three knobs away, which is the exact layout the gather pass exists
         * to fix. Nothing about the failure said "the file": the group simply
         * stopped widening. */
        const scattered = [...new Set(Object.values(g.roles))]
            .filter((k) => k !== g.roles.value)
            .filter((k) => keys.indexOf(k) >= 0 && g.keys.indexOf(k) < 0);
        if (scattered.length === 0) continue;

        const wantSpan = g.keys.length + scattered.length;
        if (wantSpan > ROW_WIDTH) continue;

        /* Seat them immediately after the run that already draws, preserving
         * the author's relative order among the ones being moved. */
        const anchorLast = g.slotStart + g.slotSpan - 1;
        const moving = keys.filter((k) => k && scattered.indexOf(k) >= 0);
        const rest = keys.filter((k) => !k || moving.indexOf(k) < 0);
        const insertAt = rest.indexOf(keys[anchorLast]) + 1;
        if (insertAt <= 0) continue;
        const cand = rest.slice(0, insertAt).concat(moving, rest.slice(insertAt));

        /* One row, or the shape draws through the label band. */
        const start = cand.indexOf(keys[g.slotStart]);
        if (start < 0) continue;
        if (rowOf(start) !== rowOf(start + wantSpan - 1)) continue;

        const afterGroups = resolveViz({ keys: cand, metaIndex }).groups || [];
        /* It must actually have widened — the detector, not the arithmetic,
         * decides whether these cells form one graphic. */
        if (!afterGroups.some((x) => x.kind === VIZ_SAMPLE && x.slotSpan === wantSpan)) continue;
        const after = sigOf(afterGroups);
        let lost = false;
        for (const d of drawn) {
            if (after.has(d)) continue;
            /* The group we deliberately widened is EXPECTED to have a new
             * signature; anything else going missing is a regression. */
            if (d === g.keys.join(" ")) continue;
            lost = true; break;
        }
        if (lost) continue;

        return { keys: cand, moved: true, span: wantSpan };
    }
    return none;
}
