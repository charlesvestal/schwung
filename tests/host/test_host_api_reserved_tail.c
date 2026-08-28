/* host_api_v1_t's reserved tail: a module that reads PAST the last real field
 * must find NULL, not the next object in memory.
 *
 * Why this is worth a test rather than a comment. Every module guards its host
 * calls as `if (host->fn) host->fn()`. That is sound only while a read inside
 * the struct is the only read that can happen — and it isn't. A module linked
 * from object files compiled against two revisions of plugin_api_v1.h resolves
 * the same call at two different offsets, and the larger one runs off the end.
 *
 * breakbeat 0.2.x does exactly that: get_bpm() at +88 from bb_render_block and
 * at +120 from bb_create_instance. A chain sub-plugin's struct is
 * chain_instance_t::subplugin_host_api, so +120 read the NEXT MEMBER of that
 * heap instance — non-NULL, so the guard passed, and the indirect call jumped
 * into the heap. SIGSEGV on the SPI callback at module load, which takes
 * MoveOriginal down and boot-loops the device if the slot is restored.
 *
 * The assertions below are about GEOMETRY, deliberately. Asserting "the
 * reserved member exists" passes with a one-pointer tail that leaves +120 off
 * the end again — which is the failure, not a smaller version of it.
 */

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "plugin_api_v1.h"

static int failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL: ");                                                 \
            printf(__VA_ARGS__);                                              \
            printf("\n      (%s:%d: %s)\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* The offset the crashing breakbeat build reads. Named rather than inlined so
 * a future reader can tell it is an OBSERVED value, not a round number. */
#define BREAKBEAT_OVERREAD_OFFSET 120

/* How far past the last real field an over-read must still land on NULL. One
 * mismatched field is 8 bytes; several revisions of drift is what we are
 * actually insuring against. */
#define MIN_RESERVED_BYTES 64

int main(void) {
    /* Zeroed the way every real instance is: mm_init memsets its container,
     * shadow_host_api is BSS, overtake_host_api is a static, and chain_host
     * memcpy's sizeof() into a calloc'd instance member. */
    host_api_v1_t api;
    memset(&api, 0, sizeof(api));

    const size_t reserved_off = offsetof(host_api_v1_t, reserved);
    const size_t total = sizeof(host_api_v1_t);

    CHECK(total - reserved_off >= MIN_RESERVED_BYTES,
          "reserved tail is %zu bytes, want >= %d — shrinking it puts "
          "over-reads back off the end of the struct",
          total - reserved_off, MIN_RESERVED_BYTES);

    /* reserved must be LAST. A field appended after it is outside the NULL run
     * and inherits the original bug, silently. */
    CHECK(reserved_off + sizeof(api.reserved) == total,
          "reserved is not the final member (ends at %zu, struct is %zu) — "
          "append new fields by consuming reserved from the front instead",
          reserved_off + sizeof(api.reserved), total);

    /* The specific offset that crashed must be inside the struct AND NULL. */
    CHECK(BREAKBEAT_OVERREAD_OFFSET + sizeof(void *) <= total,
          "offset %d is still past the end of host_api_v1_t (%zu bytes)",
          BREAKBEAT_OVERREAD_OFFSET, total);

    if (BREAKBEAT_OVERREAD_OFFSET + sizeof(void *) <= total) {
        void *over;
        memcpy(&over, (const char *)&api + BREAKBEAT_OVERREAD_OFFSET,
               sizeof(over));
        CHECK(over == NULL,
              "read at +%d is %p, not NULL — a module's `if (host->fn)` guard "
              "will pass and call it",
              BREAKBEAT_OVERREAD_OFFSET, over);
    }

    /* Sweep the whole tail, not just the one offset we happen to know about.
     * A test pinned to 120 alone passes for a tail that starts at 120. */
    for (size_t off = reserved_off; off + sizeof(void *) <= total;
         off += sizeof(void *)) {
        void *p;
        memcpy(&p, (const char *)&api + off, sizeof(p));
        CHECK(p == NULL, "reserved slot at +%zu is %p, not NULL", off, p);
    }

    /* Every real field still precedes the tail — this is what makes the sweep
     * above a statement about padding rather than about live callbacks. */
    CHECK(offsetof(host_api_v1_t, get_beat_position) < reserved_off,
          "get_beat_position is not before the reserved tail");

    if (failures == 0) {
        printf("PASS: host_api_v1_t reserved tail (%zu bytes at +%zu, "
               "struct %zu)\n",
               total - reserved_off, reserved_off, total);
        return 0;
    }
    printf("\n%d check(s) failed\n", failures);
    return 1;
}
