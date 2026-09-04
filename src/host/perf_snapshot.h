/* perf_snapshot.h — the SPI frame-budget snapshot, published over SHM.
 *
 * This struct used to be a file-static `spi_timing_snapshot_t` inside
 * src/schwung_shim.c, readable only by the shim's own background logger. The
 * manager's CPU page needs it, so it lives here and is published to
 * /schwung-perf.
 *
 * WHY THIS EXISTS AT ALL: modules are not processes. Every slot synth, slot
 * FX, Master FX and overtake DSP is a .so running on the SPI callback inside
 * MoveOriginal, so /proc can report what MoveOriginal costs in total but can
 * never split it by module. These numbers are the only per-module attribution
 * that exists.
 *
 * WRITER: the SPI callback, single writer, no locks. The stores are the ones
 * the shim already performed into a static — publishing costs no memcpy.
 * READER: any process, via the seqlock below.
 */

#ifndef PERF_SNAPSHOT_H
#define PERF_SNAPSHOT_H

#include <assert.h>
#include <stdint.h>

#define SHM_SCHWUNG_PERF      "/schwung-perf"
#define SCHWUNG_PERF_MAGIC    0x50455246u   /* "PERF" */
#define SCHWUNG_PERF_VERSION  1u

/* A whole page, deliberately. /dev/shm is tmpfs and allocates by page: measured
 * on the device, an 84-byte segment occupied 4096 — the same 8 blocks as a
 * 512-byte one. So headroom is free, and the asserts below are written so that
 * adding a field costs nothing and only SHRINKING fails the build. See
 * CLAUDE.md, "An SHM buffer sized to `sizeof` reads as FULL, and is not". */
#define SCHWUNG_PERF_SHM_SIZE 4096

/* Chain slots and Master FX slots. Restated rather than included because this
 * header is read by tests/host/ on the dev machine, which does not build the
 * chain manager. The tests/host/Makefile pins them against the real headers. */
#define PERF_CHAIN_SLOTS    4
#define PERF_MASTER_FX_SLOTS 8

