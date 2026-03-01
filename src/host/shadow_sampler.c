/* shadow_sampler.c - Quantized sampler and skipback subsystem
 * Extracted from move_anything_shim.c for maintainability.
 *
 * This module handles:
 *   - Quantized sampler (Shift+Sample): record audio to WAV
 *   - Skipback (Shift+Capture): save last 30 seconds of audio
 *   - MIDI clock BPM measurement
 *   - VU metering for sampler UI */

#define _GNU_SOURCE
#include "shadow_sampler.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

/* ============================================================================
 * Host callbacks (set during sampler_init)
 * ============================================================================ */

static sampler_host_t s_host;
static float *s_set_tempo_ptr;  /* pointer to shim's sampler_set_tempo */

/* ============================================================================
 * Globals
 * ============================================================================ */

/* Sampler state machine */
sampler_state_t sampler_state = SAMPLER_IDLE;

/* Duration options (bars); 0 = unlimited */
const int sampler_duration_options[] = {0, 1, 2, 4, 8, 16};
int sampler_duration_index = 3;  /* Default: 4 bars */

/* MIDI clock counting */
int sampler_clock_count = 0;
int sampler_target_pulses = 0;
int sampler_bars_completed = 0;

/* Fallback timing (when no MIDI clock) */
int sampler_fallback_blocks = 0;
int sampler_fallback_target = 0;
int sampler_clock_received = 0;

/* Pre-roll state */
int sampler_preroll_enabled = 0;
int sampler_preroll_clock_count = 0;
int sampler_preroll_target_pulses = 0;
int sampler_preroll_fallback_blocks = 0;
int sampler_preroll_fallback_target = 0;

/* Tempo detection: MIDI clock BPM measurement */
struct timespec sampler_clock_last_beat = {0, 0};
int sampler_clock_beat_ticks = 0;
float sampler_measured_bpm = 0.0f;
float sampler_last_known_bpm = 0.0f;
int sampler_clock_active = 0;
int sampler_clock_stale_frames = 0;

/* Tempo detection: settings file */
int sampler_settings_tempo = 0;

/* Overlay state */
int sampler_overlay_active = 0;
int sampler_overlay_timeout = 0;

/* Source selection */
sampler_source_t sampler_source = SAMPLER_SOURCE_RESAMPLE;

/* Menu cursor */
int sampler_menu_cursor = SAMPLER_MENU_SOURCE;

/* VU meter */
int16_t sampler_vu_peak = 0;
int sampler_vu_hold_frames = 0;

/* Full-screen mode flag */
int sampler_fullscreen_active = 0;

/* Recording state */
static FILE *sampler_wav_file = NULL;
uint32_t sampler_samples_written = 0;
static char sampler_current_recording[256] = "";
static int16_t *sampler_ring_buffer = NULL;
static size_t sampler_ring_write_pos = 0;
static size_t sampler_ring_read_pos = 0;
static pthread_t sampler_writer_thread;
static pthread_mutex_t sampler_ring_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sampler_ring_cond = PTHREAD_COND_INITIALIZER;
static volatile int sampler_writer_running = 0;
static volatile int sampler_writer_should_exit = 0;

/* Skipback state */
static int16_t *skipback_buffer = NULL;
static volatile size_t skipback_write_pos = 0;
static volatile int skipback_buffer_full = 0;
static int skipback_saving = 0;
static pthread_t skipback_writer_thread;
volatile int skipback_overlay_timeout = 0;

/* ============================================================================
 * Initialization
 * ============================================================================ */

void sampler_init(const sampler_host_t *host, float *sampler_set_tempo_ptr) {
    s_host = *host;
    s_set_tempo_ptr = sampler_set_tempo_ptr;
}

/* ============================================================================
 * WAV, ring buffer, recording, audio capture, MIDI clock
 * ============================================================================ */

