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
export const SLOT_BUSES = 8;
export const BUS_FX_SLOTS = 8;
export const BUS_SENDS = 2;
export const SEND_LEVEL_MAX = 127;
/* chain_internal.h's SPLIT_VOICES_MAX — how many voices a module may declare,
 * and therefore how many a bus's voice multi-select can list. */
export const SPLIT_VOICES_MAX = 32;
/* A detent per unit would make a full sweep 127 turns of the jog, so a send row
 * steps by four — the same step the FX-bus settings rows use. */
export const SEND_LEVEL_STEP = 4;

/*
 * THE SEND MIXER RIDES BUSES, NOT VOICES.
 *
 * It carried a fader per voice for a while, fed by a `buses:voice<V>:send<M>`
 * route and a `voice_sends` array in the slot document. dr32 already published
 * per-pad `send1`/`send2` knobs on its own `pads` level, beside pan and cutoff
 * — so the same number had two homes that did not agree, which is what this
 * screen was actually showing. A VOICE's send belongs to the module (its
 * pages, its state blob), a BUS's to the bus row here, a SLOT's to Slot
 * Settings. The audio path is unchanged; only the owner of the numbers moved.
 * See src/host/voice_send_source.h.
 */

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
    return {
        unresolved: false, buses,
        mainSends: normaliseSends(o.main_sends),
    };
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
 * The rows of the bus list: every PRESENT bus, then the Sends row, then New Bus
 * if a free bus is left.
 *
 * THERE IS NO "MAIN" ROW HERE, and the slot's own two send levels are not on
 * this screen. They are real now — chain_drain_main_send is the second drain
 * point, taken after the slot FX in the shim's mix pass, which is why they were
 * inert before and are not any more — but they belong to the SLOT, not to its
 * buses, so they live in Slot Settings beside Volume and the channels. Most
 * modules publish no `split_voices` and never see this screen at all; a slot
 * send that could only be reached through it would be unreachable for them,
 * which is the exact hole it exists to close.
 *
 * The row is `Send Mixer`, not `Sends`, and the difference is not cosmetic:
 * under a menu called Buses, "Sends" reads as "this slot's sends" and means
 * "a mixer for the buses' sends" — an ambiguity that got worse the moment the
 * slot acquired sends of its own. It is offered only when there is at least
 * one bus or voice to ride: a mixer with no faders is a row that answers a
 * click by doing nothing.
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
    /*
     * NO SEND-MIXER ROW. This screen is about MAKING AND FILLING CONTAINERS --
     * Bus 1..N and New Bus -- and the mixer is about LEVELS INTO A AND B. They
     * were one screen, so "New Bus" sat beside a mixer, and the mixer that
     * called itself the sends view silently omitted the slot Main row, whose two
     * levels lived two screens away under a different name.
     *
     * The mixer is its own `Sends` row on Slot Settings now, beside this one,
     * and it carries Main. Reported as "combining buses and sends is weird".
     */
    if (config.buses.some((b) => !b.present)) rows.push({ kind: "new", name: "New Bus" });
    return rows;
}

/*
 * How many buses this slot actually has — what the `Buses` settings row shows
 * beside its label, the way the module picker's row shows "2 FX".
 *
 * -1 for an UNRESOLVED config, never 0: "no buses" and "the read did not
 * complete" are different sentences and the row prints different things for
 * them. Collapsing the two is the tri-state mistake this repo has paid for
 * three times in one day.
 */
