/*
 * bus_model.mjs — a slot's BUSES as data: the wire formats, the row lists and
 * the writes, with no device and no drawing in any of it.
 *
 * Pure for the same reason chain_model.mjs is: every rule below has to be
 * runnable under node in tests/host, and shadow_ui_buses.mjs (which draws it)
 * resolves its imports from /data/UserData/schwung and cannot be loaded there.
 *
 * ================= THE READ THAT DECIDES WHETHER ANY OF THIS EXISTS ==========
 *
 * `synth:split_voices` has THREE answers and they are not two:
 *
 *   JSON   the module splits; these are its voices
 *   ""     the channel served us and the key produced nothing — the module
 *          cannot split, and NOTHING is offered: no row, no hint, no screen
 *   null   the read did not complete. NOT news about the module.
 *
 * chain_host.c clamps a plugin's -1 to "" deliberately (see its split_voices
 * branch) so that "no split support" and "the read failed" cannot collide —
 * which means a null reaching here is a real channel failure and the only
 * correct response is to wait and ask again. parseSplitVoices branches on the
 * RAW value for that reason: JSON.parse of a null and of an empty string both
 * lose the distinction, and by the time a caller holds a parsed value only the
 * code that saw the wire can still report which it was.
 *
 * ================= ORPHANS ==================================================
 *
 * A bus stores voice IDS, so an id that no longer resolves (the module was
 * swapped, or gained or lost a voice) is RETAINED and COUNTED rather than
 * dropped or re-pointed. The screens must show that: a partial restore that
 * reports nothing is indistinguishable from a working one. busRowLabel marks
 * such a bus with a trailing "!" and voiceRows emits every unresolved id as its
 * own row, so the ids are readable and can be cleared.
 */

/* ---- Caps. Mirrors of the C constants, the way MASTER_FX_SLOTS is --------- *
 *
 * SLOT_BUSES and BUS_FX_SLOTS are chain_internal.h's SLOT_BUSES and
 * MAX_AUDIO_FX; BUS_SENDS is bus_mix.h's BUS_MIX_SENDS and SEND_LEVEL_MAX its
 * BUS_MIX_SEND_LEVEL_MAX. Nothing here re-derives them from a screen. */
export const SLOT_BUSES = 4;
export const BUS_FX_SLOTS = 8;
export const BUS_SENDS = 2;
export const SEND_LEVEL_MAX = 127;
/* A detent per unit would make a full sweep 127 turns of the jog, so a send row
 * steps by four — the same step the FX-bus settings rows use. */
export const SEND_LEVEL_STEP = 4;

/* ==========================================================================
 * THE WIRE — parsing, all of it tri-state-aware
 * ========================================================================== */

/*
 * `synth:split_voices` -> { unresolved } | { voices: [{id,label}, ...] }
 *
 * `voices: []` is a REAL answer meaning "this module does not split", and it is
 * what every caller tests to decide whether a bus affordance exists at all.
 */
export function parseSplitVoices(raw) {
    if (raw === null || raw === undefined) return { unresolved: true, voices: [] };
    const s = String(raw).trim();
    if (s === "") return { unresolved: false, voices: [] };
    let arr;
    try { arr = JSON.parse(s); } catch (e) { return { unresolved: false, voices: [] }; }
    if (!Array.isArray(arr)) return { unresolved: false, voices: [] };
    /* THE INDEX IS THE RENDER-BUFFER INDEX (split_voices_parse.h), so an entry
     * with no usable id keeps its place as a hole rather than being compacted
     * out — a shifted index silently re-points every voice behind it. */
    const voices = arr.map((v, i) => {
        const id = (v && typeof v === "object" && typeof v.id === "string") ? v.id : "";
        const label = (v && typeof v === "object" && v.label) ? String(v.label) : id;
        return { id, label: label || `Voice ${i + 1}`, index: i };
    });
    return { unresolved: false, voices };
}

/*
 * `buses:config` -> { unresolved } | { buses: [...], mainSends: [a,b] }
 *
 * Positional and never compacted, exactly as bus_emit_config writes it: an
 * empty bus in the middle is a real state and compacting it renumbers
 * everything behind it, which is the defect that lost the Master FX chain.
 */
export function parseBusesConfig(raw) {
    if (raw === null || raw === undefined) return { unresolved: true, buses: [], mainSends: [] };
    const s = String(raw).trim();
    if (s === "") return { unresolved: true, buses: [], mainSends: [] };
    let o;
    try { o = JSON.parse(s); } catch (e) { return { unresolved: true, buses: [], mainSends: [] }; }
    if (!o || !Array.isArray(o.buses)) return { unresolved: true, buses: [], mainSends: [] };
    const buses = [];
    for (let b = 0; b < SLOT_BUSES; b++) {
        const raw_b = o.buses[b] || {};
        buses.push({
            index: b,
            present: !!raw_b.present,
            name: String(raw_b.name || `Bus ${b + 1}`),
            orphans: Number(raw_b.orphans) || 0,
            voices: Array.isArray(raw_b.voices) ? raw_b.voices.map(String) : [],
            sends: normaliseSends(raw_b.sends),
            fx: normaliseFx(raw_b.fx),
        });
    }
    return { unresolved: false, buses, mainSends: normaliseSends(o.main_sends) };
}

