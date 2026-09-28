/*
 * cc_map.mjs -- the GENERIC CC MAP: any controller's CC drives a parameter.
 * The second user of the control foundation (control_target / control_map /
 * control_host); design in
 * docs/superpowers/specs/2026-09-26-custom-surface-layout-design.md.
 *
 * Bindings live in the per-set control document (`cc`): one per (channel, cc),
 * each with a target and a mode --
 *   abs   the CC's 0-127 spans the parameter's range (an enum by option)
 *   rel   a relative encoder (1..63 up, 65..127 down) steps it through the
 *         same knob engine as a Move knob
 *
 * WHO OWNS A CC is decided before this module sees it: the shim hands over
 * only CCs a binding owns (and swallows them from Move), or every CC while
 * LEARNING -- and never one an active remote surface claims (cc_claim.h). So
 * feed() can trust that a bound CC is ours.
 *
 * WRITES ARE COALESCED: a controller sends a CC per few ms; the latest value
 * per binding is written ONCE per tick(), never per message -- a parameter
 * write is an IPC round trip.
 *
 * LEARN is a MODE, parameter first, as in a DAW: while it is on, the last
 * parameter moved on Move is the one being learned (the shared learn broker,
 * re-armed after every capture), and the next controller CC moved binds to it
 * -- replacing whatever that CC or that parameter was bound to before. Then
 * move another parameter and another CC, as many as you like, without
 * re-arming. The footer says where it is (`learnFooter`). The CC half is ours:
 * the shim publishes every CC while `setShimLearn(true)`. It ends by the CC
 * Map's Stop Learn, or after CC_LEARN_IDLE_MS with nothing moved.
 */
import { knobInit, knobStep } from "./knob_engine.mjs";
import { enumIndexOf, KIND_ENUM } from "./param_pages/param_meta.mjs";
import { formatParamForSet, learnEnumWireFormat } from "./param_format.mjs";
import { bindCC } from "./control_map.mjs";
import { sameTarget } from "./control_target.mjs";

/* Learn mode ends by itself after this long with nothing moved. */
export const CC_LEARN_IDLE_MS = 120000;
/* A relative binding idle this long re-reads its value before stepping, so a
 * change made elsewhere (Move's grid) is where it continues from. */
export const REL_RESEED_MS = 2000;

/* Signed ticks for a relative CC value (the E16's and most encoders' form). */
export function relativeTicks(v) {
    if (v <= 0 || v > 127 || v === 64) return 0;
    return v < 64 ? v : v - 128;
}

/* An absolute 0-127 value as the parameter's engine value (an enum's index). */
export function absValue(v, meta) {
    const f = Math.max(0, Math.min(127, v)) / 127;
    if (meta && meta.kind === KIND_ENUM && Array.isArray(meta.options) && meta.options.length) {
        return Math.round(f * (meta.options.length - 1));
    }
    const lo = meta && typeof meta.min === "number" ? meta.min : 0;
    const hi = meta && typeof meta.max === "number" ? meta.max : 1;
    const x = lo + f * (hi - lo);
    return meta && meta.type === "int" ? Math.round(x) : x;
}

