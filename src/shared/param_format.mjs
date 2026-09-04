/**
 * param_format.mjs — single source of truth for converting (rawValue, meta)
 * → display string, and (rawValue, meta) → wire string for set_param.
 *
 * Replaces the scattered formatters in shadow_ui.js and shadow_ui_patches.mjs.
 *
 * Recognized `meta.unit` values (declared by modules in chain_params):
 *   "dB"  — signed, decimals from step                    → "-6.0 dB"
 *   "Hz"  — non-negative, decimals from step; auto kHz    → "440 Hz" / "1.50 kHz" if >=1000
 *   "ms"  — non-negative                                  → "12.5 ms"
 *   "sec" — non-negative                                  → "1.234 sec"
 *   "%"   — values in 0..1 scaled x100, otherwise raw     → "50%"
 *   "st"  — semitones, signed integer                     → "+7 st" / "-3 st" / "0 st"
 *   "BPM" — integer                                       → "120 BPM"
 *   (any other string) — appended verbatim with a space
 *
 * `meta.display_format` (printf-style ".Nf" or ".N%") wins over unit logic.
 */

export function precisionForStep(step, fallback = 2) {
    const s = Math.abs(Number(step));
    if (!isFinite(s) || s <= 0) return fallback;
    if (s >= 1)     return 0;
    if (s >= 0.1)   return 1;
    if (s >= 0.01)  return 2;
    if (s >= 0.001) return 3;
    return 4;
}

/**
 * @param {string} fmt   printf-style ".Nf" or ".N%"
 * @param {number} num   the raw param value
 * @param {string} [unit] meta.unit — a leading "%" in `fmt` is a PRINTF sigil
 *                 (".0f" formats a float, it does not mean "as a percent"),
 *                 so an "f"-type format carries no scaling of its own. A
 *                 param whose fraction 0..1 is meant to read as a percentage
 *                 says so via `unit: "%"`, not via the format string — a
 *                 ".0f" format on such a param must still scale by 100 or it
 *                 prints the raw fraction rounded to an integer (0.232 -> "0",
 *                 0.823 -> "1"), which is indistinguishable from a real value
 *                 near zero without ever looking wrong until you compare it
 *                 to the knob.
 */
export function applyDisplayFormat(fmt, num, unit) {
    const match = String(fmt).match(/^%?\.(\d{1,2})(f|%)$/);
    if (!match) return null;
    const decimals = parseInt(match[1], 10);
    if (match[2] === "%" || unit === "%") return (num * 100).toFixed(decimals) + "%";
    return num.toFixed(decimals);
}

function fmtPercent(num, meta) {
    const max = (meta && typeof meta.max === "number") ? meta.max : 1;
    const display = (max <= 1) ? num * 100 : num;
    /* Default % to 0 decimals unless step is sub-1%-of-the-displayed-range. */
    let decimals = 0;
    if (meta && typeof meta.step === "number" && meta.step > 0) {
        if (max <= 1 && meta.step < 0.01) decimals = precisionForStep(meta.step) - 2;
        else if (max > 1 && meta.step < 1) decimals = precisionForStep(meta.step);
    }
    return display.toFixed(decimals) + "%";
}

function fmtSemitones(num) {
    const n = Math.round(num);
    if (n > 0) return "+" + n + " st";
    return n + " st";
}

function fmtHz(num, meta) {
    const decimals = precisionForStep(meta && meta.step, 0);
    if (Math.abs(num) >= 1000) {
        return (num / 1000).toFixed(2) + " kHz";
    }
    return num.toFixed(decimals) + " Hz";
}

export function formatParamValue(rawValue, meta) {
    if (!meta) {
        const num = Number(rawValue);
        return isFinite(num) ? num.toFixed(2) : String(rawValue);
    }
    if (meta.type === "enum" && Array.isArray(meta.options)) {
        const idx = Math.round(Number(rawValue));
        if (idx >= 0 && idx < meta.options.length) return meta.options[idx];
        return String(rawValue);
    }

    const num = Number(rawValue);
    if (!isFinite(num)) return String(rawValue);

    if (meta.display_format) {
        const out = applyDisplayFormat(meta.display_format, num, meta.unit);
        if (out !== null) {
            /* applyDisplayFormat already adds % when format ends in %; otherwise
               append the meta unit (skipping % since the format itself injects it). */
            if (out.endsWith("%")) return out;
            return meta.unit && meta.unit !== "%" ? out + " " + meta.unit : out;
        }
    }

    if (meta.type === "int" && !meta.unit) {
        return String(Math.round(num));
    }

    const unit = meta.unit;
    if (unit === "dB")  return num.toFixed(precisionForStep(meta.step, 1)) + " dB";
    if (unit === "Hz")  return fmtHz(num, meta);
    if (unit === "ms")  return num.toFixed(precisionForStep(meta.step, 1)) + " ms";
    if (unit === "sec") return num.toFixed(precisionForStep(meta.step, 3)) + " sec";
    if (unit === "%")   return fmtPercent(num, meta);
    if (unit === "st")  return fmtSemitones(num);
    if (unit === "BPM") return Math.round(num) + " BPM";

    if (meta.type === "int") return String(Math.round(num)) + (unit ? " " + unit : "");

    const decimals = precisionForStep(meta.step);
    return num.toFixed(decimals) + (unit ? " " + unit : "");
}

/* The two conventions a plugin's set_param can speak for an enum. */
export const WIRE_NAME = "name";
export const WIRE_INDEX = "index";

