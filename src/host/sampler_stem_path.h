/* sampler_stem_path.h - deriving a stem's filename from the master's.
 *
 * A header, and a static inline, for the same reason recall_quantize.h and
 * transport_grid.h are: this is the one piece of the stems feature that is
 * pure string arithmetic with edge cases, and it has TWO callers on opposite
 * sides of the subsystem — sampler_worker_open_stems (a take being armed) and
 * skipback_writer_func (a rolling save). One fact with two consumers and no
 * shared home is how both ends of a rule drift apart.
 *
 * The rule: "<dir>/<base>.wav" + "Slot1" -> "<dir>/<base>_Slot1.wav".
 *
 * The split is on the LAST '.' AFTER the last '/', which is what keeps a set
 * name containing a dot ("my.set_20260902.wav") intact and stops a dot in a
 * DIRECTORY name ("/data/UserData/v0.9/song") from being mistaken for an
 * extension — a master path with no extension of its own would otherwise be
 * cut in the middle of its directory. */

#ifndef SAMPLER_STEM_PATH_H
#define SAMPLER_STEM_PATH_H

#include <stdio.h>
#include <string.h>

static inline void sampler_stem_path_build(char *out, size_t out_len,
                                           const char *master, const char *suffix) {
    if (!out || out_len == 0) return;
    if (!master || !suffix) { out[0] = '\0'; return; }

    const char *slash = strrchr(master, '/');
    const char *name = slash ? slash + 1 : master;
    const char *dot = strrchr(name, '.');

    /* A leading dot is not an extension — ".wav" as a whole filename is a
     * hidden file called "wav", and splitting it produces "/_Slot1.wav" with
     * the name gone. */
    if (dot == name) dot = NULL;

    if (dot) {
        int stem_len = (int)(dot - master);
        snprintf(out, out_len, "%.*s_%s%s", stem_len, master, suffix, dot);
    } else {
        snprintf(out, out_len, "%s_%s.wav", master, suffix);
    }
}

#endif /* SAMPLER_STEM_PATH_H */
