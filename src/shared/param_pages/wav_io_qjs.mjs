/*
 * The QuickJS file IO for wav_peaks.mjs, kept in a file of its own.
 *
 * `std` and `os` are QuickJS MODULES (registered by JS_NewCustomContext in
 * host/js_host_common.c). Importing them from wav_peaks.mjs would make that
 * file — and viz_draw.mjs, and therefore most of the renderer — unloadable
 * under node, which is where every host test runs. So the dependency is
 * injected, and this is the only file in the tree that names them.
 *
 * NOTHING under tests/ may import this. It is pulled in once, from
 * src/shadow/shadow_ui_param_pages.mjs, which is device-only already.
 *
 * host_read_file is deliberately NOT used: it slurps the whole file, and a
 * multi-megabyte read inside one 60 Hz tick is exactly the input lag
 * wav_peaks.mjs is streamed to avoid.
 */
import * as std from "std";
import * as os from "os";
import { setWavPeaksIO } from "./wav_peaks.mjs";

setWavPeaksIO({
    open(path) {
        const f = std.open(path, "rb");
        if (!f) return null;
        return {
            read: (buf, pos, len) => f.read(buf, pos, len),
            /* whence 0 = SEEK_SET, matching std.seek. */
            seek: (off, whence) => f.seek(off, whence),
            close: () => f.close(),
        };
    },
    stat(path) {
        /* os.stat returns [obj, errno]; a non-zero errno means no answer, and
         * a signature we cannot compute is what makes the cache re-read a file
         * that changed underneath it. */
        const st = os.stat(path);
        if (!st || st[1] !== 0 || !st[0]) return null;
        return { size: st[0].size || 0, mtime: st[0].mtime || 0 };
    },
});
