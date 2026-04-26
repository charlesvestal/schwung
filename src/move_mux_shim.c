/*
 * move-mux-shim path-remap pure logic.
 *
 * See move_mux_shim.h for the contract. This file is the unit-testable
 * core; LD_PRELOAD wrappers (open/openat/stat/access/...) layer on top of
 * it in Task 1.2.
 *
 * Hot-path constraints: no allocation, no locks, libc usage limited to
 * getenv, strncmp, snprintf, strlen, strcpy. The sets browser polls
 * UserLibrary every ~50ms idle and ~170ms interactive; this routine runs
 * on every poll.
 */

#include "move_mux_shim.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char USERDATA_PREFIX[] = "/data/UserData/";

int mux_remap_path(const char *in, char *out, size_t out_sz) {
    if (!in || !out || out_sz == 0) return -1;

    const char *id = getenv("MOVE_INSTANCE_ID");

    /* No instance configured -> passthrough. */
    if (!id || !*id) {
        if (strlen(in) >= out_sz) return -1;
        strcpy(out, in);
        return 0;
    }

    const size_t plen = sizeof(USERDATA_PREFIX) - 1;

    /* Outside /data/UserData/ -> passthrough. */
    if (strncmp(in, USERDATA_PREFIX, plen) != 0) {
        if (strlen(in) >= out_sz) return -1;
        strcpy(out, in);
        return 0;
    }

    /* Idempotency: if the path is already under /data/UserData/move-<id>/,
     * leave it alone so wrapping a wrapped path does not double-remap. */
    char already[64];
    int n = snprintf(already, sizeof(already), "/data/UserData/move-%s/", id);
    if (n > 0 && (size_t)n < sizeof(already) && strncmp(in, already, (size_t)n) == 0) {
        if (strlen(in) >= out_sz) return -1;
        strcpy(out, in);
        return 0;
    }

    int written = snprintf(out, out_sz, "/data/UserData/move-%s/%s",
                           id, in + plen);
    if (written < 0 || (size_t)written >= out_sz) return -1;
    return 1;
}
