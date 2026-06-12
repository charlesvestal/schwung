/*
 * Shadow shared-memory helper.
 *
 * One helper replacing the repeated shm_open/ftruncate/mmap/memset blocks
 * in schwung_shim.c (init_shadow_shm) and shadow_ui.c (open_shadow_shm).
 */
#ifndef SHADOW_SHM_UTIL_H
#define SHADOW_SHM_UTIL_H

#include <stddef.h>

/*
 * Map a POSIX shared-memory segment read/write.
 *
 *   create = 1: shm_open(O_CREAT | O_RDWR, 0666), ftruncate to `size`
 *               (checked — a full /dev/shm fails here cleanly instead of
 *               SIGBUS-ing on first page touch later), mmap RW, and
 *               optionally zero the mapping when `zero` is set.
 *   create = 0: shm_open(O_RDWR) + mmap RW (attach to existing segment).
 *
 * The fd is closed after mmap — the mapping stays valid without it and no
 * caller uses the fd post-init.
 *
 * Returns the mapping, or NULL on failure. Failures are logged once via
 * unified_log, except a missing segment on attach (create=0, ENOENT):
 * callers may be in a retry loop waiting for the producer, so that case
 * stays quiet and simply returns NULL.
 */
void *shadow_shm_map(const char *name, size_t size, int create, int zero);

#endif /* SHADOW_SHM_UTIL_H */