static void sampler_write_wav_header(FILE *f, uint32_t data_size) {
    sampler_wav_header_t header;
    memcpy(header.riff_id, "RIFF", 4);
    header.file_size = 36 + data_size;
    memcpy(header.wave_id, "WAVE", 4);
    memcpy(header.fmt_id, "fmt ", 4);
    header.fmt_size = 16;
    header.audio_format = 1;  /* PCM */
    header.num_channels = SAMPLER_NUM_CHANNELS;
    header.sample_rate = SAMPLER_SAMPLE_RATE;
    header.byte_rate = SAMPLER_SAMPLE_RATE * SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
    header.block_align = SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
    header.bits_per_sample = SAMPLER_BITS_PER_SAMPLE;
    memcpy(header.data_id, "data", 4);
    header.data_size = data_size;
    fseek(f, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, f);
}

static size_t sampler_ring_available_write(void) {
    size_t write_pos = __atomic_load_n(&sampler_ring_write_pos, __ATOMIC_ACQUIRE);
    size_t read_pos = __atomic_load_n(&sampler_ring_read_pos, __ATOMIC_ACQUIRE);
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;
    if (write_pos >= read_pos)
        return buffer_samples - (write_pos - read_pos) - 1;
    else
        return read_pos - write_pos - 1;
}

static size_t sampler_ring_available_read(void) {
    size_t write_pos = __atomic_load_n(&sampler_ring_write_pos, __ATOMIC_ACQUIRE);
    size_t read_pos = __atomic_load_n(&sampler_ring_read_pos, __ATOMIC_ACQUIRE);
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;
    if (write_pos >= read_pos)
        return write_pos - read_pos;
    else
        return buffer_samples - (read_pos - write_pos);
}

static void *sampler_writer_thread_func(void *arg) {
    (void)arg;
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;
    size_t write_chunk = SAMPLER_SAMPLE_RATE * SAMPLER_NUM_CHANNELS / 4;  /* ~250ms */

    while (1) {
        pthread_mutex_lock(&sampler_ring_mutex);
        while (sampler_ring_available_read() < write_chunk && !sampler_writer_should_exit) {
            pthread_cond_wait(&sampler_ring_cond, &sampler_ring_mutex);
        }
        int should_exit = sampler_writer_should_exit;
        pthread_mutex_unlock(&sampler_ring_mutex);

        size_t available = sampler_ring_available_read();
        while (available > 0 && sampler_wav_file) {
            size_t read_pos = __atomic_load_n(&sampler_ring_read_pos, __ATOMIC_ACQUIRE);
            size_t to_end = buffer_samples - read_pos;
            size_t to_write = (available < to_end) ? available : to_end;
            fwrite(&sampler_ring_buffer[read_pos], sizeof(int16_t), to_write, sampler_wav_file);
            sampler_samples_written += to_write / SAMPLER_NUM_CHANNELS;
            __atomic_store_n(&sampler_ring_read_pos, (read_pos + to_write) % buffer_samples, __ATOMIC_RELEASE);
            available = sampler_ring_available_read();
        }

        if (should_exit) break;
    }
    return NULL;
}

/* Read tempo from the current Set's Song.abl file. */
float sampler_read_set_tempo(const char *set_name) {
    if (!set_name || !set_name[0]) return 0.0f;

    DIR *sets_dir = opendir(SAMPLER_SETS_DIR);
    if (!sets_dir) return 0.0f;

    char best_path[512] = "";
    time_t best_mtime = 0;
    struct dirent *entry;

    while ((entry = readdir(sets_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s/%s/Song.abl",
                 SAMPLER_SETS_DIR, entry->d_name, set_name);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (st.st_mtime > best_mtime) {
                best_mtime = st.st_mtime;
                snprintf(best_path, sizeof(best_path), "%s", path);
            }
        }
    }
    closedir(sets_dir);

    if (best_path[0] == '\0') return 0.0f;

    FILE *f = fopen(best_path, "r");
    if (!f) return 0.0f;

    float tempo = 0.0f;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "\"tempo\":");
        if (p) {
            p += 8;
            while (*p == ' ') p++;
            tempo = strtof(p, NULL);
            if (tempo >= 20.0f && tempo <= 999.0f) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Set tempo: %.1f BPM from %s", tempo, best_path);
                s_host.log(msg);
                break;
            }
            tempo = 0.0f;
        }
    }
    fclose(f);
    return tempo;
}

