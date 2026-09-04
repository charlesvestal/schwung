/* shadow_sampler.h - Quantized sampler and skipback subsystem
 * Extracted from schwung_shim.c for maintainability. */

#ifndef SHADOW_SAMPLER_H
#define SHADOW_SAMPLER_H

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

/* ============================================================================
 * Audio constants (must match shim mailbox layout)
 * ============================================================================ */
#define SAMPLER_AUDIO_OUT_OFFSET 256
#define SAMPLER_AUDIO_IN_OFFSET  2304
#define SAMPLER_FRAMES_PER_BLOCK 128

/* ============================================================================
 * Types
 * ============================================================================ */

typedef enum {
    SAMPLER_IDLE = 0,
    SAMPLER_ARMED,
    SAMPLER_RECORDING,
    SAMPLER_PREROLL,
    SAMPLER_PAUSED,
    SAMPLER_FINALIZING  /* stop requested; shim worker is draining/closing
                           the file. Mirrors as RECORDING via
                           sampler_get_state() so JS modules can keep
                           polling host_sampler_is_recording() until the
                           WAV is complete on disk. */
} sampler_state_t;

typedef enum {
    TEMPO_SOURCE_DEFAULT = 0,
    TEMPO_SOURCE_SETTINGS,
    TEMPO_SOURCE_SET,
    TEMPO_SOURCE_LAST_CLOCK,
    TEMPO_SOURCE_CLOCK
} tempo_source_t;

typedef enum {
    SAMPLER_SOURCE_RESAMPLE = 0,
    SAMPLER_SOURCE_MOVE_INPUT
} sampler_source_t;

typedef enum {
    SAMPLER_MENU_SOURCE = 0,
    SAMPLER_MENU_DURATION,
    SAMPLER_MENU_PREROLL,
    SAMPLER_MENU_COUNT
} sampler_menu_item_t;

typedef struct {
    char riff_id[4];
    uint32_t file_size;
    char wave_id[4];
    char fmt_id[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_id[4];
    uint32_t data_size;
} sampler_wav_header_t;

/* ============================================================================
 * Constants
 * ============================================================================ */

#define SAMPLER_DURATION_COUNT 6
#define SAMPLER_CLOCK_STALE_THRESHOLD 200
#define SAMPLER_SETTINGS_PATH "/data/UserData/schwung/settings.txt"
#define SAMPLER_SETS_DIR "/data/UserData/UserLibrary/Sets"
#define SAMPLER_OVERLAY_DONE_FRAMES 90
#define SAMPLER_VU_HOLD_DURATION 8
#define SAMPLER_VU_DECAY_RATE 1500
#define SAMPLER_SAMPLE_RATE 44100
#define SAMPLER_NUM_CHANNELS 2
#define SAMPLER_BITS_PER_SAMPLE 16
#define SAMPLER_RING_BUFFER_SECONDS 2
#define SAMPLER_RING_BUFFER_SAMPLES (SAMPLER_SAMPLE_RATE * SAMPLER_RING_BUFFER_SECONDS)
#define SAMPLER_RING_BUFFER_SIZE (SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS * sizeof(int16_t))
#define SAMPLER_RECORDINGS_DIR "/data/UserData/UserLibrary/Samples/Schwung/Resampler"

/* ---------------------------------------------------------------- stems ---
 *
 * A STEM IS A SLOT, and under Move->Schwung that is not a simplification --
 * the shim builds each slot as `move_track[s] + synth[s]` and then runs the
 * slot FX chain on the SUM (schwung_shim.c, the rebuild_from_la branch). Move's
 * track and Schwung's synth are inseparable after that point, and splitting
 * them before it would hand back stems without their FX. So the four slot
 * stems ARE the four tracks, and under Move->Schwung they sum to the master
 * exactly: the reconstruction is composited from those four Link Audio
 * channels and nothing else. (Which is also why Move's metronome is missing
 * from it -- see shadow_metronome.h. Nothing else is in there either.)
 *
 * The fifth stem is MOVE, and it exists for the case the other four cannot
 * cover. With Link Audio routing OFF there is no four-way split at all: Move's
 * audio arrives as one mailbox mix, and the slots carry only Schwung's own
 * synths. Without a Move stem the feature would quietly record four
 * synth-only files and drop the rest of the music on the floor. It is empty
 * under Move->Schwung, by construction, for the same reason the slot stems
 * are complete there.
 *
 * Stems are PRE-MASTER-FX and pre-master-volume. Master FX processes the
 * mixed bus, so there is no per-stem version of it to capture; a stem sum will
 * differ from the master file by exactly whatever the Master FX chain does.
 *
 * Every stem file is opened eagerly and DELETED AT FINALIZE if it never saw a
 * non-zero sample, so an unloaded slot leaves no file. Opening them lazily on
 * first audio was the obvious alternative and is wrong: the file would then
 * start at the first sound rather than at t=0, and the stems would no longer
 * line up with each other or with the master.
 */
#define SAMPLER_STEM_COUNT 5
#define SAMPLER_STEM_MOVE  4   /* index of the Move stem; 0-3 are the slots */

/* File-name suffixes, parallel to the stem indices. */
extern const char *const sampler_stem_names[SAMPLER_STEM_COUNT];

#define SKIPBACK_DEFAULT_SECONDS 30
#define SKIPBACK_MAX_SECONDS 300
#define SKIPBACK_DIR "/data/UserData/UserLibrary/Samples/Schwung/Skipback"
/*
 * Stem rolling buffers are capped well below SKIPBACK_MAX_SECONDS.
 *
 * The master buffer is one stereo ring of up to 5 minutes (53 MB). Five of
 * those is ~265 MB, which is not a budget this device has to spend on a
 * feature that is off by default. 60 s x 5 is 53 MB -- the same as one master
 * buffer at its maximum -- and it is the length that actually gets used: the
 * default skipback length is 30 s and both fit under the cap untouched.
 *
 * When the master is longer than this, the stems are a SUFFIX of it. Both end
 * at the same instant (the save), so the stem files line up with the tail of
 * the master rather than drifting against it.
 */
#define SKIPBACK_STEM_MAX_SECONDS 60
#define SKIPBACK_OVERLAY_FRAMES 171

/* ============================================================================
 * Callback struct - shim functions the sampler needs
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    void (*announce)(const char *msg);
    void (*overlay_sync)(void);
    int (*run_command)(const char *const argv[]);
    /* Pointers to shim's mmap addresses (indirect, since they change) */
    uint8_t **global_mmap_addr;
    uint8_t **hardware_mmap_addr;
} sampler_host_t;

