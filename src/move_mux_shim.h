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

/*
 * Resolve `in` to a path suitable for handing to a libc function:
 *   - If `in` does not need remapping (NULL, no instance, outside
 *     /data/UserData/, already remapped), returns `in` verbatim.
 *   - If remap succeeds into `scratch`, returns `scratch`.
 *   - If remap would overflow `scratch`, increments an internal counter
 *     and falls back to `in` (we never fail a syscall just because the
 *     remap buffer was too small — better to operate on the original
 *     path than to error out spuriously).
 *
 * `scratch` and `scratch_sz` are caller-provided so this stays
 * allocation-free and reentrant. `scratch_sz == 0` is treated as "no
 * scratch available" and returns `in`.
 *
 * Used by the LD_PRELOAD wrappers in this same file (Task 1.2) and is
 * separately unit-testable via the host harness.
 */
const char *mux_resolve(const char *in, char *scratch, size_t scratch_sz);

/* Test/debug accessor for the remap-overflow counter. */
unsigned long mux_remap_overflow_count(void);