/* Read tempo_bpm from Move Anything settings file */
static int sampler_read_settings_tempo(void) {
    FILE *f = fopen(SAMPLER_SETTINGS_PATH, "r");
    if (!f) return 0;

    char line[256];
    int bpm = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        if (strcmp(line, "tempo_bpm") == 0) {
            bpm = atoi(eq + 1);
            if (bpm < 20) bpm = 20;
            if (bpm > 300) bpm = 300;
            break;
        }
    }
    fclose(f);
    return bpm;
}

/* Get best available BPM using fallback chain */
float sampler_get_bpm(tempo_source_t *source) {
    /* 1. Active MIDI clock */
    if (sampler_clock_active && sampler_measured_bpm >= 20.0f) {
        if (source) *source = TEMPO_SOURCE_CLOCK;
        return sampler_measured_bpm;
    }

    /* 2. Current Set's tempo */
    float set_tempo = s_set_tempo_ptr ? *s_set_tempo_ptr : 0.0f;
    if (set_tempo >= 20.0f) {
        if (source) *source = TEMPO_SOURCE_SET;
        return set_tempo;
    }

    /* 3. Last measured clock BPM */
    if (sampler_last_known_bpm >= 20.0f) {
        if (source) *source = TEMPO_SOURCE_LAST_CLOCK;
        return sampler_last_known_bpm;
    }

    /* 4. Settings file tempo */
    if (sampler_settings_tempo == 0) {
        sampler_settings_tempo = sampler_read_settings_tempo();
        if (sampler_settings_tempo == 0) sampler_settings_tempo = -1;
    }
    if (sampler_settings_tempo > 0) {
        if (source) *source = TEMPO_SOURCE_SETTINGS;
        return (float)sampler_settings_tempo;
    }

    /* 5. Default */
    if (source) *source = TEMPO_SOURCE_DEFAULT;
    return 120.0f;
}

/* Build a screen reader string describing current sampler menu item */
void sampler_announce_menu_item(void) {
    char sr_buf[128];
    if (sampler_menu_cursor == SAMPLER_MENU_SOURCE) {
        snprintf(sr_buf, sizeof(sr_buf), "Source, %s",
                 sampler_source == SAMPLER_SOURCE_RESAMPLE ? "Resample" : "Move Input");
    } else if (sampler_menu_cursor == SAMPLER_MENU_DURATION) {
        int bars = sampler_duration_options[sampler_duration_index];
        if (bars == 0)
            snprintf(sr_buf, sizeof(sr_buf), "Duration, Until stop");
        else
            snprintf(sr_buf, sizeof(sr_buf), "Duration, %d bar%s", bars, bars > 1 ? "s" : "");
    } else if (sampler_menu_cursor == SAMPLER_MENU_PREROLL) {
        snprintf(sr_buf, sizeof(sr_buf), "Pre-roll, %s",
                 sampler_preroll_enabled ? "On" : "Off");
    } else {
        return;
    }
    s_host.announce(sr_buf);
}