export function createCCMap(io) {
    const o = io || {};
    const controls = o.controls;
    const edit = o.edit || (() => null);
    const targets = o.targets;
    const broker = o.learn || null;
    const setShimLearn = o.setShimLearn || (() => {});
    /* Learn reports each step ON SCREEN (the host's overlay), not only by
     * speech: it is armed while you are elsewhere, finding the parameter. */
    const announce = o.announce || (() => {});
    const notify = o.notify || ((title, text) => announce(title + ": " + text));
    const now = o.now || (() => Date.now());

    const bindings = () => (controls().cc || []);
    const key = (ch, cc) => ch + ":" + cc;
    const find = (ch, cc) => bindings().find((b) => b.channel === ch && b.cc === cc) || null;

    /* Latest value owed per binding, flushed once per tick. */
    const pending = new Map();
    /* Relative bindings step from a knob state seeded by one read. */
    const knobStates = new Map();
    /* Absolute enum bindings whose wire form has been read. */
    const wireKnown = new Set();

    /* ---- learn ---- */
    let learning = null;   /* { target | null, at } -- at: the last activity */
    const owner = { id: "ccmap" };

    function endLearn(title, text) {
        if (!learning) return;
        learning = null;
        setShimLearn(false);
        if (broker) broker.cancel(owner);
        if (title) notify(title, text || "");
    }

    /* The broker fires ONCE per arm, so it is re-armed after every capture:
     * that is what makes learn a mode rather than a one-shot. */
    function armBroker() {
        if (!broker || !learning) return;
        broker.arm(owner, (target) => {
            if (!learning) return;
            if (!target) { endLearn(); return; }   /* another learn took over */
            learning.target = target;
            learning.at = now();
            armBroker();
        }, true);
    }

    function beginLearn() {
        if (learning) endLearn();
        learning = { target: null, at: now() };
        setShimLearn(true);
        armBroker();
        announce("CC Learn on: move a parameter, then a control");
    }

    /* The CC a target is bound to, or null. */
    function bindingOf(target) {
        return bindings().find((b) => sameTarget(b.target, target)) || null;
    }

    /* Bind (ch, cc) to the target being learned. One CC per parameter: a
     * second CC brushed while learning MOVES the binding rather than adding a
     * duplicate, as in a DAW. Returns true if the document changed. */
    function learnBind(ch, cc) {
        const target = learning.target;
        const cur = find(ch, cc);
        if (cur && sameTarget(cur.target, target)) return false;
        edit((doc) => {
            const kept = Object.assign({}, doc, { cc: (doc.cc || []).filter((b) => !sameTarget(b.target, target)) });
            return bindCC(kept, { channel: ch, cc, mode: cur ? cur.mode : "abs", target });
        });
        knobStates.delete(key(ch, cc));
        wireKnown.delete(key(ch, cc));
        announce((target.label || target.key) + ": CC" + cc);
        return true;
    }

    function writeBinding(b, engineValue, meta) {
        const wire = formatParamForSet(engineValue, meta);
        pending.set(key(b.channel, b.cc), { b, wire });
    }

    return {
        /**
         * A [status, cc, value] from the external port. True if it was ours.
         * `claimed`: an active remote surface owns this message (its own
         * encoders). Such a message is never bound and never learned -- the
         * ownership order, applied on this side too, because the surface's
         * CCs still reach the UI (for the surface) under E16 / EC4.
         */
        feed(status, cc, value, claimed) {
            if ((status & 0xF0) !== 0xB0 || claimed) return false;
            const ch = status & 0x0F;
            if (learning) {
                learning.at = now();
                /* No parameter yet: nothing to bind to. The footer says so. */
                if (!learning.target) return true;
                /* The binding message itself moves nothing -- an absolute
                 * knob would otherwise jump the parameter on the first touch. */
                if (learnBind(ch, cc)) return true;
            }
            const b = find(ch, cc);
            if (!b) return false;
            if (targets.status(b.target) !== "live") return true;   /* dark: owned, inert */
            const meta = targets.metaOf(b.target);
            if (!meta || meta.readOnly) return true;
            if (b.mode === "rel") {
                const ticks = relativeTicks(value);
                if (!ticks) return true;
                const k = key(ch, cc);
                let st = knobStates.get(k);
                if (st && now() - (st.touchedAt || 0) >= REL_RESEED_MS) st = null;
                if (!st) {
                    const raw = targets.read(b.target);
                    if (raw !== null && raw !== undefined) learnEnumWireFormat(meta, raw);
                    let start;
                    if (meta.kind === KIND_ENUM) { const i = enumIndexOf(meta, raw); start = i >= 0 ? i : 0; }
                    else { const n = Number(raw); start = isFinite(n) ? n : 0; }
                    st = knobInit(start);
                    knobStates.set(k, st);
                }
                st.touchedAt = now();
                const dir = ticks > 0 ? 1 : -1;
                let v = st.value;
                for (let i = 0; i < Math.abs(ticks); i++) v = knobStep(st, meta, dir, now(), false);
                writeBinding(b, v, meta);
                return true;
            }
            /* An enum is written in the module's OWN wire form (name or
             * index), which is learned from what it answers -- the absolute
             * path never reads otherwise, and wrote "2" to a module that
             * takes "Tri". One read, the first time a binding is used. */
            if (meta.kind === KIND_ENUM && !wireKnown.has(key(ch, cc))) {
                const raw = targets.read(b.target);
                if (raw !== null && raw !== undefined) { learnEnumWireFormat(meta, raw); wireKnown.add(key(ch, cc)); }
            }
            writeBinding(b, absValue(value, meta), meta);
            return true;
        },

        /** Flush the owed writes (one per binding) and expire a stale learn. */
        tick(t) {
            const at = t === undefined ? now() : t;
            if (learning && at - learning.at >= CC_LEARN_IDLE_MS) endLearn("CC Learn", "ended (idle)");
            if (!pending.size) return;
            for (const { b, wire } of pending.values()) targets.write(b.target, wire);
            pending.clear();
        },

        /** The (channel, cc) pairs the shim must hand over and swallow. */
        claimPairs() { return bindings().map((b) => [b.channel, b.cc]); },

        beginLearn,
        cancelLearn() { endLearn(); announce("CC Learn off"); },
        get learning() { return learning ? { target: learning.target } : null; },

        /**
         * What the footer says while learn mode is on, as "Learn: <action>",
         * or null when it is off:
         *   Learn: move a param     nothing chosen yet
         *   Learn: Cutoff > CC?     a parameter chosen, no CC bound to it
         *   Learn: Cutoff: CC18     bound (Ch2 CC18 off channel 1)
         * `fits(action)` is the caller's pixel measure; the name is shortened
         * until the action fits.
         */
        learnFooter(fits) {
            if (!learning) return null;
            const t = learning.target;
            if (!t) return "Learn: move a param";
            const b = bindingOf(t);
            const tail = !b ? " > CC?" : ": " + (b.channel ? "Ch" + (b.channel + 1) + " " : "") + "CC" + b.cc;
            /* The NAME gives way, never the CC: the footer drops a hint that
             * does not fit rather than clipping it. `fits` measures. */
            let name = String(t.label || t.key);
            const ok = fits || (() => true);
            while (name.length > 1 && !ok(name + tail)) name = name.slice(0, -1);
            return "Learn: " + name + tail;
        },
        /** The document changed: relative knob states may name old targets. */
        reload() { knobStates.clear(); pending.clear(); wireKnown.clear(); },
    };
}