function normaliseSends(arr) {
    const out = [];
    for (let i = 0; i < BUS_SENDS; i++) {
        const v = Array.isArray(arr) ? Number(arr[i]) : 0;
        out.push(Number.isFinite(v) ? Math.max(0, Math.min(SEND_LEVEL_MAX, v)) : 0);
    }
    return out;
}

function normaliseFx(arr) {
    const out = [];
    for (let k = 0; k < BUS_FX_SLOTS; k++) {
        const e = (Array.isArray(arr) && arr[k]) ? arr[k] : {};
        out.push({ module: String(e.module || ""), bypassed: !!e.bypassed });
    }
    return out;
}

/* ==========================================================================
 * THE MODELS
 * ========================================================================== */

/*
 * What a bus's inserts say in one column: "cho>pha", or "--" for none.
 *
 * `abbrev` is the caller's module-abbreviation function (getModuleAbbrev in
 * shadow_ui.js, which is cached and knows a module's declared short name);
 * passing it in keeps this pure and keeps ONE abbreviation rule in the UI.
 */
export function insertSummary(fx, abbrev) {
    const parts = [];
    for (const e of (fx || [])) {
        if (!e || !e.module) continue;
        parts.push(abbrev ? abbrev(e.module) : String(e.module).slice(0, 2).toUpperCase());
    }
    if (!parts.length) return "--";
    /* THE COLUMN IS ~11 CHARACTERS and it also carries both send levels, so a
     * chain of three abbreviations spends the whole row on the half of the
     * answer the bus's own screen already gives in full. Past two, the COUNT is
     * what fits and is still true; the names are one click away. Measured, not
     * assumed — the first form ("cho>pha>tap 20/15") truncated to "cho..." in
     * the render and lost both levels with it. */
    if (parts.length > 2) return `${parts.length} FX`;
    return parts.join(">");
}

/*
 * The rows of the bus list: every PRESENT bus, then Main, then New Bus if a
 * free bus is left.
 *
 * Main is last and always present: it is where every unassigned voice already
 * plays, so it is the row you compare the others against, and its send levels
 * are the slot's own. It is a row rather than a header because it has the same
 * two send values as any bus — `main_sends` in the same config document.
 */
export function busListRows(config, abbrev) {
    const rows = [];
    if (!config || config.unresolved) return rows;
    for (const b of config.buses) {
        if (!b.present) continue;
        rows.push({
            kind: "bus", index: b.index, name: b.name, orphans: b.orphans,
            summary: insertSummary(b.fx, abbrev), sends: b.sends,
        });
    }
    rows.push({
        kind: "main", index: -1, name: "Main", orphans: 0,
        summary: "--", sends: config.mainSends,
    });
    if (config.buses.some((b) => !b.present)) rows.push({ kind: "new", name: "New Bus" });
    return rows;
}

/* The lowest bus index not in use, or -1. Positional: a hole in the middle is
 * filled before a later index, because the array is never compacted and a
 * deleted bus 2 must be re-creatable as bus 2. */
export function firstFreeBus(config) {
    if (!config || config.unresolved) return -1;
    for (const b of config.buses) if (!b.present) return b.index;
    return -1;
}

/* A row's LABEL and VALUE for the one list engine. The orphan mark is part of
 * the label because it is a fact about the bus, not about its sends, and it has
 * to survive the value column being truncated. */
export function busRowLabel(row) {
    if (!row) return "";
    if (row.kind === "new") return "New Bus";
    return row.orphans > 0 ? `${row.name} !` : row.name;
}

export function busRowValue(row) {
    if (!row || row.kind === "new") return "";
    const [a, b] = row.sends || [0, 0];
    /* "A/B", not "A B": the slash is what says these are two values rather than
     * one number the eye has to split. */
    return `${row.summary} ${a}/${b}`;
}

/*
 * The voice rows for one bus: every voice the module declares, then every id
 * this bus holds that no longer resolves.
 *
 * `on` is which bus that voice is currently assigned to (-1 = Main), so the
 * screen answers "where is each voice" and not only "is it mine". Assigning a
 * voice that belongs to another bus MOVES it — a voice renders into exactly one
 * buffer, so membership is exclusive by construction and the screen must not
 * pretend otherwise.
 *
 * An ORPHAN row carries `orphan: true` and its raw id as the label, because the
 * id is the only thing left of it and hiding it would leave a counted orphan
 * with nothing to act on.
 */
