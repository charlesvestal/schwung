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

export function applyDisplayFormat(fmt, num) {
    const match = String(fmt).match(/^%?\.(\d{1,2})(f|%)$/);
    if (!match) return null;
    const decimals = parseInt(match[1], 10);
    if (match[2] === "%") return (num * 100).toFixed(decimals) + "%";
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
        const out = applyDisplayFormat(meta.display_format, num);
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

/* Wire-format value for set_param (no unit suffix; numeric strings only). */
export function formatParamForSet(rawValue, meta) {
    if (!meta) {
        const num = Number(rawValue);
        return isFinite(num) ? num.toFixed(3) : String(rawValue);
    }
    if (meta.type === "int") return String(Math.round(Number(rawValue)));
    if (meta.type === "enum") {
        let idx;
        if (Array.isArray(meta.options)) {
            const labelIdx = meta.options.indexOf(String(rawValue));
            idx = labelIdx >= 0 ? labelIdx : Math.round(Number(rawValue));
        } else {
            idx = Math.round(Number(rawValue));
        }
        if (!isFinite(idx) || idx < 0) idx = 0;
        if (meta.options_as_string && Array.isArray(meta.options) &&
            idx < meta.options.length) {
            return meta.options[idx];
        }
        return String(idx);
    }
    const decimals = Math.max(3, precisionForStep(meta.step));
    return Number(rawValue).toFixed(decimals);
}
