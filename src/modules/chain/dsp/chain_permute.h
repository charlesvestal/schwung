/*
 * Renumbering a chain section WITHOUT unloading anything.
 *
 * WHY THIS EXISTS. Adding, removing or reordering a module used to be
 * expressed to the DSP as a sequence of `<id>:module` writes, and every such
 * write unloads the position and dlopen()s a fresh instance. So inserting a
 * MIDI FX at the head destroyed and rebuilt every MIDI FX behind it, and
 * removing a mid-chain audio FX did the same to everything downstream. The
 * editor papered over that by carrying each module's opaque `:state` blob, its
 * modulation base and its LFO routing across the renumber — but a state blob is
 * not an instance. A running arpeggiator loses its phase; a reverb or delay
 * loses the tail that was ringing. Reported on hardware as "we can't change
 * phase of a running module by just adding a new one".
 *
 * Renumbering is an ARRAY PERMUTATION, not a teardown. The instance keeps
 * running; only its index changes. This header is that permutation, kept pure
 * and dependency-free (like chain_key_index.h beside it) so it can be compiled
 * and RUN natively by tests/host rather than only verified by ear on hardware.
 *
 * WHAT MAKES IT SAFE. A chain position is described by a dozen parallel arrays
 * — the dlopen handle, the vtable, the instance pointer, the module name, the
 * param metadata, the bypass flag, and so on. Anything a permutation MISSES
 * becomes a module driving another module's parameters, which is far worse than
 * the bug being fixed. So this takes the arrays as DATA (a list of base/stride
 * pairs) rather than naming any of them, the caller enumerates them once, and
 * a test pins that enumeration against the struct definition.
 */
#ifndef CHAIN_PERMUTE_H
#define CHAIN_PERMUTE_H

#include <stddef.h>
#include <string.h>

#include "chain_key_index.h"

/* One per-position array: `base` is element 0, `elem` the stride. */
typedef struct {
    void *base;
    size_t elem;
} chain_perm_array_t;

/* Largest per-position element this will shift. The biggest today is
 * param_smoother_t (~1.2 KB); the metadata blocks are behind pointers
 * precisely so they are 8 bytes here. A larger one is REFUSED rather than
 * truncated — this runs in the audio callback and the scratch is a stack
 * buffer, so growing silently is not an option. */
#define CHAIN_PERM_MAX_ELEM 2048

/* Positions the map can describe. Matches the section caps. */
#define CHAIN_PERM_MAX_POS 16

static inline void *chain_perm_at(const chain_perm_array_t *a, int i) {
    return (char *)a->base + (size_t)i * a->elem;
}

/* Every array must be movable with the single-element scratch below. */
static inline int chain_perm_arrays_ok(const chain_perm_array_t *a, int n) {
    if (!a || n <= 0) return 0;
    for (int i = 0; i < n; i++) {
        if (!a[i].base || a[i].elem == 0 || a[i].elem > CHAIN_PERM_MAX_ELEM) return 0;
    }
    return 1;
}

/* map[old] = new, identity to start. -1 means "no longer in the chain". */
static inline void chain_perm_map_identity(int *map, int count) {
    for (int i = 0; i < count && i < CHAIN_PERM_MAX_POS; i++) map[i] = i;
}

/*
 * Open a hole at `at`, shifting [at, count) up one. Returns the new count, or
 * -1 if refused.
 *
 * The hole is ZEROED, which is what makes it a legal chain position rather than
 * a copy of its neighbour: both the audio and the MIDI walk test the plugin and
 * instance pointers per position and SKIP a false one (they never stop at it),
 * so a chain with a hole in it renders correctly until the caller loads
 * something there.
 */
static inline int chain_perm_insert(chain_perm_array_t *a, int n,
                                    int count, int cap, int at, int *map) {
    if (!chain_perm_arrays_ok(a, n) || !map) return -1;
    if (count < 0 || cap <= 0 || cap > CHAIN_PERM_MAX_POS) return -1;
    if (count >= cap) return -1;               /* full */
    if (at < 0 || at > count) return -1;       /* `== count` is an append */
    for (int i = 0; i < n; i++) {
        if (count > at) {
            memmove(chain_perm_at(&a[i], at + 1), chain_perm_at(&a[i], at),
                    (size_t)(count - at) * a[i].elem);
        }
        memset(chain_perm_at(&a[i], at), 0, a[i].elem);
    }
    chain_perm_map_identity(map, count);
    for (int i = at; i < count; i++) map[i] = i + 1;
    return count + 1;
}

