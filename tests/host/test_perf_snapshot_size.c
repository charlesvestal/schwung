/* Unit test: the /schwung-perf container can only grow.
 *
 * The failure this guards against is the CONTROL_BUFFER_SIZE one: a buffer
 * sized to `sizeof` with an equality assert reads as a hard limit, so the next
 * person to need a field squeezes it into spare bits of an existing one
 * instead. It costs nothing to have headroom (tmpfs allocates by page), so the
 * asserts are written to fail only on a SHRINK.
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "perf_snapshot.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

int main(void)
{
    /* The version check must be readable off a segment shorter than the whole
     * struct — otherwise the check itself is the thing that SIGBUSes. */
    CHECK(offsetof(schwung_perf_snapshot_t, magic) == 0,
          "magic must be at offset 0");
    CHECK(offsetof(schwung_perf_snapshot_t, version) == 4,
          "version must be at offset 4");
    CHECK(offsetof(schwung_perf_snapshot_t, seq) == 8,
          "seq must follow version - the Go mirror assumes it");

    CHECK(sizeof(schwung_perf_snapshot_t) <= SCHWUNG_PERF_SHM_SIZE,
          "struct must fit its segment");

    /* Headroom is the point. If this ever fails the container is being sized to
     * fit rather than to leave room, which is the mistake this test exists to
     * prevent. 256 bytes is roughly 32 more uint64 fields. */
    CHECK(SCHWUNG_PERF_SHM_SIZE - sizeof(schwung_perf_snapshot_t) >= 256,
          "the segment must keep at least 256 bytes of headroom so adding a "
          "field is free - size the container, not the struct");

    CHECK(SCHWUNG_PERF_SHM_SIZE >= 4096,
          "segment must be at least one page");

    /* Pin the slot counts against the real headers. If MASTER_FX_SLOTS is
     * raised and this header is not, the page silently reports half the
     * chain — the same failure test_master_fx_slot_routing guards. */
    CHECK(PERF_CHAIN_SLOTS == 4,
          "PERF_CHAIN_SLOTS must match SHADOW_CHAIN_INSTANCES");
    CHECK(PERF_MASTER_FX_SLOTS == 8,
          "PERF_MASTER_FX_SLOTS must match MASTER_FX_SLOTS in "
          "src/host/shadow_chain_mgmt.h");

    if (fails) {
        fprintf(stderr, "test_perf_snapshot_size: %d failure(s)\n", fails);
        return 1;
    }
    printf("PASS test_perf_snapshot_size\n");
    return 0;
}