typedef struct {
    /* magic and version MUST stay the first two fields.
     *
     * A segment left behind by an older shim may be SHORTER than this struct,
     * and touching the tail of an undersized mapping is SIGBUS. Keeping the
     * version check itself inside the first 8 bytes means it can always be read
     * safely off whatever is actually there. This is the LINK_AUDIO_IN_SHM
     * lesson; do not reorder. */
    uint32_t magic;
    uint32_t version;

    /* Seqlock. The writer does seq++ … stores … seq++, so an ODD value means a
     * write is in flight and a reader that sees the same EVEN value before and
     * after read a consistent snapshot. */
    uint32_t seq;

    uint32_t frame_ready;         /* 1 = frame-level fields valid */
    uint32_t granular_ready;      /* 1 = section + slot fields valid */

    /* How many SPI frames the averages below cover. A consumer that does not
     * know the window cannot tell a real average from a partial one. */
    uint32_t sample_window_frames;

    /* The denominator, MEASURED rather than assumed.
     *
     * This is frame_total_avg: the whole loop iteration, which is paced by the
     * blocking ioctl and therefore sits at the SPI frame period (~2710-2830 us
     * measured; 128 frames / 44100 Hz = 2902 us nominal). Using the measured
     * value means a percentage cannot silently drift from reality if the period
     * ever changes. NOTE this is why total_us is not a load signal on its own —
     * our work shrinks the driver's wait by the same amount. */
    uint64_t frame_period_us;

    /* Frame-level timing, avg/max over the last sample_window_frames. */
    uint64_t frame_total_avg, frame_total_max;
    uint64_t frame_pre_avg, frame_pre_max;
    uint64_t frame_ioctl_avg, frame_ioctl_max;
    uint64_t frame_post_avg, frame_post_max;

    /* Granular pre-ioctl sections. */
    uint64_t midi_mon_avg, midi_mon_max;
    uint64_t fwd_midi_avg, fwd_midi_max;
    uint64_t mix_audio_avg, mix_audio_max;
    uint64_t ui_req_avg, ui_req_max;
    uint64_t param_req_avg, param_req_max;
    uint64_t fwd_cc_avg, fwd_cc_max;
    /* proc_midi is where MIDI FX cost lands. MIDI FX have no per-frame render —
     * they run event-driven inside the chain host's on_midi — so they are NOT
     * separable per module and must never be presented as if they were. */
    uint64_t proc_midi_avg, proc_midi_max;
    uint64_t jack_stash_avg, jack_stash_max;
    uint64_t drain_dsp_avg, drain_dsp_max;
    uint64_t jack_wake_avg, jack_wake_max;
    uint64_t mix_buf_avg, mix_buf_max;
    uint64_t tts_avg, tts_max;
    uint64_t display_avg, display_max;
    uint64_t clear_leds_avg, clear_leds_max;
    uint64_t jack_midi_avg, jack_midi_max;
    uint64_t ui_midi_avg, ui_midi_max;
    uint64_t flush_leds_avg, flush_leds_max;
    uint64_t screenreader_avg, screenreader_max;
    uint64_t jack_pre_avg, jack_pre_max;
    uint64_t jack_disp_avg, jack_disp_max;
    uint64_t pin_avg, pin_max;

    /* Post-ioctl chunks. */
    uint64_t post_midi_scan_avg, post_midi_scan_max;
    uint64_t post_drain_dsp_avg, post_drain_dsp_max;
    uint64_t post_render_avg, post_render_max;

    /* Per-slot render breakdown. The _max entries already existed; the _avg
     * entries are new and are the point of this work — a max over a ~3 s window
     * is a SPIKE DETECTOR, not a load figure. */
    uint64_t slot_render_avg[PERF_CHAIN_SLOTS], slot_render_max[PERF_CHAIN_SLOTS];
    uint64_t slot_synth_avg[PERF_CHAIN_SLOTS],  slot_synth_max[PERF_CHAIN_SLOTS];
    uint64_t slot_fx_avg[PERF_CHAIN_SLOTS],     slot_fx_max[PERF_CHAIN_SLOTS];

    /* Per-Master-FX-slot, new call site. */
    uint64_t mfx_avg[PERF_MASTER_FX_SLOTS], mfx_max[PERF_MASTER_FX_SLOTS];

    /* Overtake DSP, new call sites. */
    uint64_t overtake_gen_avg, overtake_gen_max;
    uint64_t overtake_fx_avg,  overtake_fx_max;

    uint32_t slot_probe_burst_max;
    uint32_t jack_audio_hits;
    uint32_t jack_audio_misses;

    uint32_t overrun_count;
    uint64_t last_overrun_total, last_overrun_pre;
    uint64_t last_overrun_ioctl, last_overrun_post;
} schwung_perf_snapshot_t;

/* Only SHRINKING fails the build. Adding a field is free until it crosses the
 * page, at which point raise SCHWUNG_PERF_SHM_SIZE and bump the version. */
_Static_assert(sizeof(schwung_perf_snapshot_t) <= SCHWUNG_PERF_SHM_SIZE,
               "schwung_perf_snapshot_t outgrew its segment - raise "
               "SCHWUNG_PERF_SHM_SIZE and bump SCHWUNG_PERF_VERSION");
_Static_assert(SCHWUNG_PERF_SHM_SIZE >= 4096,
               "SCHWUNG_PERF_SHM_SIZE must never shrink below one page - "
               "tmpfs allocates by page, so a smaller container saves nothing "
               "and only makes the next field addition a breaking change");
_Static_assert(__builtin_offsetof(schwung_perf_snapshot_t, magic) == 0,
               "magic must be the first field - the version check has to be "
               "readable off a short segment left by an older shim");
_Static_assert(__builtin_offsetof(schwung_perf_snapshot_t, version) == 4,
               "version must be the second field");

#endif /* PERF_SNAPSHOT_H */