/*
 * Take position `at` out and CLOSE THE GAP, shifting (at, count) down one.
 * Returns the new count, or -1 if refused.
 *
 * The caller must already have destroyed whatever occupied `at` — this moves
 * pointers, it does not own lifetimes. The vacated LAST position is zeroed so
 * a stale duplicate of the module that used to be there cannot be reached by a
 * count that has not shrunk yet.
 */
static inline int chain_perm_remove(chain_perm_array_t *a, int n,
                                    int count, int at, int *map) {
    if (!chain_perm_arrays_ok(a, n) || !map) return -1;
    if (count <= 0 || count > CHAIN_PERM_MAX_POS) return -1;
    if (at < 0 || at >= count) return -1;
    for (int i = 0; i < n; i++) {
        if (count - at - 1 > 0) {
            memmove(chain_perm_at(&a[i], at), chain_perm_at(&a[i], at + 1),
                    (size_t)(count - at - 1) * a[i].elem);
        }
        memset(chain_perm_at(&a[i], count - 1), 0, a[i].elem);
    }
    chain_perm_map_identity(map, count);
    map[at] = -1;
    for (int i = at + 1; i < count; i++) map[i] = i - 1;
    return count - 1;
}

/*
 * Move position `from` to position `to`, rotating everything between them.
 * Returns the count (unchanged), or -1 if refused.
 *
 * `from == to` is refused rather than treated as a no-op so a caller cannot
 * quietly issue an announcement for a move that did not happen.
 */
static inline int chain_perm_move(chain_perm_array_t *a, int n,
                                  int count, int from, int to, int *map) {
    if (!chain_perm_arrays_ok(a, n) || !map) return -1;
    if (count <= 0 || count > CHAIN_PERM_MAX_POS) return -1;
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) return -1;
    unsigned char scratch[CHAIN_PERM_MAX_ELEM];
    for (int i = 0; i < n; i++) {
        memcpy(scratch, chain_perm_at(&a[i], from), a[i].elem);
        if (to > from) {
            memmove(chain_perm_at(&a[i], from), chain_perm_at(&a[i], from + 1),
                    (size_t)(to - from) * a[i].elem);
        } else {
            memmove(chain_perm_at(&a[i], to + 1), chain_perm_at(&a[i], to),
                    (size_t)(from - to) * a[i].elem);
        }
        memcpy(chain_perm_at(&a[i], to), scratch, a[i].elem);
    }
    chain_perm_map_identity(map, count);
    map[from] = to;
    if (to > from) { for (int i = from + 1; i <= to; i++) map[i] = i - 1; }
    else           { for (int i = to; i < from; i++) map[i] = i + 1; }
    return count;
}

/*
 * Follow a position id ("fx2", "midi_fx1") through the permutation, IN PLACE.
 *
 * Modulation targets, LFO targets and knob mappings all name a position by this
 * string, so a permutation that moved the arrays and not these would silently
 * re-aim every routing at whatever slid into the position it named — the chain
 * sounds different afterwards in a way the user did not ask for and cannot see.
 *
 * Returns 1 when the id was rewritten, 0 when it names something this
 * permutation does not touch (another section, the synth, an LFO), and -1 when
 * it named a position that is GONE — the caller clears that routing, because
 * there is nowhere for it to point.
 *
 * One pass over the map, never a sequence of single renames: shifting 1 to 2
 * and then 2 to 3 would rewrite the same id twice and land it two places away.
 */
static inline int chain_perm_retarget(char *id, size_t id_len,
                                      const char *prefix, int max,
                                      const int *map, int count) {
    if (!id || !prefix || !map || id_len == 0) return 0;
    int at = chain_fx_index_from_id(id, prefix, max);
    if (at < 0) return 0;               /* not this section */
    if (at >= count) return 0;          /* outside what moved */
    int now = map[at];
    if (now < 0) { id[0] = '\0'; return -1; }
    if (now == at) return 0;
    chain_fx_component_id(id, id_len, prefix, now);
    return 1;
}

#endif /* CHAIN_PERMUTE_H */