export function busCount(config) {
    if (!config || config.unresolved) return -1;
    return config.buses.filter((b) => b.present).length;
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
    /* THE HIGHEST BUS WINS, and that is not a preference — it is what the audio
     * does. chain_bus_rebuild_voice_map applies the buses in ascending order
     * into one voice_bus[] table and bus_voice_apply only ever WRITES, so a
     * voice listed on two buses renders into the LAST one that claimed it.
     * Taking the first here instead made the screen name one bus while the
     * audio used another, silently — reachable whenever a voiceMoveWrites
     * removal fails, or from an externally authored patch. */
    const owner = {};
    for (const b of config.buses) {
        if (!b.present) continue;
        for (const id of b.voices) owner[id] = b.index;
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
 * One bus's action menu. ONLY a bus has one: the Sends row opens the mixer and
 * New Bus creates, and a list must never carry a row that answers a click by
 * doing nothing.
 */
export function busActionItems(row) {
    if (!row || row.kind !== "bus") return [];
    return [
        { id: "voices", label: "Voices" },
        { id: "chain", label: "Inserts" },
        { id: "send1", label: "Send A", type: "int" },
        { id: "send2", label: "Send B", type: "int" },
        { id: "rename", label: "Rename" },
        { id: "delete", label: "Delete" },
    ];
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

/* ==========================================================================
 * THE KNOB GRID — a bus insert's key, and the send mixer's contract
 *
 * Two unrelated things live here for one reason: both are RULES, and this is
 * the file tests/host can run. The screens that draw them cannot be imported
 * at all (shadow_ui_buses.mjs resolves /data/UserData/schwung paths).
 * ========================================================================== */

/*
 * The component key of one insert position — "bus1:fx2".
 *
 * It is the DSP key prefix and the editor's component key at the same time,
 * which is what lets the existing knob grid address a bus insert with no
 * mapping of its own: the grid asks for "<prefix>:<param>" and chain_bus.c
 * serves exactly "bus<N>:fx<K>:<param>". Master FX plays the same trick with
 * "master_fx:fx2" (see masterFxComponentKey in shadow_ui.js); this is the
 * third chain to do it and the second to write the spelling down once.
 *
 * Null outside the caps, deliberately: an out-of-range "bus9:fx1" would
 * otherwise be routed as a real position and land on whatever the chain host
 * does with an unmatched key.
 */
export function busComponentKey(busIndex, fxIndex) {
    if (!(busIndex >= 0 && busIndex < SLOT_BUSES)) return null;
    if (!(fxIndex >= 0 && fxIndex < BUS_FX_SLOTS)) return null;
    return `bus${busIndex + 1}:fx${fxIndex + 1}`;
}

/** The inverse: { bus, fx } 0-based, or null. Bounded the same way. */
export function parseBusComponentKey(componentKey) {
    const m = /^bus(\d+):fx(\d+)$/.exec(String(componentKey || ""));
    if (!m) return null;
    const bus = Number(m[1]) - 1;
    const fx = Number(m[2]) - 1;
    if (!(bus >= 0 && bus < SLOT_BUSES)) return null;
    if (!(fx >= 0 && fx < BUS_FX_SLOTS)) return null;
    return { bus, fx };
}

/*
 * THE SEND MIXER, as a synthesised contract.
 *
 * A slot's sends are already editable — one int row per bus on that bus's own
 * menu — and that is the thing this replaces: a level you have to click into,
 * jog, and click out of is not a level you can RIDE. Every send is one
 * encoder here.
 *
 * ONE PAGE PER SEND, not one page per bus. Both groupings are authored, and
 * this one is bounded by construction: a page is the present buses, so at most
 * SLOT_BUSES cells against the eight knobs, and the
 * planner is handed `paginate: false` because a mixer split across "Send A"
 * and "Send A - 2" would put two of its faders on a page you cannot see while
 * turning the others. The per-bus grouping is 2 cells a page and ten pages.
 *
 * The rows are the SAME rows the list draws, in the same order (busListRows
 * without New Bus), so the two screens cannot disagree about what a slot
 * holds or what it is called.
 */

/* The grid key for one row's send. Flat — the mapping back to the two
 * real spelling ("bus2:send1") is busSendGridRealKey, and it is the only place
 * that knows it.
 *
 * EXPORTED for the pin below: comparing the grid path against the list path
 * needs a row and a send to produce a grid key outside this module. */
export function sendGridKey(row, send) {
    return `bus${row.index + 1}_send${send}`;
}

/**
 * The real DSP key a grid key reads and writes, or null when it names no send.
 *
 * TWO spellings now — "bus2:send1" and "buses:voice7:send1" — and they are two
 * because they are two different things: a bus's send is post-insert and
 * post-fader, a voice's is pre-insert (and post-fader). One key that meant
 * either depending on what happened to be loaded is exactly the "two meanings
 * behind one control" the Main row was removed for.
 *
 * Same rule as busSendKey in shadow_ui.js, which the list path uses — and the
 * reason both exist rather than one is that the list addresses a ROW object and
 * the grid addresses a KEY.
 *
 * THEY MUST AGREE, and test_bus_model.sh pins it by LIFTING busSendKey out of
 * shadow_ui.js and running the two against every row of a slot — a comment
 * saying they must agree, with nothing joining them, is the duplication it
 * claims to have closed.
 */
export function busSendGridRealKey(gridKey) {
    const key = String(gridKey || "");
    /* MAIN. The slot's own two levels, which this mixer now carries: it is the
     * one screen that answers "where is my reverb coming from", and it could not
     * while the source most slots use was edited two screens away. The key is
     * unchanged -- `buses:main_send<N>`, exactly what Slot Settings wrote. */
    const main = /^main_send(\d+)$/.exec(key);
    if (main) {
        const n = Number(main[1]);
        if (!(n >= 1 && n <= BUS_SENDS)) return null;
        return `buses:main_send${n}`;
    }
    const bus = /^bus(\d+)_send(\d+)$/.exec(key);
    if (bus) {
        const b = Number(bus[1]);
        const n = Number(bus[2]);
        if (!(b >= 1 && b <= SLOT_BUSES)) return null;
        if (!(n >= 1 && n <= BUS_SENDS)) return null;
        return `bus${b}:send${n}`;
    }
    /* NO "voice<N>_send<M>" FORM. It resolved to "buses:voice<V>:send<M>", a
     * route the chain host no longer serves — a voice's send level is one of
     * the module's own parameters now. A key that named one must answer null,
     * not a key the host would refuse silently. */
    return null;
}

/**
 * Every declared param of the send mixer — all of its pages' worth.
 *
 * `short_name` is the enum square's problem in another costume: a cell is
 * ~30px and a bus name is whatever the user typed, so the cell gets a clipped
 * name and the held-knob header gets the real one.
 */
/*
 * The four characters an enum-less mixer cell has room for.
 *
 * SPACES ARE DROPPED FIRST, and that is not tidying. A bus with no name of its
 * own is called "Bus <n>", so a plain slice(0, 4) gives every one of them
 * "Bus " — eight identical cells on a full slot, with nothing on the page to
 * tell them apart. "Bus1".."Bus8" costs the same four columns and says which.
 */
function mixerShortName(name) {
    return String(name || "").replace(/\s+/g, "").slice(0, 4);
}

export function busSendGridParams(config) {
    const out = [];
    /*
     * THE UNRESOLVED GUARD IS EXPLICIT AGAIN, and this time it is load-bearing.
     *
     * It used to be absent on the grounds that busListRows already answers no
     * rows for a read that did not complete, so a copy here was a guard no test
     * could kill. That reasoning ended when Main was added: Main is not a bus
     * and does not come from busListRows, so an unresolved config would now
     * declare exactly one fader and the mixer would draw -- claiming "this slot
     * has no buses", which is the tri-state mistake this file already documents
     * one screen up. Mutating this away is killable now.
     */
    if (!config || config.unresolved) return out;
    /* Buses, from the list (New Bus is not a fader), with MAIN AT THE HEAD.
     *
     * Main is "the rest of the slot" -- everything not routed into a bus -- and
     * it is the send source most slots have and many have alone. It was missing
     * here, which made a screen called the send mixer answer a question it could
     * not: turn every fader up and a slot with no buses still sends nothing.
     * Its levels are the same `buses:main_send<N>` keys Slot Settings wrote, so
     * this is a row appearing, not a value moving. */
    const rows = busListRows(config, undefined).filter((r) => r.kind === "bus");
    for (let send = 1; send <= BUS_SENDS; send++) {
        out.push({
            key: `main_send${send}`,
            name: "Main",
            short_name: "Main",
            type: "int", min: 0, max: SEND_LEVEL_MAX, step: 1, default: 0,
        });
        for (const row of rows) {
            out.push({
                key: sendGridKey(row, send),
                name: row.name,
                short_name: mixerShortName(row.name),
                type: "int", min: 0, max: SEND_LEVEL_MAX, step: 1, default: 0,
            });
        }
    }
    return out;
}

/**
 * The hierarchy: one level per send per KIND, and a root that CARRIES NO KNOBS.
 *
 * The planner names the walk root's grid page "Main" whatever the level
 * declares — deliberately, so 16 modules do not each open on their own word
 * for "where you land". That is the wrong name for half a mixer, and a root
 * holding Send A would have paged "Main / Send B" (a word that means the
 * planner's landing page here, and no longer a send destination). A root with
 * no keys emits no grid page at all, so the pages are the levels below it and
 * each is named for the send it is.
 *
 * THERE ARE NO VOICE LEVELS. Two more (`voice_a` / `voice_b`) lived here while
 * the host owned per-voice send levels; the module owns them now, so the mixer
 * is the buses again — at most SLOT_BUSES cells a page, which is why the
 * caller can pin it to one page per section.
 *
 * A level with no keys is OMITTED rather than emitted empty — a slot with no
 * buses gets the two voice pages and nothing else, and a module that cannot
 * split gets the two bus pages, which is exactly what shipped before.
 *
 * Answers null for an unresolved config. A read that did not complete is not
 * "this slot has no buses", and a contract built from one would draw a mixer
 * with only Main on it — a picture of a claim nothing made.
 *
 * ASSUMES BUS_SENDS === 2, deliberately and not by oversight. The levels are
 * named send_a / send_b and the split is a HALF, so a third send would build
 * one page of N and one of 2N. It is not generalised because the DSP side is
 * not: `BUS_MIX_SENDS` is 2 under a `_Static_assert`, and this file's own cap
 * test derives from it. If that ever moves, this function is rewritten rather
 * than parameterised in place — test_bus_model.sh's `[3, 3]` fails loudly
 * first, so it cannot ship quietly.
 */
export function busSendGridHierarchy(config) {
    if (!config || config.unresolved) return null;
    const params = busSendGridParams(config);
    const busKeys = params.map((p) => p.key);
    const half = (a) => [a.slice(0, a.length / BUS_SENDS), a.slice(a.length / BUS_SENDS)];
    const [busA, busB] = half(busKeys);

    const levels = { root: { label: "Send Mixer", knobs: [], params: [] } };
    /* NO PER-LEVEL `paginate` HERE, and that is not an omission: the planner
     * takes it once for the whole contract (planPages' `paginate` argument,
     * passed through the chrome), so a flag written on a level would be read by
     * nobody and would read as a promise the planner never made. The caller
     * decides — enterBusSendsGrid pins the mixer to one page only while it is
     * Main plus the buses, which is at most SLOT_BUSES + 1 cells. */
    const add = (id, label, keys) => {
        if (!keys.length) return;
        levels[id] = { label, knobs: keys, params: keys.map((k) => ({ key: k })) };
        levels.root.params.push({ level: id, label });
    };
    add("send_a", "Send A", busA);
    add("send_b", "Send B", busB);

    /* Nothing to ride at all is not a mixer. It is reachable: busListRows opens
     * the door when there is a bus OR a voice, and a bus can be deleted from
     * the very screen behind this one. */
    if (!levels.root.params.length) return null;
    return { modes: null, levels };
}

/* ==========================================================================
 * THE PRODUCER — the half of the file format that did not exist
 *
 * bus_parse_section (chain_patch.c) reads "buses" and "main_sends" out of a
 * saved slot document, and for a while NOTHING IN THE TREE WROTE THEM. That is
 * not "buses do not persist": patch_info_t is zeroed before the parse and the
 * section early-returns on a document without the key, so every bus arrived
 * `present == 0` and chain_bus_apply_patch RESET all four. Building a kit and
 * then loading any preset — or changing sets — destroyed it, mid-session and
 * silently. This function is the missing half, and
 * tests/host/test_chain_patch_roundtrip.sh now feeds what it emits to the real
 * C parser rather than to a hand-written fixture.
 * ========================================================================== */

/*
 * The `buses` / `main_sends` fields of a slot patch document, from a parsed
 * `buses:config` and a per-position state reader.
 *
 * `fxState(busIndex, fxIndex)` answers that insert's opaque state — an object,
 * a string, or undefined for "none". It is a callback because reading it is one
 * IPC round-trip per OCCUPIED position (~2.8 ms each) and only the caller knows
 * how to spend those; nothing here reads anything.
 *
 * Returns null for an unresolved config. A document built from a read that did
 * not complete would claim the slot has no buses, and the loader believes it.
 *
 * SHAPE, matched to bus_parse_section field by field:
 *
 *   { "main_sends": [a, b],
 *     "buses": [ {"present":0},                       <- a HOLE, never compacted
 *                {"present":1, "name":"Hats",
 *                 "voices":["chh","ohh"],
 *                 "sends":[20,0],
 *                 "fx":[{"module":"tapescam","bypassed":0,"state":{...}},
 *                       {"module":"","bypassed":0}]}, <- a hole IN the chain
 *                ... SLOT_BUSES entries ... ] }
 *
 * Key ORDER is load-bearing and not cosmetic. bus_field() finds the FIRST
 * occurrence of a key inside the object's span, and an insert's opaque state is
 * inside that span — so "name" must precede "fx", and inside an fx entry
 * "module" and "bypassed" must precede "state", or a module that happens to
 * store a key called "name" answers for the bus. `main_sends` is emitted before
 * `buses` for the same reason: bus_parse_section scans the WHOLE document for
 * it.
 */
export function busPatchFields(config, fxState) {
    if (!config || config.unresolved || !Array.isArray(config.buses)) return null;
    /* KEY ORDER, again: `main_sends` before `buses`, because bus_parse_section
     * scans the WHOLE document for it and only then walks the array. */
    /* NO `voice_sends`. Per-voice send levels are the module's own parameters
     * and travel in the synth's `state` blob, which this document already
     * carries — writing a second copy here would race the state load on the way
     * back in, and the loser would be a level the user cannot find. */
    const out = {
        main_sends: normaliseSends(config.mainSends),
        buses: [],
    };
    for (let b = 0; b < SLOT_BUSES; b++) {
        const bus = config.buses[b];
        if (!bus || !bus.present) { out.buses.push({ present: 0 }); continue; }
        /* Positional up to the last occupied insert, then stop: the tail is all
         * holes and the parser treats an absent entry exactly as an empty
         * module, so emitting eight `{"module":""}` objects per bus would be
         * four times the document for no information. */
        let last = -1;
        for (let k = 0; k < BUS_FX_SLOTS; k++)
            if (bus.fx && bus.fx[k] && bus.fx[k].module) last = k;
        const fx = [];
        for (let k = 0; k <= last; k++) {
            const e = (bus.fx && bus.fx[k]) || {};
            const entry = { module: String(e.module || ""), bypassed: e.bypassed ? 1 : 0 };
            if (entry.module) {
                const st = fxState ? fxState(b, k) : undefined;
                /* Absent, never null or "": the parser reads a `state` whose
                 * value is neither an object nor a string as no state at all,
                 * and writing one anyway would stage an empty blob over a
                 * running FX's parameters on the next load. */
                if (st !== undefined && st !== null && st !== "") entry.state = st;
            }
            fx.push(entry);
        }
        out.buses.push({
            present: 1,
            name: String(bus.name || `Bus ${b + 1}`),
            voices: (bus.voices || []).map(String),
            sends: normaliseSends(bus.sends),
            fx,
        });
    }
    return out;
}