void sampler_start_preroll(void) {
    sampler_preroll_clock_count = 0;
    sampler_preroll_fallback_blocks = 0;

    int bars = sampler_duration_options[sampler_duration_index];
    sampler_preroll_target_pulses = bars * 4 * 24;

    tempo_source_t src;
    float bpm = sampler_get_bpm(&src);
    float seconds = bars * 4.0f * 60.0f / bpm;
    sampler_preroll_fallback_target = (int)(seconds * 44100.0f / 128.0f);

    sampler_state = SAMPLER_PREROLL;
    sampler_fullscreen_active = 1;
    sampler_overlay_active = 1;
    s_host.overlay_sync();

    char msg[128];
    snprintf(msg, sizeof(msg), "Sampler: preroll started (%d bars, %.1f BPM)", bars, bpm);
    s_host.log(msg);
}

void sampler_tick_preroll(void) {
    if (sampler_state != SAMPLER_PREROLL) return;

    sampler_preroll_fallback_blocks++;
    if (sampler_preroll_fallback_target > 0 && sampler_preroll_fallback_blocks >= sampler_preroll_fallback_target) {
        s_host.log("Sampler: preroll complete (fallback timer)");
        sampler_start_recording();
    }
}

void sampler_start_recording(void) {
    if (sampler_writer_running) return;

    /* Generate date-based save directory and filename */
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm_info = localtime_r(&now, &tm_buf);
    if (!tm_info) {
        s_host.log("Sampler: failed to get local time");
        s_host.announce("Recording failed");
        return;
    }
    char date_subdir[32];
    if (strftime(date_subdir, sizeof(date_subdir), "%Y-%m-%d", tm_info) == 0) {
        s_host.log("Sampler: failed to format date subdirectory");
        s_host.announce("Recording failed");
        return;
    }
    char recording_dir[256];
    snprintf(recording_dir, sizeof(recording_dir), "%s/%s", SAMPLER_RECORDINGS_DIR, date_subdir);

    {
        struct stat st;
        if (stat(recording_dir, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", recording_dir, NULL };
            s_host.run_command(mkdir_argv);
        }
    }

    /* Get BPM for filename */
    tempo_source_t bpm_src;
    float bpm_for_name = sampler_get_bpm(&bpm_src);
    int bpm_int = (int)(bpm_for_name + 0.5f);

    snprintf(sampler_current_recording, sizeof(sampler_current_recording), "%s/sample_%04d%02d%02d_%02d%02d%02d_%dbpm.wav",
             recording_dir,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
             bpm_int);

    /* Allocate ring buffer */
    sampler_ring_buffer = malloc(SAMPLER_RING_BUFFER_SIZE);
    if (!sampler_ring_buffer) {
        s_host.log("Sampler: failed to allocate ring buffer");
        s_host.announce("Recording failed");
        return;
    }

    /* Open file */
    sampler_wav_file = fopen(sampler_current_recording, "wb");
    if (!sampler_wav_file) {
        s_host.log("Sampler: failed to open WAV file");
        s_host.announce("Recording failed");
        free(sampler_ring_buffer);
        sampler_ring_buffer = NULL;
        return;
    }

    /* Initialize state */
    sampler_samples_written = 0;
    __atomic_store_n(&sampler_ring_write_pos, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&sampler_ring_read_pos, 0, __ATOMIC_RELEASE);
    sampler_writer_should_exit = 0;
    sampler_clock_count = 0;
    sampler_bars_completed = 0;
    sampler_clock_received = 0;
    sampler_fallback_blocks = 0;

    /* Calculate target: bars * 4 beats * 24 PPQN (0 = unlimited) */
    int bars = sampler_duration_options[sampler_duration_index];
    if (bars > 0) {
        sampler_target_pulses = bars * 4 * 24;
        tempo_source_t src;
        float bpm = sampler_get_bpm(&src);
        float seconds = bars * 4.0f * 60.0f / bpm;
        sampler_fallback_target = (int)(seconds * 44100.0f / 128.0f);
        {
            char msg[128];
            const char *src_names[] = {"default", "settings", "set", "last clock", "clock"};
            snprintf(msg, sizeof(msg), "Sampler: using %.1f BPM (%s) for fallback timing",
                     bpm, src_names[src]);
            s_host.log(msg);
        }
    } else {
        sampler_target_pulses = 0;
        sampler_fallback_target = 0;
    }

    /* Write placeholder header */
    sampler_write_wav_header(sampler_wav_file, 0);

    /* Start writer thread */
    if (pthread_create(&sampler_writer_thread, NULL, sampler_writer_thread_func, NULL) != 0) {
        s_host.log("Sampler: failed to create writer thread");
        s_host.announce("Recording failed");
        fclose(sampler_wav_file);
        sampler_wav_file = NULL;
        free(sampler_ring_buffer);
        sampler_ring_buffer = NULL;
        return;
    }

    sampler_writer_running = 1;
    sampler_state = SAMPLER_RECORDING;
    sampler_overlay_active = 1;
    sampler_overlay_timeout = 0;
    s_host.overlay_sync();

    char msg[256];
    if (bars > 0)
        snprintf(msg, sizeof(msg), "Sampler: recording started (%d bars) -> %s",
                 bars, sampler_current_recording);
    else
        snprintf(msg, sizeof(msg), "Sampler: recording started (until stopped) -> %s",
                 sampler_current_recording);
    s_host.log(msg);
}

void sampler_stop_recording(void) {
    /* If in preroll, just cancel back to armed (no WAV to finalize) */
    if (sampler_state == SAMPLER_PREROLL) {
        s_host.log("Sampler: preroll cancelled");
        sampler_state = SAMPLER_ARMED;
        s_host.overlay_sync();
        return;
    }

    if (!sampler_writer_running) return;

    s_host.log("Sampler: stopping recording");

    /* Signal writer thread to exit */
    pthread_mutex_lock(&sampler_ring_mutex);
    sampler_writer_should_exit = 1;
    pthread_cond_signal(&sampler_ring_cond);
    pthread_mutex_unlock(&sampler_ring_mutex);

    pthread_join(sampler_writer_thread, NULL);
    sampler_writer_running = 0;

    /* Update WAV header with final size */
    if (sampler_wav_file) {
        uint32_t data_size = sampler_samples_written * SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
        sampler_write_wav_header(sampler_wav_file, data_size);
        fclose(sampler_wav_file);
        sampler_wav_file = NULL;
    }

    /* Free ring buffer */
    if (sampler_ring_buffer) {
        free(sampler_ring_buffer);
        sampler_ring_buffer = NULL;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Sampler: saved %s (%u samples, %.1f sec)",
             sampler_current_recording, sampler_samples_written,
             (float)sampler_samples_written / SAMPLER_SAMPLE_RATE);
    s_host.log(msg);

    sampler_current_recording[0] = '\0';
    sampler_state = SAMPLER_IDLE;

    s_host.announce("Sample saved");

    /* Keep fullscreen active for "saved" message, then timeout */
    sampler_overlay_active = 1;
    sampler_overlay_timeout = SAMPLER_OVERLAY_DONE_FRAMES;
    s_host.overlay_sync();
}

void sampler_capture_audio(void) {
    if (sampler_state != SAMPLER_RECORDING || !sampler_ring_buffer) return;

    /* Select audio source */
    int16_t *audio = NULL;
    uint8_t *gmmap = s_host.global_mmap_addr ? *s_host.global_mmap_addr : NULL;
    uint8_t *hmmap = s_host.hardware_mmap_addr ? *s_host.hardware_mmap_addr : NULL;

    if (sampler_source == SAMPLER_SOURCE_RESAMPLE && gmmap) {
        audio = (int16_t *)(gmmap + SAMPLER_AUDIO_OUT_OFFSET);
    } else if (sampler_source == SAMPLER_SOURCE_MOVE_INPUT && hmmap) {
        audio = (int16_t *)(hmmap + SAMPLER_AUDIO_IN_OFFSET);
    }
    if (!audio) return;

    size_t samples_to_write = SAMPLER_FRAMES_PER_BLOCK * SAMPLER_NUM_CHANNELS;
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;

    /* Write to ring buffer if space available */
    if (sampler_ring_available_write() >= samples_to_write) {
        size_t write_pos = __atomic_load_n(&sampler_ring_write_pos, __ATOMIC_ACQUIRE);
        for (size_t i = 0; i < samples_to_write; i++) {
            sampler_ring_buffer[write_pos] = audio[i];
            write_pos = (write_pos + 1) % buffer_samples;
        }
        __atomic_store_n(&sampler_ring_write_pos, write_pos, __ATOMIC_RELEASE);

        pthread_mutex_lock(&sampler_ring_mutex);
        pthread_cond_signal(&sampler_ring_cond);
        pthread_mutex_unlock(&sampler_ring_mutex);
    }

    /* Fallback timeout */
    if (!sampler_clock_received && sampler_fallback_target > 0) {
        sampler_fallback_blocks++;
        int bars = sampler_duration_options[sampler_duration_index];
        if (bars > 0) {
            int completed = (sampler_fallback_blocks * bars) / sampler_fallback_target;
            if (completed < 0) completed = 0;
            if (completed > bars - 1) completed = bars - 1;
            sampler_bars_completed = completed;
        }
        if (sampler_fallback_blocks >= sampler_fallback_target) {
            s_host.log("Sampler: fallback timeout reached (no MIDI clock)");
            sampler_stop_recording();
        }
    }
}

void sampler_on_clock(uint8_t status) {
    if (status == 0xF8) {
        /* MIDI Clock tick */
        sampler_clock_active = 1;
        sampler_clock_stale_frames = 0;
        sampler_clock_beat_ticks++;

        /* Measure BPM every 24 ticks (one beat) */
        if (sampler_clock_beat_ticks >= 24) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (sampler_clock_last_beat.tv_sec > 0) {
                double elapsed = (now.tv_sec - sampler_clock_last_beat.tv_sec)
                               + (now.tv_nsec - sampler_clock_last_beat.tv_nsec) / 1e9;
                if (elapsed > 0.1 && elapsed < 10.0) {
                    sampler_measured_bpm = 60.0f / (float)elapsed;
                    sampler_last_known_bpm = sampler_measured_bpm;
                }
            }
            sampler_clock_last_beat = now;
            sampler_clock_beat_ticks = 0;
        }

        /* Preroll-specific: count pulses for preroll countdown */
        if (sampler_state == SAMPLER_PREROLL) {
            sampler_preroll_clock_count++;
            if (sampler_preroll_target_pulses > 0 && sampler_preroll_clock_count >= sampler_preroll_target_pulses) {
                s_host.log("Sampler: preroll complete via MIDI clock");
                sampler_start_recording();
            }
        }

        /* Recording-specific: count pulses for auto-stop */
        if (sampler_state == SAMPLER_RECORDING) {
            sampler_clock_received = 1;
            sampler_clock_count++;
            sampler_bars_completed = sampler_clock_count / 96;

            if (sampler_target_pulses > 0 && sampler_clock_count >= sampler_target_pulses) {
                s_host.log("Sampler: target duration reached via MIDI clock");
                sampler_stop_recording();
            }
        }
    } else if (status == 0xFA) {
        /* MIDI Start */
        if (sampler_state == SAMPLER_ARMED) {
            s_host.log("Sampler: triggered by MIDI Start");
            if (sampler_preroll_enabled && sampler_duration_options[sampler_duration_index] > 0) {
                sampler_start_preroll();
            } else {
                sampler_start_recording();
            }
        }
    }
    else if (status == 0xFC) {
        /* MIDI Stop */
        if (sampler_state == SAMPLER_RECORDING) {
            s_host.log("Sampler: stopped by MIDI Stop");
            sampler_stop_recording();
        } else if (sampler_state == SAMPLER_PREROLL) {
            s_host.log("Sampler: preroll cancelled by MIDI Stop");
            sampler_state = SAMPLER_ARMED;
            s_host.overlay_sync();
        }
    }
}