export function voiceRows(config, voices, busIndex) {
    const rows = [];
    if (!config || config.unresolved) return rows;
    const owner = {};
    for (const b of config.buses) {
        if (!b.present) continue;
        for (const id of b.voices) if (owner[id] === undefined) owner[id] = b.index;
    }
    for (const v of voices || []) {
        if (!v.id) continue;   /* a hole in the module's own list */
        const on = owner[v.id] === undefined ? -1 : owner[v.id];
        rows.push({ kind: "voice", id: v.id, label: v.label, on, mine: on === busIndex });
    }
    const known = {};
    for (const v of voices || []) if (v.id) known[v.id] = true;
    const bus = config.buses[busIndex];
    for (const id of (bus ? bus.voices : [])) {
        if (known[id]) continue;
        rows.push({ kind: "orphan", id, label: id, on: busIndex, mine: true });
    }
    return rows;
}

/* The row's value column: whose it is. "*" for this bus, another bus's NAME
 * when it belongs to one (so moving it is an informed choice rather than a
 * surprise), and nothing for a voice that is on Main. */
export function voiceRowValue(row, config) {
    if (!row) return "";
    /* The SAME "!" the bus list marks an orphaned bus with — one mark, one
     * meaning, on both screens. It was "missing", which the value column cut to
     * "mis..." in the render: four characters of nothing where a single legible
     * one says it. */
    if (row.kind === "orphan") return "!";
    if (row.mine) return "*";
    if (row.on < 0) return "";
    const b = config && config.buses ? config.buses[row.on] : null;
    /* The whole name, not a truncated one: the list engine fits a value in
     * PIXELS against the row it is drawn in, and a character count applied
     * first would cut a name that fits. */
    return b ? b.name : "";
}

/*
 * The voice-id list to write to `bus<N>:voices` after toggling `id`.
 *
 * Orphans are CARRIED, not silently dropped: the write is a whole-list replace,
 * so rebuilding it from only the resolvable voices would quietly erase the very
 * ids the orphan count exists to report. Toggling an orphan row off is how they
 * are cleared, and it is the only thing that clears them.
 */
export function toggledVoiceIds(config, busIndex, id) {
    const bus = config && config.buses ? config.buses[busIndex] : null;
    const cur = bus ? bus.voices.slice() : [];
    const at = cur.indexOf(id);
    if (at >= 0) cur.splice(at, 1);
    else cur.push(id);
    return cur;
}

/* Removing `id` from whichever OTHER bus holds it, so a move is one write per
 * bus rather than a voice silently listed in two places. Returns
 * [{bus, ids}, ...] for the caller to write. */
export function voiceMoveWrites(config, busIndex, id) {
    const out = [];
    for (const b of (config && config.buses ? config.buses : [])) {
        if (!b.present || b.index === busIndex) continue;
        if (b.voices.indexOf(id) < 0) continue;
        out.push({ bus: b.index, ids: b.voices.filter((v) => v !== id) });
    }
    return out;
}

/*
 * One bus's action menu. Filtered by what the row IS: Main has no chain, no
 * voices of its own and no name, so it is offered neither — a list must never
 * carry a row that answers a click by doing nothing.
 */
export function busActionItems(row) {
    if (!row) return [];
    const items = [];
    if (row.kind === "bus") {
        items.push({ id: "voices", label: "Voices" });
        items.push({ id: "chain", label: "Inserts" });
    }
    items.push({ id: "send1", label: "Send A", type: "int" });
    items.push({ id: "send2", label: "Send B", type: "int" });
    if (row.kind === "bus") {
        items.push({ id: "rename", label: "Rename" });
        items.push({ id: "delete", label: "Delete" });
    }
    return items;
}

/* The positions of a bus's insert chain, as chain_diagram components: every
 * position up to the last loaded one, then a `+`. Positional — a hole in the
 * middle stays a hole and draws as "--", because bus_emit_config never compacts
 * and neither may the picture of it. */
export function busChainComponents(fx) {
    const list = fx || [];
    let last = -1;
    for (let k = 0; k < BUS_FX_SLOTS; k++) if (list[k] && list[k].module) last = k;
    const out = [];
    for (let k = 0; k <= last; k++) {
        out.push({
            id: `fx${k + 1}`, kind: "module", section: "fx", index: k,
            label: `FX ${k + 1}`, module: (list[k] && list[k].module) || "",
        });
    }
    if (last + 1 < BUS_FX_SLOTS) out.push({ id: "add_fx", kind: "add", section: "fx", label: "+" });
    return out;
}

/* A row's level for one of the two sends. `id` is the action-item id, so the
 * menu never has to know which array index "Send B" is. */
export function busSendValue(row, id) {
    const at = id === "send2" ? 1 : 0;
    return (row && row.sends && row.sends[at] !== undefined) ? row.sends[at] : 0;
}
