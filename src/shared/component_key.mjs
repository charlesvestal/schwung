/*
 * component_key.mjs — is this key a CHAIN COMPONENT's parameter?
 *
 * WHY IT EXISTS. The p-lock gesture converts a write made while a step is held
 * into a lock on that step. The test for "is this a component write" was
 * `fullKey.indexOf(":") > 0` -- which is true of every key the controller
 * sends, including its OWN control channel. `lanes:step_locks_query` is the
 * lock-map question the grid asks on a step press; it was being rewritten as
 * `lanes:plock_step` with target `lanes` and param `step_locks_query`, and the
 * translate that receives it marks the press SPENT. A spent press is never
 * replayed to Move, so the step never toggled its note.
 *
 * Reported from the device as "I tap a step with the grid up and no note
 * appears -- but if I tap five or six times it eventually works", the
 * intermittency being the map refreshing: when the query did not fire, the tap
 * got through.
 *
 * A QUESTION MUST NEVER BECOME AN EDIT. The rule below is the one the shim
 * already enforces (`shadow_component_param_split`, shadow_chain_mgmt.c):
 * `synth`, `fx<N>`, `midi_fx<N>` and nothing else. It lives here so the UI and
 * the shim can be pinned to the SAME rule by a test rather than by two people
 * remembering, and so that the next `lanes:`-prefixed key the grid needs
 * cannot resurrect this.
 */

/* The target half of a key, or "" if it has none. */
export function componentTargetOf(fullKey) {
    if (typeof fullKey !== "string") return "";
    const colon = fullKey.indexOf(":");
    if (colon <= 0) return "";
    return fullKey.substring(0, colon);
}

/* Exactly the shim's rule. `fx` and `midi_fx` must carry a number and nothing
 * else -- "fxAny" is not a component, and neither is a bare "fx". */
export function isComponentTarget(target) {
    if (typeof target !== "string" || target.length === 0) return false;
    if (target === "synth") return true;
    for (const prefix of ["midi_fx", "fx"]) {
        if (target.startsWith(prefix)) {
            const digits = target.substring(prefix.length);
            if (digits.length === 0) return false;
            return /^[0-9]+$/.test(digits);
        }
    }
    return false;
}

/* The whole question in one call: may a write to this key be converted into a
 * p-lock? */
export function isComponentParamKey(fullKey) {
    const target = componentTargetOf(fullKey);
    if (!target) return false;
    if (!isComponentTarget(target)) return false;
    /* A param half is required: "synth:" names no parameter. */
    return fullKey.length > target.length + 1;
}