/**
 * Learn which of the two an enum's plugin speaks, from a value the PLUGIN
 * reported, and latch it onto the metadata.
 *
 * The problem this solves: `chord_set_param` is a strcmp ladder over the option
 * NAMES with no trailing else, so a numeric index is silently discarded — the
 * value never moves while the UI renders the index it just invented, and the
 * user watches it change and snap back. Plenty of plugins are the other way
 * round (an atoi()) and plenty accept both. Nothing in the contract obliges an
 * author to say which, and until now the knob grid guessed "index" for
 * everyone unless a module declared `options_as_string`.
 *
 * The two enum writers in shadow_ui.js never needed a declaration — they ask
 * what the plugin currently REPORTS and answer in kind:
 *
 *     const pluginUsesIndex = (ctx.meta.options.indexOf(currentVal) < 0);
 *
 * This is the same question, asked once per key and remembered, because the
 * grid — unlike those two — holds the enum as a number end to end and would
 * otherwise have to re-derive it from a value it wrote itself.
 *
 * TWO RULES, and both are load-bearing:
 *
 *   `raw` MUST have come from the device. The grid's own writes populate
 *   `s.values`; learning from that cache means the first index write teaches
 *   it "this plugin uses indices" for the rest of the session, which is a
 *   verdict that makes itself true and looks right in a one-detent test.
 *
 *   A value that is NEITHER a declared option nor a number teaches nothing.
 *   That is the one place this deliberately diverges from `pluginUsesIndex`,
 *   which treats any non-name as an index: those two recompute per write and
 *   self-correct, this one latches, so an unrecognised reading leaves the
 *   question open for the next read rather than locking in an answer derived
 *   from a value neither convention explains.
 *
 * An explicit `options_as_string: true` is an OVERRIDE and is never learned
 * over — a module that declares its convention has said the last word.
 *
 * @returns {string|null} the latched convention, or null if still unknown
 */
export function learnEnumWireFormat(meta, raw) {
    if (!meta || meta.type !== "enum" || !Array.isArray(meta.options)) return null;
    if (meta.options_as_string) return WIRE_NAME;
    if (meta.wire_format) return meta.wire_format;
    if (raw === null || raw === undefined) return null;
    const s = String(raw);
    if (s === "" || s.trim() === "") return null;
    if (meta.options.indexOf(s) >= 0 || meta.options.indexOf(s.trim()) >= 0) {
        meta.wire_format = WIRE_NAME;
        return WIRE_NAME;
    }
    if (isFinite(Number(s.trim()))) {
        meta.wire_format = WIRE_INDEX;
        return WIRE_INDEX;
    }
    return null;
}

/** True when this enum should be written as its option NAME. */
export function enumWiresNames(meta) {
    return !!meta && (!!meta.options_as_string || meta.wire_format === WIRE_NAME);
}

/**
 * The wire value for picking option `index`, in whatever convention this
 * plugin speaks — the one call an option PICKER should make.
 *
 * A picker is not a knob: it does not carry a running numeric state, it hands
 * you an index straight out of a list. The temptation is therefore to write
 * `String(index)`, which is precisely the write chord silently discards. So the
 * detection is not optional here, and `currentRaw` — the value the PLUGIN last
 * reported — is what settles it, exactly as learnEnumWireFormat requires.
 *
 * Pass `currentRaw` undefined when the format is already latched (or declared);
 * the learn step is a no-op then.
 */
export function enumWireValue(meta, index, currentRaw) {
    learnEnumWireFormat(meta, currentRaw);
    return formatParamForSet(index, meta);
}

/* Wire-format value for set_param (no unit suffix; numeric strings only). */
export function formatParamForSet(rawValue, meta) {
    if (!meta) {
        const num = Number(rawValue);
        return isFinite(num) ? num.toFixed(3) : String(rawValue);
    }
    if (meta.type === "int") return String(Math.round(Number(rawValue)));
    if (meta.type === "enum") {
        /*
         * A NUMBER IS AN INDEX. A NAME IS A NAME. The two are not interchangeable
         * and the option text cannot arbitrate between them.
         *
         * This used to try `options.indexOf(String(rawValue))` FIRST, for any
         * value. Every caller in this repo hands over `knobStep()` output — an
         * index, a number — so an index whose numeral happened to appear among
         * the options was resolved as that OPTION'S POSITION instead:
         *
         *   options ["-100","-50","0","+50","+100"], user picks -100 (index 0)
         *   -> indexOf("0") is 2 -> the module is told 2, i.e. "0"
         *
         * Measured over tests/fixtures/module-contracts.json, 40 enum params in
         * the fleet write a wrong value that way — minijv's four lfo1offsets,
         * v_octave, transpose, the chord_pc family — and it is SILENT, because
         * the cell goes on showing the value the grid believes it wrote. An
         * enum of ["0","1"] is unharmed only by the coincidence that there the
         * text equals the index.
         *
         * The label lookup itself is not wrong; its PRECEDENCE was. It exists
         * for enums a plugin wires by name, and `enumWiresNames` is the
         * declared signal for exactly that — so a string is read as a label
         * only when the plugin actually speaks names, and a number is always
         * an index.
         */
        let idx;
        const wiresNames = enumWiresNames(meta);
        if (Array.isArray(meta.options) && typeof rawValue === "string" && wiresNames) {
            const labelIdx = meta.options.indexOf(rawValue);
            idx = labelIdx >= 0 ? labelIdx : Math.round(Number(rawValue));
        } else {
            idx = Math.round(Number(rawValue));
        }
        if (!isFinite(idx) || idx < 0) idx = 0;
        if (enumWiresNames(meta) && Array.isArray(meta.options) &&
            idx < meta.options.length) {
            return meta.options[idx];
        }
        return String(idx);
    }
    const decimals = Math.max(3, precisionForStep(meta.step));
    return Number(rawValue).toFixed(decimals);
}
