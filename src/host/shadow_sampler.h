/* shadow_sampler.h - Quantized sampler and skipback subsystem
 * Extracted from move_anything_shim.c for maintainability. */

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
    SAMPLER_RECORDING
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
#define SAMPLER_SETTINGS_PATH "/data/UserData/move-anything/settings.txt"
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
#define SAMPLER_RECORDINGS_DIR "/data/UserData/UserLibrary/Samples/Move Everything"

#define SKIPBACK_SECONDS 30
#define SKIPBACK_SAMPLES (SAMPLER_SAMPLE_RATE * SKIPBACK_SECONDS)
#define SKIPBACK_BUFFER_SIZE (SKIPBACK_SAMPLES * SAMPLER_NUM_CHANNELS * sizeof(int16_t))
#define SKIPBACK_DIR "/data/UserData/UserLibrary/Samples/Move Everything/Skipback"
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
extern int sampler_target_pulses;
extern int sampler_bars_completed;
extern int sampler_fallback_blocks;
extern int sampler_fallback_target;
extern int sampler_clock_received;

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
void sampler_start_recording(void);
void sampler_stop_recording(void);

/* Capture one audio block during recording */
void sampler_capture_audio(void);

/* Process MIDI clock/start/stop messages */
void sampler_on_clock(uint8_t status);

/* Skipback: allocate buffer, capture audio, trigger save */
void skipback_init(void);
void skipback_capture(int16_t *audio);
void skipback_trigger_save(void);

/* Update VU meter from audio source */
void sampler_update_vu(void);

#endif /* SHADOW_SAMPLER_H */