/* ============================================================================
 * Extern globals - sampler state readable/writable by the shim
 * ============================================================================ */

extern sampler_state_t sampler_state;
extern const int sampler_duration_options[];
extern int sampler_duration_index;

extern int sampler_clock_count;
/* Free-running MIDI clock pulses (24 PPQN) since the last MIDI Start. Unlike
 * sampler_clock_count, advances whatever the sampler is doing — see the
 * definition. */
extern int shadow_transport_pulses;
extern int sampler_target_pulses;
extern int sampler_bars_completed;
extern int sampler_fallback_blocks;
extern int sampler_fallback_target;
extern int sampler_clock_received;
extern int sampler_transport_playing;

extern struct timespec sampler_clock_last_beat;
extern int sampler_clock_beat_ticks;
extern float sampler_measured_bpm;
extern float sampler_last_known_bpm;
extern int sampler_clock_active;
extern int sampler_clock_stale_frames;

extern int sampler_settings_tempo;

extern int sampler_overlay_active;
extern int sampler_overlay_timeout;
extern sampler_source_t sampler_source;
extern int sampler_menu_cursor;
extern int16_t sampler_vu_peak;
extern int sampler_vu_hold_frames;
extern int sampler_fullscreen_active;

extern uint32_t sampler_samples_written;

extern int sampler_preroll_enabled;
extern int sampler_preroll_clock_count;
extern int sampler_preroll_target_pulses;
extern int sampler_preroll_fallback_blocks;
extern int sampler_preroll_fallback_target;

extern int sampler_external_stop_only;

extern volatile int skipback_overlay_timeout;

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize sampler subsystem with callbacks to shim functions.
 * Must be called before any other sampler function.
 * sampler_set_tempo_ptr: pointer to shim's sampler_set_tempo global. */
void sampler_init(const sampler_host_t *host, float *sampler_set_tempo_ptr);

/* Read tempo from current set's Song.abl file */
float sampler_read_set_tempo(const char *set_name);

/* Get best available BPM using fallback chain */
float sampler_get_bpm(tempo_source_t *source);

/* Build screen reader string for current menu item */
void sampler_announce_menu_item(void);

