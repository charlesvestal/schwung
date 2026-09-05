/*
 * bus_mix.h — per-voice bus routing and mixing arithmetic.
 *
 * Header-only and dependency-free for the same reason as master_fx_key.h and
 * chain_key_index.h: so tests/host can compile and RUN it natively. Its caller
 * is chain_host.c's v2_render_block, a translation unit that cannot be built on
 * the dev machine, which is how arithmetic like this ships untested.
 *
 * THE ALIASING IS THE FEATURE. Two voices in one bus are handed the SAME
 * pointer, so their sum happens in the module's own accumulating render with no
 * mixing pass of ours at all, and a voice in no bus is handed the main buffer
 * so the sparse case costs literally nothing. Callers must therefore clear only
 * the DISTINCT buffers (bus_mix_active_mask) and modules must ACCUMULATE.
 *
 * Every function here runs on the SCHED_FIFO SPI callback: no allocation, no
 * I/O, no locks.
 */
#ifndef BUS_MIX_H
#define BUS_MIX_H

#include <stdint.h>
#include <stddef.h>

/* voice_bus[] entry meaning "this voice is not on any bus". */
#define BUS_MIX_MAIN (-1)

/* Send levels are 0..127 so they survive a MIDI CC round trip unchanged and
 * need no float in the audio path. 127 is exactly unity, not 127/128. */
#define BUS_MIX_SEND_LEVEL_MAX 127

/*
 * How many global send buses a slot can feed.
 *
 * IT LIVES HERE, not in shadow_chain_mgmt.h, because BOTH sides need it and
 * only one of them may include that header: the chain is a MODULE, dlopen'd
 * through plugin_api_v2, and a module reaching into a shim header is exactly
 * the coupling that produced breakbeat's ABI drift. bus_mix.h is the one
 * header both the chain and the shim include. shadow_chain_mgmt.h defines
 * SEND_BUSES from this and static-asserts they agree, so there is one number
 * with one definition and a build failure if that stops being true. */
#define BUS_MIX_SENDS 2

/*
 * Build the per-voice output table.
 *
 * voice_bus[i] is BUS_MIX_MAIN or a bus index. Entries alias deliberately.
 * A bus that is out of range, or whose buffer is NULL because it has not been
 * allocated yet (bus chains are allocated on demand, off the RT thread), falls
 * back to main_buf — the voice is still heard, just not separately.
 */
static inline void bus_mix_build_table(int16_t **voice_out, int n_voices,
                                       const int8_t *voice_bus,
                                       int16_t *main_buf,
                                       int16_t *const *bus_buf, int n_buses)
{
    for (int i = 0; i < n_voices; i++) {
        int b = voice_bus ? voice_bus[i] : BUS_MIX_MAIN;
        voice_out[i] = (b >= 0 && b < n_buses && bus_buf && bus_buf[b])
                     ? bus_buf[b] : main_buf;
    }
}

/*
 * Bitmask of the buses at least one voice targets — the set that must be
 * cleared before an accumulating render. Returns how many bits are set.
 *
 * Clearing by this mask rather than clearing all n_buses is what keeps an
 * unused bus free: an allocated-but-unrouted bus is never touched per frame.
 */
static inline int bus_mix_active_mask(const int8_t *voice_bus, int n_voices,
                                      int n_buses, uint32_t *out_mask)
{
    uint32_t m = 0;
    int n = 0;
    for (int i = 0; i < n_voices; i++) {
        int b = voice_bus ? voice_bus[i] : BUS_MIX_MAIN;
        if (b >= 0 && b < n_buses && !(m & (1u << b))) {
            m |= 1u << b;
            n++;
        }
    }
    if (out_mask) *out_mask = m;
    return n;
}

static inline int16_t bus_mix_clamp(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* dst += src, saturating. n is SAMPLES (frames * 2), not frames. */
static inline void bus_mix_accumulate(int16_t *dst, const int16_t *src, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = bus_mix_clamp((int32_t)dst[i] + (int32_t)src[i]);
}

/*
 * dst += src * (level / BUS_MIX_SEND_LEVEL_MAX), saturating.
 *
 * Level 0 returns without touching dst — a send at zero must cost nothing,
 * because most buses feed most sends at zero.
 */
static inline void bus_mix_send(int16_t *dst, const int16_t *src, int n, int level)
{
    if (level <= 0) return;
    if (level > BUS_MIX_SEND_LEVEL_MAX) level = BUS_MIX_SEND_LEVEL_MAX;
    for (int i = 0; i < n; i++) {
        int32_t scaled = ((int32_t)src[i] * (int32_t)level) / BUS_MIX_SEND_LEVEL_MAX;
        dst[i] = bus_mix_clamp((int32_t)dst[i] + scaled);
    }
}

#endif /* BUS_MIX_H */
