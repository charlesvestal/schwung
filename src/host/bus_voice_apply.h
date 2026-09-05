/*
 * bus_voice_apply.h — apply a bus's STORED VOICE IDS to the render-time
 * voice -> bus map, counting the ids that no longer resolve.
 *
 * Header-only and dependency-free for the same reason as bus_route.h: its one
 * production caller lives in chain_bus.c, a translation unit that dlopens
 * plugins and cannot be built on the dev machine, which is how arithmetic like
 * this ships untested.
 *
 * WHY IDS AND NOT INDICES. A bus config stores the module's own voice ids, so
 * a module that adds a voice in a later version does not re-point every
 * existing bus by shifting the indices under it. The cost is that an id can
 * legitimately stop resolving — the module dropped that voice, or the slot now
 * holds a different module entirely.
 *
 * AN ORPHAN IS COUNTED AND RETAINED, NEVER RE-POINTED. The stored id stays in
 * the bus's list (this file never writes it) so it comes back if the module
 * does; only the derived map is left alone. And the count is returned rather
 * than swallowed, because a partial restore that reports nothing is
 * indistinguishable from a working one — the same rule the snapshot/recall
 * skipped-position count exists for.
 *
 * Pure: no allocation, no I/O, no locks. Called on the SPI callback.
 */
#ifndef BUS_VOICE_APPLY_H
#define BUS_VOICE_APPLY_H

#include <stdint.h>
#include "bus_route.h"
#include "bus_mix.h"

/*
 * Point every voice this bus claims at `bus`, and answer how many of its
 * stored ids did not resolve.
 *
 * `ids` / `n_ids` is the module's flat declared voice list (the index into it
 * IS the render-buffer index — see chain_instance_t::synth_split_voice_ids).
 * `stored` / `n_stored` is the bus's saved id list. `voice_bus` is the derived
 * map, `voice_bus_len` its capacity.
 *
 * ONLY WRITES ENTRIES IT RESOLVES. It never clears, so the caller rebuilds by
 * resetting the whole map to BUS_MIX_MAIN first and then applying every bus in
 * turn — see chain_reset_voice_bus. Clearing here instead would make the
 * result depend on the order the buses are applied in.
 *
 * A NULL/empty stored id is skipped and NOT counted as an orphan: it is an
 * empty slot in the list, not a voice that went missing. bus_voice_index makes
 * the same distinction from the other side.
 */
static inline int bus_voice_apply(const char *const *ids, int n_ids,
                                  const char *const *stored, int n_stored,
                                  int bus, int8_t *voice_bus, int voice_bus_len)
{
    int orphans = 0;
    if (!stored || n_stored <= 0 || !voice_bus || voice_bus_len <= 0) return 0;
    if (bus < 0 || bus > INT8_MAX) return 0;
    for (int s = 0; s < n_stored; s++) {
        const char *id = stored[s];
        if (!id || id[0] == '\0') continue;
        int idx = bus_voice_index(ids, n_ids, id);
        /* The map is shorter than the id list only if a caller passed a
         * mismatched pair; treat it exactly as a miss rather than writing past
         * the end of the map on the audio thread. */
        if (idx < 0 || idx >= voice_bus_len) { orphans++; continue; }
        voice_bus[idx] = (int8_t)bus;
    }
    return orphans;
}

#endif /* BUS_VOICE_APPLY_H */
