/**
 * file_flat.mjs -- the decisions behind a TURNED file cell (the flat list of
 * the current file's folder). PURE: the host owns the reads and the drawing.
 *
 * The list opens on the loaded file and a release (or 1 s idle) commits the
 * row under the cursor. A one-detent brush must therefore never load
 * anything: a module may load a whole sample or kit inside set_param, and the
 * user never chose files[0] -- they touched the knob once.
 */

/*
 * current  the cell's read: string, or null when the read did not complete
 * files    [{ path, label }] in the folder the browser state resolved
 *
 * { refuse: "unreadable" }  the read failed -- a null is not "no file", and
 *                           opening on start_path would offer the wrong folder
 * { refuse: "empty" }       nothing to list
 * { stored, index }         stored is -1 when the current file is not listed
 */
export function planFileFlatOpen(current, files) {
    if (current === null || current === undefined) return { refuse: "unreadable" };
    const list = files || [];
    if (!list.length) return { refuse: "empty" };
    const stored = list.findIndex((f) => f && f.path === String(current));
    return { stored, index: stored >= 0 ? stored : 0 };
}

/* The file to load, or null. Only a cursor the user MOVED picks anything,
 * and landing back on the loaded file is not a change. */
export function fileFlatPick(files, index, stored, moved) {
    if (!moved || index === stored) return null;
    const f = (files || [])[index];
    return f && f.path ? f : null;
}

/*
 * A refused open repeats on every detent of the same spin -- a blocking
 * read, a directory scan and a spoken announcement each time. The latch
 * swallows the rest of that spin: same key, within windowMs of the LAST
 * detent (each swallowed detent extends it).
 */
export function createRefusalLatch(windowMs = 1000) {
    let key = null;
    let at = 0;
    return {
        /** True when this detent belongs to a spin already refused. */
        latched(k, now) {
            if (k === key && now - at < windowMs) { at = now; return true; }
            return false;
        },
        refuse(k, now) { key = k; at = now; },
        clear() { key = null; at = 0; },
    };
}