/* ============================================================================
 * Skipback
 * ============================================================================ */

void skipback_init(void) {
    if (skipback_buffer) return;
    skipback_buffer = (int16_t *)calloc(SKIPBACK_SAMPLES * SAMPLER_NUM_CHANNELS, sizeof(int16_t));
    if (skipback_buffer) {
        skipback_write_pos = 0;
        skipback_buffer_full = 0;
        s_host.log("Skipback: allocated 30s rolling buffer");
    } else {
        s_host.log("Skipback: failed to allocate buffer");
    }
}

void skipback_capture(int16_t *audio) {
    if (!skipback_buffer || !audio || __atomic_load_n(&skipback_saving, __ATOMIC_ACQUIRE)) return;

    size_t total_samples = SKIPBACK_SAMPLES * SAMPLER_NUM_CHANNELS;
    size_t block_samples = SAMPLER_FRAMES_PER_BLOCK * SAMPLER_NUM_CHANNELS;
    size_t wp = skipback_write_pos;

    for (size_t i = 0; i < block_samples; i++) {
        skipback_buffer[wp] = audio[i];
        wp = (wp + 1) % total_samples;
    }

    if (!skipback_buffer_full && wp < skipback_write_pos)
        skipback_buffer_full = 1;
    skipback_write_pos = wp;
}

