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

import { KIND_NUMBER, KIND_ENUM, KIND_OPAQUE } from "./param_meta.mjs";

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
function stemsAgree(items) {
    if (items.length < 2) return true;
    return items.every((i) => i.stem === items[0].stem);
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
            singles.push({ kind: v.kind, key, slot });
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
        if (!slots.length) {
            invalid.push({ group: g.groupId, kind, reason: "no spanning roles" });
            continue;
        }
        if (!isAdjacentRun(slots)) {
            invalid.push({ group: g.groupId, kind, reason: "roles not adjacent on one row" });
            continue;
        }
        out.push({
            kind, group: g.groupId, roles: mapRoles(g.roles),
            keys: spanning.map((r) => r.key),
            ...span(slots), source: VIZ_SOURCE_DECLARED,
        });
    }
    for (const s of singles) {
        out.push({
            kind: s.kind, group: null, roles: { value: s.key }, keys: [s.key],
            slotStart: s.slot, slotSpan: 1, source: VIZ_SOURCE_DECLARED,
        });
    }
    return { groups: out, excluded };
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

const LFO_WORD = {
    shape: /shape|waveform|wave/,
    rate: /(^|_)(rate|speed|freq)($|_)/,
    depth: /(^|_)(depth|amount|amt)($|_)/,
    phase: /(^|_)phase($|_)/,
};

function detectLfo(pool) {
    let shape = null, rate = null, depth = null, phase = null;
    for (const item of pool) {
        const k = item.key.toLowerCase();
        if (!shape && LFO_WORD.shape.test(k) && isEnum(item.meta) &&
            (item.meta.options || []).some((o) => WAVEFORM_NAMES.test(String(o)))) shape = { ...item, stem: stemOf(k, LFO_WORD.shape) };
        else if (!rate && LFO_WORD.rate.test(k)) rate = { ...item, stem: stemOf(k, LFO_WORD.rate) };
        else if (!depth && LFO_WORD.depth.test(k)) depth = { ...item, stem: stemOf(k, LFO_WORD.depth) };
        else if (!phase && LFO_WORD.phase.test(k)) phase = { ...item, stem: stemOf(k, LFO_WORD.phase) };
    }
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
    if (shape && shape.stem === rate.stem) { roles.shape = shape.key; items.push(shape); }
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

function detectFader(pool) {
    const out = [];
    for (const item of pool) {
        const k = item.key.toLowerCase();
        if (!isNumeric(item.meta)) continue;
        if (isEqBandIsh(k)) continue;
        if (!/(^|_)(gain|volume|vol|level|amp)($|_)/.test(k)) continue;
        /* A fader is a unipolar level; a bipolar range is a pan or a trim,
         * not a volume, whatever the name says. */
        if (typeof item.meta.min === "number" && item.meta.min < 0) continue;
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
        if (!isEnum(item.meta)) continue;
        const opts = (item.meta.options || []).map(String);
        if (opts.length !== 2) continue;
        if (!opts.every((o) => BOOL_OPTION.test(o.trim()))) continue;
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

function detectSample(pool, metaIndex) {
    const out = [];
    for (const item of pool) {
        if (item.meta.type !== "filepath" && item.meta.type !== "file") continue;
        /* The companion position marker is a `wav_position` param that may or
         * may not be on this same page — search the whole module, not just
         * the pool, the way the module contract intends a "position" role to
         * work. Prefer one whose key shares a stem with the sample key. */
        let position = null;
        const stem = item.key.replace(/(path|file|sample)$/i, "");
        for (const k of metaIndex.keys) {
            const m = metaIndex.get(k);
            if (!m || m.type !== "wav_position") continue;
            if (!position || (stem && k.startsWith(stem))) position = k;
        }
        const roles = { value: item.key };
        if (position) roles.position = position;
        out.push({
            kind: VIZ_SAMPLE, group: null, roles, keys: [item.key],
            slotStart: item.slot, slotSpan: 1, source: VIZ_SOURCE_DETECTED,
        });
    }
    return out;
}

/* Priority order — see the module doc comment. Each function returns the
 * groups it fires; every key any of them returns is removed from the pool
 * before the next detector runs. */
const DETECTORS = [
    detectEnvelope, detectFilter, detectLfo, detectWaveform,
    detectFader, detectSwitch, detectEq,
    (pool, metaIndex) => detectSample(pool, metaIndex),
];

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
