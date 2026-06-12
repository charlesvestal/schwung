/*
 * Shadow shared-memory helper — see shadow_shm_util.h.
 *
 * Not realtime-safe (shm_open/mmap/unified_log). Never call from the SPI
 * callback path.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shadow_shm_util.h"
#include "unified_log.h"

void *shadow_shm_map(const char *name, size_t size, int create, int zero)
{
    int oflags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    int fd = shm_open(name, oflags, 0666);
    if (fd < 0) {
        /* On attach, ENOENT just means the producer hasn't created the
         * segment yet — callers may be retrying, so stay quiet. */
        if (!(create == 0 && errno == ENOENT)) {
            unified_log("shm", LOG_LEVEL_ERROR, "shm_open(%s) failed: %s",
                        name, strerror(errno));
        }
        return NULL;
    }
    if (create && ftruncate(fd, (off_t)size) != 0) {
        /* A full /dev/shm fails here; without this check the first page
         * touch on the mapping SIGBUSes instead of failing cleanly. */
        unified_log("shm", LOG_LEVEL_ERROR, "ftruncate(%s, %zu) failed: %s",
                    name, size, strerror(errno));
        close(fd);
        return NULL;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        unified_log("shm", LOG_LEVEL_ERROR, "mmap(%s, %zu) failed: %s",
                    name, size, strerror(errno));
        return NULL;
    }
    if (zero)
        memset(map, 0, size);
    return map;
}
