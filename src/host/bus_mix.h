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
 * PER-VOICE SENDS ARE A SUPERSET LAID OVER THAT, not a replacement for it: a
 * voice that asks for one is handed its own buffer (bus_mix_solo_mask,
 * bus_mix_build_table_split) and folded back afterwards, and a voice that does
 * not is aliased exactly as above. bus_mix_active_mask is deliberately
 * UNCHANGED by the partition — a solo voice's bus still has to be cleared and
 * still runs its inserts, because that is where the voice's audio ends up.
 *
 * Every function here runs on the SCHED_FIFO SPI callback: no allocation, no
 * I/O, no locks.
 *
 * THE MODULE CONTRACT FOR move_plugin_render_split (a dlsym'd symbol, not a
 * field on plugin_api_v2_t — see chain_host.c's v2_load_synth): a module
 * offering per-voice render is switched between it and render_block AT
 * RUNTIME, PER FRAME, by whether the slot's synth currently has any voice
 * assigned to a bus. Assigning a single voice on the shadow UI flips the
 * module's active entry point mid-stream with no reload, so both must be
 * state-compatible — same voice allocator, same envelope/LFO/phase state.
 * render_split ACCUMULATES into voice_out[] (the caller clears destinations
 * first), the opposite of render_block's overwrite, and must never write more
 * than the `frames` argument's worth of samples into any voice_out[] entry —
 * those pointers alias the shared bus buffers.
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

/* The mask returned by bus_mix_active_mask is a uint32_t, so it can name at
 * most 32 buses no matter how many a caller claims. A bus at or past this
 * index is EXCLUDED, not truncated onto another: its voices fall back to
 * main_buf and its bit stays clear, which is the same answer the NULL-buffer
 * case gives. Excluding beats undefined behaviour in the shift below, and
 * n_buses is a runtime int, so no compile-time assert on SLOT_BUSES can
 * guard it. */
#define BUS_MIX_MAX_BUSES 32

/*
 * The same ceiling for VOICES, and for the same reason: bus_mix_solo_mask
 * returns a uint32_t. A voice at or past this index is EXCLUDED from the solo
 * partition — it renders straight into its bus or into main, exactly as every
 * voice did before per-voice sends existed, so the failure is "its send is not
 * heard", never a shift into somebody else's buffer. chain_internal.h
 * static-asserts SPLIT_VOICES_MAX <= this, so on the real path the exclusion
 * is unreachable; it is here because n_voices is a runtime int.
 */
#define BUS_MIX_MAX_VOICES 32

/*
 * How many global send buses a slot can feed.
 *
 * IT LIVES HERE, not in shadow_chain_mgmt.h, because BOTH sides need it and
 * only one of them may include that header: the chain is a MODULE, dlopen'd
 * through plugin_api_v2, and a module reaching into a shim header is exactly
 * the coupling that produced breakbeat's ABI drift. bus_mix.h is the one
 * header both the chain and the shim include. shadow_chain_mgmt.h defines
 * SEND_BUSES from this name and static-asserts the two agree, so the shim
 * cannot grow a third send bus the chain does not know about. */
#define BUS_MIX_SENDS 2

/*
 * The one resolve. bus_mix_build_table and bus_mix_active_mask must agree on
 * what a voice's target IS, or the clear set names a buffer nobody rendered
 * into — and a caller that trusts the mask memsets a NULL on the SPI
 * callback. Returns bus_buf[b] only when b is in range (and below
 * BUS_MIX_MAX_BUSES, so the mask's shift stays defined) and the bus has
 * actually been allocated; NULL otherwise, meaning "the voice's audio went
 * to main_buf".
 */
static inline int16_t *bus_mix_target(int b, int n_buses, int16_t *const *bus_buf)
{
    return (b >= 0 && b < n_buses && b < BUS_MIX_MAX_BUSES && bus_buf && bus_buf[b])
         ? bus_buf[b] : NULL;
}

/*
 * Where voice `i`'s audio BELONGS — its bus buffer, or main.
 *
 * Distinct from where it is RENDERED, which is the same thing only for a voice
 * with no per-voice send. A solo-buffered voice renders into its own buffer and
 * is then accumulated into this, so the two answers must come from one place or
 * a voice's audio lands somewhere its sends were not taken from.
 *
 * A bus that is out of range, or whose buffer is NULL because it has not been
 * allocated yet (bus chains are allocated on demand, off the RT thread), falls
 * back to main_buf — the voice is still heard, just not separately.
 */
static inline int16_t *bus_mix_voice_dest(int i, const int8_t *voice_bus,
                                          int16_t *main_buf,
                                          int16_t *const *bus_buf, int n_buses)
{
    int b = voice_bus ? voice_bus[i] : BUS_MIX_MAIN;
    int16_t *t = bus_mix_target(b, n_buses, bus_buf);
    return t ? t : main_buf;
}

/*
 * ================= THE SOLO PARTITION =======================================
 *
 * A per-voice send has to be scaled per voice, and the aliasing above makes
 * that impossible for a voice sharing a bus buffer: by the time the chain sees
 * that buffer the module has already summed them into it. So a voice that
 * actually asks for one — and ONLY such a voice — is handed its own buffer out
 * of a pool, its sends are taken from that, and its audio is then accumulated
 * into the destination it would have had. Everything downstream is unchanged.
 *
 * The rule is therefore exactly: **a voice is solo-buffered iff it has a
 * non-zero per-voice send level.** All-zero is the sparse case and must stay
 * byte-for-byte what it was — aliased, no pool slot cleared, no extra pass —
 * which is what keeps buses cheap and is what test_bus_mix.c pins by running
 * the two build paths against each other.
 *
 * `voice_send` is a FLAT [n_voices][n_sends] table of 0..127 levels; passing
 * NULL means no voice has one.
 */
static inline int bus_mix_voice_is_solo(const int8_t *voice_send, int n_voices,
                                        int n_sends, int i)
{
    if (!voice_send || n_sends <= 0) return 0;
    if (i < 0 || i >= n_voices || i >= BUS_MIX_MAX_VOICES) return 0;
    for (int s = 0; s < n_sends; s++)
        if (voice_send[(size_t)i * (size_t)n_sends + (size_t)s] > 0) return 1;
    return 0;
}

/*
 * Bitmask of the voices that need their own buffer this frame. Returns how many
 * bits are set — a caller uses that to decide whether the split render is worth
 * entering at all when no bus is routed, which is dr32's whole case: 32 pads on
 * main, each with its own send, and not one bus between them.
 */
static inline int bus_mix_solo_mask(const int8_t *voice_send, int n_voices,
                                    int n_sends, uint32_t *out_mask)
{
    uint32_t m = 0;
    int n = 0;
    for (int i = 0; i < n_voices && i < BUS_MIX_MAX_VOICES; i++) {
        if (bus_mix_voice_is_solo(voice_send, n_voices, n_sends, i)) {
            m |= 1u << i;
            n++;
        }
    }
    if (out_mask) *out_mask = m;
    return n;
}

/*
 * Build the per-voice output table.
 *
 * voice_bus[i] is BUS_MIX_MAIN or a bus index. Entries alias deliberately:
 * voices sharing a bus get the SAME pointer and sum inside the module's own
 * accumulating render.
 *
 * A voice named by `solo_mask` is handed `solo_pool + i * solo_stride` instead.
 * THE POOL IS INDEXED BY VOICE INDEX AND NEVER COMPACTED — a packed pool would
 * need a second index the caller then has to carry to the send tap and to the
 * fold-back, and an index that means two things is the defect this branch has
 * already paid for once. A slot per voice costs 512 bytes of a 16 KB inline
 * array; a wrong pointer costs somebody's kick drum.
 *
 * `solo_mask` is IGNORED when no pool is supplied, so the failure of a caller
 * that computed a mask without a buffer is today's behaviour rather than a
 * store through NULL on the SPI callback.
 */
static inline void bus_mix_build_table_split(int16_t **voice_out, int n_voices,
                                             const int8_t *voice_bus,
                                             int16_t *main_buf,
                                             int16_t *const *bus_buf, int n_buses,
                                             uint32_t solo_mask,
                                             int16_t *solo_pool, int solo_stride)
{
    if (!solo_pool || solo_stride <= 0) solo_mask = 0;
    for (int i = 0; i < n_voices; i++) {
        if (i < BUS_MIX_MAX_VOICES && (solo_mask & (1u << i)))
            voice_out[i] = solo_pool + (size_t)i * (size_t)solo_stride;
        else
            voice_out[i] = bus_mix_voice_dest(i, voice_bus, main_buf, bus_buf, n_buses);
    }
}

/*
 * The sparse case, spelled as what it is: the split build with an EMPTY solo
 * mask. Defined in terms of it rather than beside it so the two cannot drift —
 * "a voice with no per-voice send routes exactly as it did before" is then a
 * property of the code and not a claim in a comment.
 */
static inline void bus_mix_build_table(int16_t **voice_out, int n_voices,
                                       const int8_t *voice_bus,
                                       int16_t *main_buf,
                                       int16_t *const *bus_buf, int n_buses)
{
    bus_mix_build_table_split(voice_out, n_voices, voice_bus, main_buf,
                              bus_buf, n_buses, 0u, NULL, 0);
}

/*
 * Bitmask of the buses at least one voice actually renders into (i.e. those
 * bus_mix_target resolves non-NULL for) — the set that must be cleared before
 * an accumulating render. A voice on an unallocated or out-of-range bus does
 * NOT set that bus's bit, because bus_mix_build_table sent its audio to
 * main_buf instead; clearing an unrendered buffer here would be a NULL
 * dereference in the caller. Returns how many bits are set.
 *
 * Clearing by this mask rather than clearing all n_buses is what keeps an
 * unused bus free: an allocated-but-unrouted bus is never touched per frame.
 */
static inline int bus_mix_active_mask(const int8_t *voice_bus, int n_voices,
                                      int n_buses, int16_t *const *bus_buf,
                                      uint32_t *out_mask)
{
    uint32_t m = 0;
    int n = 0;
    for (int i = 0; i < n_voices; i++) {
        int b = voice_bus ? voice_bus[i] : BUS_MIX_MAIN;
        if (bus_mix_target(b, n_buses, bus_buf) && !(m & (1u << b))) {
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
 * The <= 0 guard is not merely a performance shortcut for the common
 * zero-send case — it is the only thing standing between a negative level
 * and a phase-inverted send (dst -= src), which would read as a synthesis
 * bug, not a mixing one, since nothing about it looks like a send.
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