static void *skipback_writer_func(void *arg) {
    (void)arg;

    /* Build date-based save directory */
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm_info = localtime_r(&now, &tm_buf);
    if (!tm_info) {
        s_host.log("Skipback: failed to get local time");
        s_host.announce("Skipback failed");
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return NULL;
    }
    char date_subdir[32];
    if (strftime(date_subdir, sizeof(date_subdir), "%Y-%m-%d", tm_info) == 0) {
        s_host.log("Skipback: failed to format date subdirectory");
        s_host.announce("Skipback failed");
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return NULL;
    }
    char skipback_dir[256];
    snprintf(skipback_dir, sizeof(skipback_dir), "%s/%s", SKIPBACK_DIR, date_subdir);

    /* Create directory */
    {
        struct stat st;
        if (stat(skipback_dir, &st) != 0) {
            const char *mkdir_argv[] = { "mkdir", "-p", skipback_dir, NULL };
            s_host.run_command(mkdir_argv);
        }
    }

    /* Generate filename */
    char path[256];
    snprintf(path, sizeof(path), "%s/skipback_%04d%02d%02d_%02d%02d%02d.wav",
             skipback_dir,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    FILE *f = fopen(path, "wb");
    if (!f) {
        s_host.log("Skipback: failed to open WAV file");
        s_host.announce("Skipback failed");
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return NULL;
    }

    /* Determine how much data to write */
    size_t total_samples = SKIPBACK_SAMPLES * SAMPLER_NUM_CHANNELS;
    size_t wp = skipback_write_pos;
    size_t data_samples;
    size_t start_pos;

    if (skipback_buffer_full) {
        data_samples = total_samples;
        start_pos = wp;
    } else {
        data_samples = wp;
        start_pos = 0;
    }

    if (data_samples == 0) {
        s_host.log("Skipback: no audio captured yet");
        s_host.announce("No audio captured yet");
        fclose(f);
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return NULL;
    }

    /* Write WAV header */
    uint32_t data_bytes = (uint32_t)(data_samples * sizeof(int16_t));
    sampler_wav_header_t hdr;
    memcpy(hdr.riff_id, "RIFF", 4);
    hdr.file_size = 36 + data_bytes;
    memcpy(hdr.wave_id, "WAVE", 4);
    memcpy(hdr.fmt_id, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_format = 1;
    hdr.num_channels = SAMPLER_NUM_CHANNELS;
    hdr.sample_rate = SAMPLER_SAMPLE_RATE;
    hdr.byte_rate = SAMPLER_SAMPLE_RATE * SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
    hdr.block_align = SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
    hdr.bits_per_sample = SAMPLER_BITS_PER_SAMPLE;
    memcpy(hdr.data_id, "data", 4);
    hdr.data_size = data_bytes;
    fwrite(&hdr, sizeof(hdr), 1, f);

    /* Write audio data from ring buffer */
    size_t pos = start_pos;
    size_t remaining = data_samples;
    while (remaining > 0) {
        size_t chunk = remaining;
        if (pos + chunk > total_samples)
            chunk = total_samples - pos;
        fwrite(skipback_buffer + pos, sizeof(int16_t), chunk, f);
        pos = (pos + chunk) % total_samples;
        remaining -= chunk;
    }

    fclose(f);

    uint32_t frames = (uint32_t)(data_samples / SAMPLER_NUM_CHANNELS);
    char msg[256];
    snprintf(msg, sizeof(msg), "Skipback: saved %s (%.1f sec)",
             path, (float)frames / SAMPLER_SAMPLE_RATE);
    s_host.log(msg);

    skipback_overlay_timeout = SKIPBACK_OVERLAY_FRAMES;
    s_host.overlay_sync();
    s_host.announce("Skipback saved");
    __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
    return NULL;
}

void skipback_trigger_save(void) {
    if (__atomic_load_n(&skipback_saving, __ATOMIC_ACQUIRE)) {
        s_host.announce("Skipback already saving");
        return;
    }
    if (!skipback_buffer) {
        s_host.announce("Skipback not available");
        return;
    }
    __atomic_store_n(&skipback_saving, 1, __ATOMIC_RELEASE);
    __sync_synchronize();

    s_host.announce("Saving skipback");

    pthread_t t;
    if (pthread_create(&t, NULL, skipback_writer_func, NULL) != 0) {
        s_host.log("Skipback: failed to create writer thread");
        s_host.announce("Skipback failed");
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return;
    }
    pthread_detach(t);
    s_host.log("Skipback: saving last 30 seconds...");
}

/* ============================================================================
 * VU Meter
 * ============================================================================ */

void sampler_update_vu(void) {
    if (!sampler_fullscreen_active) return;

    int16_t *audio = NULL;
    uint8_t *gmmap = s_host.global_mmap_addr ? *s_host.global_mmap_addr : NULL;
    uint8_t *hmmap = s_host.hardware_mmap_addr ? *s_host.hardware_mmap_addr : NULL;

    if (sampler_source == SAMPLER_SOURCE_RESAMPLE && gmmap) {
        audio = (int16_t *)(gmmap + SAMPLER_AUDIO_OUT_OFFSET);
    } else if (sampler_source == SAMPLER_SOURCE_MOVE_INPUT && hmmap) {
        audio = (int16_t *)(hmmap + SAMPLER_AUDIO_IN_OFFSET);
    }

    if (!audio) return;

    /* Scan 128 stereo frames, find peak absolute value */
    int16_t frame_peak = 0;
    for (int i = 0; i < SAMPLER_FRAMES_PER_BLOCK * 2; i++) {
        int16_t val = audio[i];
        if (val < 0) val = -val;
        if (val > frame_peak) frame_peak = val;
    }

    /* Peak hold and decay */
    if (frame_peak >= sampler_vu_peak) {
        sampler_vu_peak = frame_peak;
        sampler_vu_hold_frames = SAMPLER_VU_HOLD_DURATION;
    } else if (sampler_vu_hold_frames > 0) {
        sampler_vu_hold_frames--;
    } else {
        int16_t decayed = sampler_vu_peak - SAMPLER_VU_DECAY_RATE;
        sampler_vu_peak = (decayed < 0) ? 0 : decayed;
    }
}
