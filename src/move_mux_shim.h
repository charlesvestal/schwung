#pragma once
#include <stddef.h>

/*
 * move-mux-shim path-remap.
 *
 * Pure logic for rewriting filesystem paths under /data/UserData/ to a
 * per-instance subdirectory keyed by the MOVE_INSTANCE_ID environment
 * variable. Layered into MoveOriginal via LD_PRELOAD (Task 1.2) so two
 * MoveOriginal processes can share one device with independent UserData
 * trees.
 *
 * Returns 1 if remapped, 0 if unchanged, -1 if buffer too small.
 *
 * Notes:
 *   - This function is on a hot path (sets-browser polls UserLibrary every
 *     ~50ms). It does not allocate, lock, or call libc beyond getenv,
 *     strncmp, snprintf, strlen, strcpy.
 *   - Paths are NOT canonicalized. ".." and symlink resolution are out of
 *     scope; a thin prefix-rewrite is the contract.
 *   - MOVE_INSTANCE_ID is treated as opaque text; no validation is done.
 *     Callers control where it is set.
 *   - Idempotent: a path already under /data/UserData/move-<id>/ is left
 *     unchanged so wrapping a wrapped path does not double-remap.
 */
int mux_remap_path(const char *in, char *out, size_t out_sz);
