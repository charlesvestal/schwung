/*
 * Unit test for mux_remap_path — Task 1.1 of the dual-MoveOriginal-instances
 * plan (docs/plans/2026-04-27-dual-move-instances.md).
 *
 * Compiles and runs on the host (macOS dev machine), not the device. No
 * cross-compile required — pure logic.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/move_mux_shim.h"

int main(void) {
    char out[512];

    /* Case 1: no instance set -> unchanged. */
    setenv("MOVE_INSTANCE_ID", "", 1);
    assert(mux_remap_path("/data/UserData/foo", out, sizeof(out)) == 0);
    assert(strcmp(out, "/data/UserData/foo") == 0);

    /* Case 2: instance "a" -> /data/UserData/move-a/foo. */
    setenv("MOVE_INSTANCE_ID", "a", 1);
    assert(mux_remap_path("/data/UserData/foo", out, sizeof(out)) == 1);
    assert(strcmp(out, "/data/UserData/move-a/foo") == 0);

    /* Case 3: path outside UserData -> unchanged. */
    assert(mux_remap_path("/etc/hosts", out, sizeof(out)) == 0);
    assert(strcmp(out, "/etc/hosts") == 0);

    /* Case 4: already-remapped path -> unchanged (idempotent). */
    assert(mux_remap_path("/data/UserData/move-a/foo", out, sizeof(out)) == 0);
    assert(strcmp(out, "/data/UserData/move-a/foo") == 0);

    /* Case 5: buffer too small -> returns -1. */
    char tiny[8];
    assert(mux_remap_path("/data/UserData/foo", tiny, sizeof(tiny)) == -1);

    printf("OK\n");
    return 0;
}