/* Start/stop recording */
/* Recording start/stop is split for realtime safety:
 *  - the RT halves below arm state and post an event to the shim worker
 *    (safe from the SPI callbacks; the triggering frame's audio lands in
 *    the pre-allocated ring immediately)
 *  - the sampler_worker_* halves run on the shim worker and do all file
 *    I/O: mkdir/fopen/header/writer-thread on start; join/trim/close on
 *    stop. Events execute in posted order, so start→stop races resolve
 *    serially. */
int  sampler_request_start(int with_preroll);            /* RT: Shift+Sample paths */
int  sampler_request_start_custom(const char *path_or_null); /* RT: cmd path file when NULL */
void sampler_request_stop(void);                          /* RT: any state */
void sampler_worker_prepare(void);                        /* worker */
void sampler_worker_finalize(void);                       /* worker */
void sampler_worker_cancel_preroll(void);                 /* worker */
void skipback_worker_spawn_save(void);                    /* worker */
void sampler_pause_recording(void);
void sampler_resume_recording(void);

/* Query sampler state */
int sampler_get_state(void);

/* Pre-roll: countdown before recording */
void sampler_tick_preroll(void);

/* Capture one audio block during recording */
void sampler_capture_audio(void);

/* Capture one audio block from a caller-provided buffer (unity-level audio).
 * Used by the RESAMPLE path so the sampler captures pre-master-volume audio
 * from the shim's unity_view[] instead of the SPI mailbox. */
void sampler_capture_audio_from_buffer(const int16_t *src);

/* Process MIDI clock/start/stop messages */
void sampler_on_clock(uint8_t status);

/* Skipback: allocate buffer, capture audio, trigger save.
 * Pass desired duration in seconds (clamped to [SKIPBACK_DEFAULT_SECONDS, SKIPBACK_MAX_SECONDS]).
 * Calling skipback_init() multiple times is safe; size is established on first call. */
void skipback_init(int seconds);
void skipback_capture(int16_t *audio);
void skipback_amend(const int16_t *audio);
void skipback_trigger_save(void);

/* Resize the rolling buffer in place, preserving as much existing audio as
 * fits in the new size (oldest samples truncated when shrinking). Safe to
 * call from any non-realtime thread; gates the audio thread via the saving
 * flag for the duration of the swap. No-op if a save is in progress. */
void skipback_resize(int new_seconds);

/* Returns the currently allocated skipback duration in seconds (0 if not yet init). */
int skipback_get_seconds(void);

/* Amend: mix additional audio into the last captured sampler block */
void sampler_amend_audio(const int16_t *audio);

/* ---------------------------------------------------------------- stems ---
 *
 * Mode is one of SAVE_STEMS_* (shadow_constants.h), mirrored from
 * shadow_control_t.save_stems. Both setter and getter are RT-safe: the setter
 * stores a byte and, when the skipback stem buffers need to appear or go away,
 * posts SHIM_EVT_SKIPBACK_RESIZE for the worker to do the allocation.
 *
 * The mode is LATCHED when a recording is prepared, so changing the setting
 * mid-take cannot leave half a take in one shape and half in another. */
void sampler_set_stem_mode(int mode);
int  sampler_get_stem_mode(void);

/* Capture one block of per-stem audio (RT). `stems` is an array of `count`
 * pointers to FRAMES_PER_BLOCK*2 interleaved int16 buffers, indexed as
 * described above; a NULL entry captures silence for that stem, which keeps
 * the stems sample-aligned rather than punching a hole.
 *
 * MUST be called BEFORE sampler_capture_audio_from_buffer() for the same
 * block: both apply the start-of-recording fade-in ramp, and the master half
 * is the one that consumes the counter. */
void sampler_capture_stems(const int16_t *const *stems, int count);

/* Same, for the skipback rolling buffers. No-op unless stems are enabled and
 * the stem buffers were successfully allocated. */
void skipback_capture_stems(const int16_t *const *stems, int count);

/* Seconds actually allocated for the skipback STEM buffers, which is capped
 * below the master's length (SKIPBACK_STEM_MAX_SECONDS) -- five rolling
 * buffers at the master's 5-minute maximum would be ~265 MB. 0 when stems are
 * off or allocation failed. The stems are a SUFFIX of the master: both end at
 * the same instant. */
int skipback_stems_get_seconds(void);

/* Update VU meter from audio source */
void sampler_update_vu(void);

#endif /* SHADOW_SAMPLER_H */
