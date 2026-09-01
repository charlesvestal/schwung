/*
 * Snapshot / recall — the PURE half.
 *
 * Shift+Copy takes a snapshot of the whole rig, Shift+Delete puts it back.
 * The capture side is not here: it reuses `autosaveAllSlots()` +
 * `saveMasterFxChainConfig()` verbatim and copies the files they write into
 * the set's `snapshot/` directory, so there is no second serializer to keep
 * in step with the first. What IS here is everything the recall has to decide,
 * because deciding is the part that can be wrong silently.
 *
 * Nothing in this file does I/O or touches a global. It is imported by
 * shadow_ui.js and run directly by tests/host/test_snapshot_plan.sh.
 */

/*
 * The two shapes on disk. `slot_N.json` is the autosave wrapper —
 * `{ name, version, modified, chain: { synth, audio_fx[], midi_fx[] } }` —
 * and `master_fx_N.json` is one position, `{ id, path, state?, params? }`.
 *
 * Both are read for exactly two things per position: which module was there,
 * and its opaque `state` blob. Everything else in those files (paths, knob
 * mappings, channels, LFOs) describes the SHAPE of the rig, and a recall
 * deliberately does not restore shape — see planRestore.
 */

/* A component's `config` is either `{ state: <blob> }` or the older loose
 * params object. Only the first can be handed back as `<prefix>:state`. */
function stateOf(config) {
    if (!config || typeof config !== "object") return null;
    if (!("state" in config)) return null;
    const st = config.state;
    if (st === null || st === undefined) return null;
    /* Written as an object when it parsed as JSON, as a string when it did
     * not (key=value pairs). set_param wants the wire form of either. */
    return (typeof st === "string") ? st : JSON.stringify(st);
}

/* One position, in the form planRestore consumes. */
function record(prefix, moduleId, config, bypassed) {
    return {
        prefix,
        moduleId: moduleId || "",
        state: stateOf(config),
        bypassed: (bypassed === 1 || bypassed === "1" || bypassed === true) ? 1 : 0,
    };
}

/*
 * Parse one `slot_N.json` into positions.
 *
 * Returns [] for anything unparseable or empty — an absent or malformed file
 * is "this slot contributes nothing", which is exactly what a slot that was
 * empty at snapshot time should contribute. It is NOT an error the caller
 * needs to distinguish, because a slot with nothing in the snapshot has
 * nothing that could have failed to restore.
 */
export function parseSlotSnapshot(json) {
    let doc;
    try { doc = JSON.parse(json); } catch (e) { return []; }
    const chain = doc && doc.chain;
    if (!chain || typeof chain !== "object") return [];

    const out = [];
    if (chain.synth && chain.synth.module) {
        out.push(record("synth", chain.synth.module, chain.synth.config, chain.synth.bypassed));
    }
    /* MIDI FX and audio FX both spell their module id `type` and their state
     * `params`, where the synth uses `module` and `config` — an asymmetry in
     * the patch format that predates this and is not worth changing here. */
    const midi = Array.isArray(chain.midi_fx) ? chain.midi_fx : [];
    for (let i = 0; i < midi.length; i++) {
        const m = midi[i];
        if (!m || !m.type) continue;
        out.push(record(`midi_fx${i + 1}`, m.type, m.params, m.bypassed));
    }
    const fx = Array.isArray(chain.audio_fx) ? chain.audio_fx : [];
    for (let i = 0; i < fx.length; i++) {
        const f = fx[i];
        if (!f || !f.type) continue;
        out.push(record(`fx${i + 1}`, f.type, f.params, f.bypassed));
    }
    return out;
}

/* Parse one `master_fx_N.json`. `slotIdx` is 0-based; the param key is 1-based. */
export function parseMasterFxSnapshot(json, slotIdx) {
    let doc;
    try { doc = JSON.parse(json); } catch (e) { return []; }
    if (!doc || !doc.id) return [];
    /* The master position stores `state` at the TOP level, not under a
     * `config` key like a slot component does. Wrap it so stateOf sees the
     * shape it expects rather than teaching stateOf about two layouts. */
    return [record(`master_fx:fx${slotIdx + 1}`, doc.id, { state: doc.state }, doc.bypassed)];
}

/*
 * Decide what a recall writes.
 *
 * `records`  positions from the snapshot files, in write order.
 * `liveIds`  prefix -> module id loaded there RIGHT NOW ("" or absent when
 *            the position is empty).
 *
 * Returns `{ writes, skipped, reasons }`.
 *
 * A recall restores STATE, never SHAPE. It deliberately does not reload a
 * module that was swapped since the snapshot: `load_file` is what restores
 * identity, and it reinstantiates — cutting reverb tails and resetting arp
 * phase, which is the opposite of what an A/B gesture is for. A position
 * whose module changed is skipped and COUNTED.
 *
 * The count is the whole point. A partial restore that reports nothing is
 * indistinguishable from a working one until you notice by ear, which is the
 * failure that makes a gesture untrustworthy. Two different things get
 * counted here and both matter:
 *
 *   swapped   the module at this position is not the one snapshotted
 *   nostate   the module is right but the snapshot holds no state for it,
 *             because it implements no `state` key at all (`denis` and
 *             `branchage` are the known cases) — nothing to put back
 *
 * An EMPTY position is not a miss. A position empty in the snapshot has
 * nothing to restore, and one empty now was never going to receive anything.
 * Counting those would make every partly-filled rig report skips forever and
 * the number would stop meaning anything.
 */
export function planRestore(records, liveIds) {
    const writes = [];
    const reasons = [];
    const live = liveIds || {};

    for (const r of records) {
        if (!r || !r.moduleId) continue;            /* empty in snapshot */
        const now = live[r.prefix] || "";
        if (!now) { reasons.push({ prefix: r.prefix, reason: "empty" }); continue; }
        if (now !== r.moduleId) {
            reasons.push({ prefix: r.prefix, reason: "swapped",
                           was: r.moduleId, now });
            continue;
        }
        if (r.state === null) {
            reasons.push({ prefix: r.prefix, reason: "nostate", was: r.moduleId });
            continue;
        }
        writes.push({ prefix: r.prefix, state: r.state, bypassed: r.bypassed });
    }

    /* "empty" is a position that held a module in the snapshot and holds
     * nothing now — the user removed it. That IS a miss: something was
     * snapshotted and did not come back. It is separated from "swapped" only
     * so the log can say which. */
    const skipped = reasons.length;
    return { writes, skipped, reasons };
}

/*
 * The toast / announce line. One string, so the OLED and the screen reader
 * cannot disagree about what just happened.
 */
export function recallMessage(skipped) {
    return (skipped > 0)
        ? ["Snapshot restored", `${skipped} skipped`]
        : ["Snapshot restored"];
}
