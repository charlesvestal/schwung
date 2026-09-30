/*
 * Duplicating a set: what of its Schwung state the copy carries.
 *
 * EVERYTHING, minus a short list of names that must NOT travel. This used to
 * be the other way round -- a list of what to copy (slot_N, master_fx_N,
 * controls.json, shadow_chain_config.json) -- and every per-set file added
 * after it was written was silently left behind: step chance, automation
 * lanes, scenes, the send FX chains and send levels. A duplicate kept its
 * synths and lost the rest, with nothing logged. A list of what to copy is
 * correct only on the day it is written; a list of what NOT to copy stays
 * correct while new state is added.
 *
 * Copying is safe for the position-keyed state: a duplicate holds the same
 * clips at the same grid positions. Move renumbers notes on load, which lanes
 * absorb (a fingerprint mismatch re-stamps) and chance absorbs (a restore
 * unbinds ids and the page follow adopts by position).
 */

/* Never copied:
 *   snapshot/        Shift+Copy's take is re-seeded from the set on every
 *                    load; a duplicate gets its own (docs/SHADOW_UI.md).
 *   copy_source.txt  names where THIS dir came from -- the source's own
 *                    would overwrite the duplicate's. */
export const SET_STATE_COPY_EXCLUDE = ["snapshot", "copy_source.txt"];

/* The one shell command (for host_system_cmd, whose allowlist takes "sh ")
 * that copies every top-level entry of `srcDir` into `dstDir` except the
 * excluded names. null for a path the command could not quote safely --
 * set_state dirs are UUIDs, so anything else is a caller bug, not a set. */
export function setStateCopyCommand(srcDir, dstDir) {
    const safe = (p) => typeof p === "string" && p.length > 0 && /^[A-Za-z0-9_./-]+$/.test(p);
    if (!safe(srcDir) || !safe(dstDir)) return null;
    const skip = SET_STATE_COPY_EXCLUDE.join("|");
    return "sh -c 'for f in " + srcDir + "/* " + srcDir + "/.[!.]*; do " +
           "[ -e \"$f\" ] || continue; " +
           "case \"${f##*/}\" in " + skip + ") continue;; esac; " +
           "cp -a \"$f\" " + dstDir + "/ || exit 1; done'";
}
