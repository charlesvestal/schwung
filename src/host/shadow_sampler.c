/* shadow_sampler.c - Quantized sampler and skipback subsystem
 * Extracted from schwung_shim.c for maintainability.
 *
 * This module handles:
 *   - Quantized sampler (Shift+Sample): record audio to WAV
 *   - Skipback (Shift+Capture): save last 30 seconds of audio
 *   - MIDI clock BPM measurement
 *   - VU metering for sampler UI */

#define _GNU_SOURCE
#include "shadow_sampler.h"
#include "shadow_transport.h"
#include "shim_worker.h"
#include "shadow_constants.h"
#include "sampler_stem_path.h"
#include <semaphore.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <sys/resource.h>
#endif

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
/*
 * A FREE-RUNNING count of MIDI clock pulses (24 PPQN) since the last MIDI
 * Start, for anything that needs to know where the beat is.
 *
 * Deliberately not `sampler_clock_count`, which looks like it would do and
 * does not: that one only advances while sampler_state == SAMPLER_RECORDING
 * and is reset to 0 when recording begins. A caller wanting "where are we in
 * the bar" would read 0 forever with the sampler idle, which is almost always.
 */
int shadow_transport_pulses = 0;
int sampler_target_pulses = 0;
int sampler_bars_completed = 0;

/* Fallback timing (when no MIDI clock) */
int sampler_fallback_blocks = 0;
int sampler_fallback_target = 0;
int sampler_clock_received = 0;
int sampler_transport_playing = 0;  /* Set on MIDI Start (0xFA), cleared on Stop (0xFC) */
int sampler_external_stop_only = 0; /* When set, ignore MIDI Stop auto-kill — only explicit stop_recording() works */

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

/* Diagnostic: max sample magnitude observed during the active recording.
 * Reset on start_recording, logged on stop. Helps tell silent-capture from
 * silent-output (e.g. did MOVE_INPUT actually deliver audio at all). */
static int32_t sampler_recording_max_peak = 0;
static uint64_t sampler_recording_blocks_captured = 0;

/* Full-screen mode flag */
int sampler_fullscreen_active = 0;

/* Fade-in ramp to avoid click at recording start */
#define SAMPLER_FADE_SAMPLES 128  /* ~3ms at 44.1kHz — short, click-free */
static int sampler_fade_in_remaining = 0;

/* Preroll-to-recording trim: frames captured during preroll that need trimming */
static uint32_t sampler_preroll_frames_captured = 0;

/* Recording state */
static FILE *sampler_wav_file = NULL;
uint32_t sampler_samples_written = 0;
static char sampler_current_recording[256] = "";
static int16_t *sampler_ring_buffer = NULL;  /* allocated once in sampler_init */
static size_t sampler_ring_write_pos = 0;
static size_t sampler_ring_read_pos = 0;
static pthread_t sampler_writer_thread;
/* Capture (RT) signals the writer with sem_post — async-signal-safe and
 * never blocks, unlike the old mutex+cond pair the SCHED_OTHER writer
 * could hold while preempted (priority inversion at FIFO 90). */
static sem_t sampler_ring_sem;
static volatile int sampler_writer_running = 0;
static volatile int sampler_writer_should_exit = 0;

/* RT/worker start-stop handshake. io_busy covers the window between a
 * stop/cancel request and the worker finishing file I/O; RT start
 * requests are refused while set (a start would reset ring positions the
 * writer is still draining). pending_* tell sampler_worker_prepare what
 * the RT half armed. */
static volatile int sampler_io_busy = 0;
static volatile int sampler_pending_preroll = 0;
static volatile int sampler_pending_custom = 0;  /* 1 = path in pending buf, 2 = read cmd file */
static char sampler_pending_path[256] = "";
#define SAMPLER_CMD_PATH_FILE "/data/UserData/schwung/sampler_cmd_path.txt"

/* ---------------------------------------------------------------- stems ---
 * See shadow_sampler.h for what a stem IS and why there are five of them. */

const char *const sampler_stem_names[SAMPLER_STEM_COUNT] = {
    "Slot1", "Slot2", "Slot3", "Slot4", "Move"
};

typedef struct {
    int16_t *ring;                 /* SAMPLER_RING_BUFFER_SIZE, allocated once */
    volatile size_t write_pos;     /* RT producer */
    volatile size_t read_pos;      /* writer-thread consumer */
    FILE    *file;
    uint32_t samples_written;      /* frames, not samples — matches the master */
    char     path[300];
    /* No non-zero sample seen yet. The file is deleted at finalize when this
     * is still set, which is how an unloaded slot leaves nothing behind. */
    volatile int silent;
} sampler_stem_t;

static sampler_stem_t sampler_stems[SAMPLER_STEM_COUNT];

/* Mirrors shadow_control_t.save_stems. Written by the shim from the SPI
 * callback (a plain byte store), read by the worker and the writer thread. */
static volatile int sampler_stem_mode = SAVE_STEMS_MASTER;

/* The mode LATCHED for the take in flight. Set by sampler_worker_prepare and
 * used by everything downstream, so flipping the setting mid-recording cannot
 * produce a take that is half one shape and half the other. */
static int sampler_take_stem_mode = SAVE_STEMS_MASTER;

/* 1 once sampler_worker_prepare has stem files open and the RT capture may
 * write into the stem rings. Cleared before the files are closed. */
static volatile int sampler_stems_capturing = 0;

/* Skipback stem rolling buffers. Sized independently of the master — see
 * SKIPBACK_STEM_MAX_SECONDS. */
static int16_t *skipback_stem_buffer[SAMPLER_STEM_COUNT] = {0};
static volatile size_t skipback_stem_write_pos = 0;
static volatile int skipback_stem_buffer_full = 0;
static volatile int skipback_stem_seconds_actual = 0;
static volatile size_t skipback_stem_total_samples = 0;

/* Skipback state */
static int16_t *skipback_buffer = NULL;
static volatile size_t skipback_write_pos = 0;
static volatile int skipback_buffer_full = 0;
static int skipback_saving = 0;
static pthread_t skipback_writer_thread;
volatile int skipback_overlay_timeout = 0;

/* Runtime size of the rolling buffer (in samples per channel × frames).
 * Established by skipback_init(); may be changed by skipback_resize(). */
static volatile int skipback_seconds_actual = 0;
static volatile size_t skipback_total_samples = 0;  /* skipback_seconds_actual * SR * channels */
static pthread_mutex_t skipback_resize_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Clamp/snap a requested seconds value to a sane range. */
static int skipback_clamp_seconds(int seconds) {
    if (seconds <= 0) return SKIPBACK_DEFAULT_SECONDS;
    if (seconds > SKIPBACK_MAX_SECONDS) return SKIPBACK_MAX_SECONDS;
    return seconds;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

void sampler_init(const sampler_host_t *host, float *sampler_set_tempo_ptr) {
    s_host = *host;
    s_set_tempo_ptr = sampler_set_tempo_ptr;
    /* Allocate the capture ring once (≈350 KB) so recording start never
     * mallocs on the SPI thread — the RT half only resets positions. */
    if (!sampler_ring_buffer) {
        sampler_ring_buffer = malloc(SAMPLER_RING_BUFFER_SIZE);
        if (!sampler_ring_buffer) {
            s_host.log("Sampler: ring buffer allocation failed — recording disabled");
        }
    }
    /* The five stem rings, allocated up front for the same reason: the RT
     * half of a recording start only resets positions. ~1.7 MB, resident
     * whether or not stems are ever switched on — the alternative is a
     * malloc on the path that arms a take, and a failure there would have to
     * be discovered in the middle of one. */
    for (int i = 0; i < SAMPLER_STEM_COUNT; i++) {
        if (!sampler_stems[i].ring) {
            sampler_stems[i].ring = malloc(SAMPLER_RING_BUFFER_SIZE);
            if (!sampler_stems[i].ring) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "Sampler: stem ring %s allocation failed — stems disabled",
                         sampler_stem_names[i]);
                s_host.log(msg);
            }
        }
    }
    sem_init(&sampler_ring_sem, 0, 0);
}

/* Every stem ring present? Stems are all-or-nothing: four of five files is a
 * worse outcome than the master alone, because the missing one is silent
 * rather than absent and nothing says which. */
static int sampler_stem_rings_ready(void) {
    for (int i = 0; i < SAMPLER_STEM_COUNT; i++)
        if (!sampler_stems[i].ring) return 0;
    return 1;
}

void sampler_set_stem_mode(int mode) {
    if (mode < SAVE_STEMS_MASTER || mode > SAVE_STEMS_BOTH) mode = SAVE_STEMS_MASTER;
    if (mode == sampler_stem_mode) return;
    int want_stems_before = SAVE_STEMS_WANTS_STEMS(sampler_stem_mode);
    sampler_stem_mode = mode;
    /* The skipback stem buffers appear and disappear with the setting, and
     * that is a calloc of tens of MB — posted to the worker, never done here.
     * This is called from the SPI callback. */
    if (want_stems_before != SAVE_STEMS_WANTS_STEMS(mode))
        shim_worker_post(SHIM_EVT_SKIPBACK_RESIZE);
}

int sampler_get_stem_mode(void) {
    return sampler_stem_mode;
}

/* ============================================================================
 * Disk I/O politeness
 *
 * NONE OF THIS IS ABOUT THREADS. The writers already run off the audio path:
 * they are created from the shim worker, which is SCHED_OTHER pinned to cores
 * 0-2 (shim_worker.c), and POSIX inherits both, so a writer is born there too.
 * The dropouts came from the SHAPE of the I/O, not from where it ran.
 *
 * A skipback save used to be one ~5 MB file. With stems it is SIX, ~32 MB at
 * the default 30 s, written back to back as fast as fwrite is willing to go.
 * That fills the page cache with dirty pages faster than this eMMC retires
 * them, and when the kernel finally forces writeback the whole system stalls
 * on it -- including the thread feeding the DAC, which never touched a file.
 * Priority does not help, because the stall is in the block layer and the
 * filesystem, not in the scheduler.
 *
 * Three things, cheapest first.
 * ============================================================================ */

/* Make this thread the last in the queue for both CPU and DISK.
 *
 * SCHED_OTHER and the core mask are already inherited from the worker; what is
 * NOT inherited from anywhere is I/O priority, and that is the one that
 * matters here. IOPRIO_CLASS_IDLE means the writer only gets the disk when
 * nothing else wants it -- exactly right for a save that may take an extra
 * second and for which nobody is waiting.
 *
 * Best-effort: every failure is ignored. A kernel without ioprio_set, or one
 * that refuses the nice value, must still produce the file. */
static void sampler_io_thread_be_polite(void) {
#ifdef __linux__
    /* IOPRIO_WHO_PROCESS = 1, IOPRIO_CLASS_IDLE = 3, class shift = 13. */
    syscall(SYS_ioprio_set, 1 /* who: this thread */, 0 /* current */,
            (3 << 13));
    setpriority(PRIO_PROCESS, 0, 10);
#endif
}

/* Bytes written between pauses, and the pause. 256 KB / 2 ms caps the dirty
 * rate at ~128 MB/s, which is well above what this device can actually retire
 * -- the pause is not a throttle, it is a yield point that stops one thread
 * owning the block queue for the length of a 32 MB burst. */
#define SAMPLER_IO_CHUNK_BYTES (256u * 1024u)
#define SAMPLER_IO_PAUSE_US    2000

/* fwrite `bytes` from `buf`, in chunks, starting writeback as we go and
 * dropping the pages behind us.
 *
 * sync_file_range hands each chunk to the block layer as soon as it is
 * written, so the dirty set never grows to the size of the file; fadvise
 * DONTNEED then evicts what has been retired, so a 32 MB save does not push
 * everything else out of a small page cache. Without these two the entire file
 * lands at fclose, which is precisely the stall being fixed.
 *
 * Returns bytes written. Writer threads only -- this sleeps. */
static size_t sampler_write_paced(FILE *f, const void *buf, size_t bytes) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t done = 0;
#ifdef __linux__
    int fd = fileno(f);
    off_t flushed_to = ftello(f);
    if (flushed_to < 0) flushed_to = 0;
#endif
    while (done < bytes) {
        size_t n = bytes - done;
        if (n > SAMPLER_IO_CHUNK_BYTES) n = SAMPLER_IO_CHUNK_BYTES;
        size_t got = fwrite(p + done, 1, n, f);
        done += got;
        if (got != n) break;      /* short write: caller sees the count */
#ifdef __linux__
        /* fflush first: sync_file_range works on the DESCRIPTOR, and what is
         * still sitting in the FILE buffer is invisible to it. */
        fflush(f);
        off_t now = ftello(f);
        if (now > flushed_to) {
            sync_file_range(fd, flushed_to, now - flushed_to,
                            SYNC_FILE_RANGE_WRITE);
            posix_fadvise(fd, flushed_to, now - flushed_to, POSIX_FADV_DONTNEED);
            flushed_to = now;
        }
#endif
        if (done < bytes) usleep(SAMPLER_IO_PAUSE_US);
    }
    return done;
}

/* Give a path to ableton:users so Move's UI can see it. The shim runs as root
 * (setuid), so what we create is owned by root and Move's UI, running as
 * ableton, would not find it.
 *
 * A DIRECT chown(2), not a forked `chown` process. Forking from a shim
 * LD_PRELOADed into MoveOriginal duplicates that whole address space, and this
 * used to run ONCE per save; with stems it runs six times in a row at the end
 * of the write burst. The uid/gid are taken from the containing directory,
 * which the directory-creation path already chowned -- so no name lookup, no
 * NSS, and no assumption about what "ableton" resolves to on this device.
 *
 * Falls back to the forked form when the parent cannot be stat'd, which is the
 * only case the copied ownership is unavailable. */
static void chown_to_ableton(const char *path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    struct stat st;
    if (slash) {
        *slash = '\0';
        if (dir[0] && stat(dir, &st) == 0 && chown(path, st.st_uid, st.st_gid) == 0)
            return;
    }
    const char *argv[] = { "chown", "ableton:users", path, NULL };
    s_host.run_command(argv);
}

static void chown_to_ableton_recursive(const char *path) {
    const char *argv[] = { "chown", "-R", "ableton:users", path, NULL };
    s_host.run_command(argv);
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

/* Drop `preroll_frames` from the FRONT of an open WAV, in place: copy the
 * tail down over the head, truncate, and update *frames to what is left.
 * Header is NOT rewritten here — the caller stamps the final size.
 *
 * Extracted from sampler_worker_finalize so the stems get the identical trim
 * rather than a second implementation of it. Returns 1 if anything moved.
 *
 * Runs on the shim worker (SCHED_OTHER). The malloc and the seeks are the
 * reason this is not on the RT path. */
static int sampler_wav_trim_front(FILE *f, uint32_t preroll_frames, uint32_t *frames) {
    if (!f || preroll_frames == 0 || !frames) return 0;
    uint32_t bytes_per_frame = SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
    uint32_t preroll_bytes = preroll_frames * bytes_per_frame;
    uint32_t total_bytes = *frames * bytes_per_frame;
    if (preroll_bytes >= total_bytes) return 0;

    uint32_t keep_bytes = total_bytes - preroll_bytes;
    uint32_t keep_frames = *frames - preroll_frames;
    size_t header_size = sizeof(sampler_wav_header_t);

    /* Process in chunks to limit memory usage */
    #define TRIM_CHUNK_SIZE (44100 * 2 * 2)  /* ~1 second */
    int16_t *chunk = malloc(TRIM_CHUNK_SIZE);
    if (!chunk) return 0;
    uint32_t remaining = keep_bytes;
    uint32_t read_offset = (uint32_t)header_size + preroll_bytes;
    uint32_t write_offset = (uint32_t)header_size;
    while (remaining > 0) {
        uint32_t to_copy = remaining < TRIM_CHUNK_SIZE ? remaining : TRIM_CHUNK_SIZE;
        fseek(f, read_offset, SEEK_SET);
        size_t got = fread(chunk, 1, to_copy, f);
        if (got == 0) break;
        fseek(f, write_offset, SEEK_SET);
        /* Paced: the trim rewrites the WHOLE file, and finalize runs it once per
         * stem. Six unpaced rewrites at stop is the same burst the skipback save
         * was. */
        sampler_write_paced(f, chunk, got);
        read_offset += (uint32_t)got;
        write_offset += (uint32_t)got;
        remaining -= (uint32_t)got;
    }
    free(chunk);
    #undef TRIM_CHUNK_SIZE

    ftruncate(fileno(f), (off_t)(header_size + keep_bytes));
    *frames = keep_frames;
    return 1;
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

/* Drain one stem ring into its file. Same shape as the master drain below;
 * factored out only because it runs five times. A NULL file still advances
 * read_pos — a stem whose fopen failed must not back its ring up and stall
 * the RT producer for the rest of the take. */
static void sampler_stem_drain(sampler_stem_t *st) {
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;
    for (;;) {
        size_t write_pos = __atomic_load_n(&st->write_pos, __ATOMIC_ACQUIRE);
        size_t read_pos  = __atomic_load_n(&st->read_pos, __ATOMIC_ACQUIRE);
        size_t available = (write_pos >= read_pos)
                             ? (write_pos - read_pos)
                             : (buffer_samples - (read_pos - write_pos));
        if (available == 0) break;
        size_t to_end = buffer_samples - read_pos;
        size_t to_write = (available < to_end) ? available : to_end;
        if (st->file)
            fwrite(&st->ring[read_pos], sizeof(int16_t), to_write, st->file);
        st->samples_written += to_write / SAMPLER_NUM_CHANNELS;
        __atomic_store_n(&st->read_pos, (read_pos + to_write) % buffer_samples,
                         __ATOMIC_RELEASE);
    }
}

static void *sampler_writer_thread_func(void *arg) {
    (void)arg;
    /* The sampler's writer is naturally paced -- it wakes on a semaphore and
     * writes ~250 ms of audio at a time -- so it does not need the chunking
     * below. It still wants the idle I/O class: with stems it drains six rings
     * per pass, and a take running while Move is playing is exactly when the
     * disk must not be contended. */
    sampler_io_thread_be_polite();
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;
    size_t write_chunk = SAMPLER_SAMPLE_RATE * SAMPLER_NUM_CHANNELS / 4;  /* ~250ms */
    int stems = SAVE_STEMS_WANTS_STEMS(sampler_take_stem_mode) && sampler_stem_rings_ready();

    while (1) {
        sem_wait(&sampler_ring_sem);
        int should_exit = sampler_writer_should_exit;
        /* Batch disk writes: skip back to sleep until ~250ms of audio has
         * accumulated (capture posts once per 2.9ms block). On exit drain
         * whatever remains.
         *
         * Keyed on the MASTER ring even when the master file is not being
         * kept: every ring advances in lockstep (one block per SPI frame,
         * from the same capture call), so it is the same measurement five
         * times over. */
        if (!should_exit && sampler_ring_available_read() < write_chunk) {
            continue;
        }

        size_t available = sampler_ring_available_read();
        while (available > 0) {
            size_t read_pos = __atomic_load_n(&sampler_ring_read_pos, __ATOMIC_ACQUIRE);
            size_t to_end = buffer_samples - read_pos;
            size_t to_write = (available < to_end) ? available : to_end;
            /* A Stems-only take still fills and drains this ring: it is what
             * feeds sampler_samples_written (the overlay's elapsed time) and
             * the fallback-duration accounting. Only the fwrite is skipped —
             * sampler_wav_file is NULL for that mode. */
            if (sampler_wav_file)
                fwrite(&sampler_ring_buffer[read_pos], sizeof(int16_t), to_write, sampler_wav_file);
            sampler_samples_written += to_write / SAMPLER_NUM_CHANNELS;
            __atomic_store_n(&sampler_ring_read_pos, (read_pos + to_write) % buffer_samples, __ATOMIC_RELEASE);
            available = sampler_ring_available_read();
        }

        if (stems)
            for (int i = 0; i < SAMPLER_STEM_COUNT; i++)
                sampler_stem_drain(&sampler_stems[i]);

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

/* Read tempo_bpm from Schwung settings file */
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
    /* 0. Internal transport (an overtake sequencer driving the clock).
     * Cable-0 (Move) clock is already covered by the measured-clock check
     * below, so only the internal source needs delegation. */
    if (shadow_transport_source() == TRANSPORT_SRC_INTERNAL) {
        float tbpm = shadow_transport_bpm();
        if (tbpm >= 20.0f) {
            if (source) *source = TEMPO_SOURCE_CLOCK;
            return tbpm;
        }
    }

    /* 1. Active MIDI clock */
    if (sampler_clock_active && sampler_measured_bpm >= 20.0f) {
        if (source) *source = TEMPO_SOURCE_CLOCK;
        return sampler_measured_bpm;
    }

    /* 1b. Internal transport's last tempo (movy sequencer stopped). Keep synced
     * params at movy's tempo instead of snapping back to the Set/default tempo,
     * so a synced LFO free-runs at the rate it was locked to. Ranks below an
     * actively-running clock (above) but above the Set tempo. */
    if (shadow_transport_last_source() == TRANSPORT_SRC_INTERNAL) {
        float lbpm = shadow_transport_last_bpm();
        if (lbpm >= 20.0f) {
            if (source) *source = TEMPO_SOURCE_LAST_CLOCK;
            return lbpm;
        }
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

/* ============================================================================
 * Recording start/stop — RT/worker split (RT-1/RT-2 in
 * docs/plans/2026-06-11-codebase-cleanup-review.md)
 *
 * The RT halves run on the SPI thread: flip state, reset counters/ring
 * positions, post an event. Capture into the pre-allocated ring begins on
 * the very next block, so the triggering note's own audio is recorded.
 * The worker halves do every file operation (mkdir/fopen/header/writer
 * thread on start; join/trim/close on stop). Worker events execute in
 * posted order, so a start→stop race inside one worker tick resolves
 * serially: prepare runs, then finalize/cancel cleans up.
 * ============================================================================ */

/* Shared RT-side arm: reset the capture machinery. Caller sets state. */
static int sampler_arm_common(void) {
    if (sampler_writer_running || sampler_io_busy) return -1;
    if (!sampler_ring_buffer) return -1;
    __atomic_store_n(&sampler_ring_write_pos, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&sampler_ring_read_pos, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < SAMPLER_STEM_COUNT; i++) {
        __atomic_store_n(&sampler_stems[i].write_pos, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&sampler_stems[i].read_pos, 0, __ATOMIC_RELEASE);
        sampler_stems[i].samples_written = 0;
        sampler_stems[i].silent = 1;
        sampler_stems[i].path[0] = '\0';
    }
    /*
     * LATCH THE MODE HERE, on the RT arm, and open the stem capture gate with
     * it.
     *
     * Both used to happen in sampler_worker_prepare, which is up to ~200 ms
     * later: sampler_state is RECORDING the moment this returns, so the master
     * ring starts filling immediately while the stem rings did not — every
     * take began with a few hundred milliseconds present in the master and
     * missing from the stems, and since the stems are trimmed by the master's
     * preroll count they would have stayed offset by it for the whole take.
     *
     * Nothing here needs the worker: the stem rings are allocated once in
     * sampler_init and their positions are reset just below, so they are ready
     * before the file that will drain them exists. Two seconds of ring against
     * a ~200 ms wait is ample headroom.
     *
     * Stems are a RESAMPLE-path feature — with the source on Move Input the
     * sampler is recording the line-in, which has no per-slot structure to
     * split. The worker reports that in the log; this half cannot, because it
     * runs on the SPI callback.
     */
    sampler_take_stem_mode =
        (sampler_source == SAMPLER_SOURCE_RESAMPLE) ? sampler_stem_mode : SAVE_STEMS_MASTER;
    sampler_stems_capturing = SAVE_STEMS_WANTS_STEMS(sampler_take_stem_mode) &&
                              sampler_stem_rings_ready();

    sampler_writer_should_exit = 0;
    sampler_samples_written = 0;
    sampler_preroll_frames_captured = 0;
    sampler_clock_count = 0;
    sampler_bars_completed = 0;
    sampler_clock_received = 0;
    sampler_fallback_blocks = 0;
    sampler_fade_in_remaining = SAMPLER_FADE_SAMPLES;
    sampler_recording_max_peak = 0;
    sampler_recording_blocks_captured = 0;
    return 0;
}

int sampler_request_start(int with_preroll) {
    if (sampler_arm_common() != 0) return -1;
    int bars = sampler_duration_options[sampler_duration_index];
    sampler_pending_custom = 0;
    sampler_pending_preroll = with_preroll ? 1 : 0;
    if (with_preroll) {
        sampler_preroll_clock_count = 0;
        sampler_preroll_fallback_blocks = 0;
        sampler_preroll_target_pulses = bars * 4 * 24;
        /* Fallback timer needs BPM, which may read Settings/Set files —
         * sampler_worker_prepare sets it (0 disables until then; the gap
         * is ≤200ms against a seconds-scale timer). */
        sampler_preroll_fallback_target = 0;
        sampler_state = SAMPLER_PREROLL;
        sampler_fullscreen_active = 1;
        sampler_overlay_active = 1;
        s_host.log("Sampler: preroll armed");
    } else {
        sampler_target_pulses = (bars > 0) ? bars * 4 * 24 : 0;
        sampler_fallback_target = 0;  /* set by sampler_worker_prepare */
        sampler_state = SAMPLER_RECORDING;
        sampler_overlay_active = 1;
        sampler_overlay_timeout = 0;
    }
    s_host.overlay_sync();
    shim_worker_post(SHIM_EVT_SAMPLER_PREP);
    return 0;
}

int sampler_request_start_custom(const char *path_or_null) {
    if (sampler_arm_common() != 0) return -1;
    sampler_pending_preroll = 0;
    if (path_or_null && path_or_null[0]) {
        snprintf(sampler_pending_path, sizeof(sampler_pending_path), "%s", path_or_null);
        sampler_pending_custom = 1;
    } else {
        sampler_pending_path[0] = '\0';
        sampler_pending_custom = 2;  /* worker reads SAMPLER_CMD_PATH_FILE */
    }
    /* Unlimited duration; external JS drives the stop. Overlay untouched —
     * this path is driven by modules, not the sampler UI. */
    sampler_target_pulses = 0;
    sampler_fallback_target = 0;
    sampler_state = SAMPLER_RECORDING;
    shim_worker_post(SHIM_EVT_SAMPLER_PREP);
    return 0;
}

void sampler_request_stop(void) {
    if (sampler_state == SAMPLER_PREROLL) {
        s_host.log("Sampler: preroll cancelled");
        sampler_state = SAMPLER_ARMED;
        sampler_io_busy = 1;
        s_host.overlay_sync();
        shim_worker_post(SHIM_EVT_SAMPLER_CANCEL);
        return;
    }
    if (sampler_state != SAMPLER_RECORDING && sampler_state != SAMPLER_PAUSED) return;
    /* Capture stops at this exact frame (the capture predicate excludes
     * FINALIZING); the worker drains the ring into the file and closes it.
     * sampler_get_state() keeps reporting RECORDING until then so JS can
     * poll host_sampler_is_recording() for file completion. */
    sampler_state = SAMPLER_FINALIZING;
    sampler_io_busy = 1;
    shim_worker_post(SHIM_EVT_SAMPLER_FINALIZE);
}

/* ---- worker halves (shim worker thread — file I/O lives here) ---------- */

static void sampler_worker_discard_stems(void);

static void sampler_worker_abort_start(const char *why) {
    s_host.log(why);
    s_host.announce("Recording failed");
    sampler_stems_capturing = 0;
    if (sampler_wav_file) {
        fclose(sampler_wav_file);
        sampler_wav_file = NULL;
    }
    sampler_worker_discard_stems();
    sampler_take_stem_mode = SAVE_STEMS_MASTER;
    sampler_state = SAMPLER_IDLE;
    sampler_fullscreen_active = 0;
    s_host.overlay_sync();
}

/* Build the auto-named output path (date dir + bpm filename) and mkdir. */
static int sampler_worker_build_auto_path(void) {
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm_info = localtime_r(&now, &tm_buf);
    if (!tm_info) return -1;
    char date_subdir[32];
    if (strftime(date_subdir, sizeof(date_subdir), "%Y-%m-%d", tm_info) == 0) return -1;
    char recording_dir[256];
    snprintf(recording_dir, sizeof(recording_dir), "%s/%s", SAMPLER_RECORDINGS_DIR, date_subdir);
    struct stat st;
    if (stat(recording_dir, &st) != 0) {
        const char *mkdir_argv[] = { "mkdir", "-p", recording_dir, NULL };
        s_host.run_command(mkdir_argv);
        chown_to_ableton_recursive(recording_dir);
    }
    tempo_source_t src;
    float bpm_for_name = sampler_get_bpm(&src);
    int bpm_int = (int)(bpm_for_name + 0.5f);
    snprintf(sampler_current_recording, sizeof(sampler_current_recording), "%s/sample_%04d%02d%02d_%02d%02d%02d_%dbpm.wav",
             recording_dir,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, bpm_int);
    return 0;
}

/* Open the five stem files beside the master and write their headers.
 * Returns the number opened. A stem that fails to open is logged and left
 * NULL: its ring still drains (see sampler_stem_drain), so one bad file
 * cannot stall the take. */
static int sampler_worker_open_stems(void) {
    if (!sampler_stem_rings_ready()) {
        s_host.log("Sampler: stem rings unavailable — recording master only");
        return 0;
    }
    int opened = 0;
    for (int i = 0; i < SAMPLER_STEM_COUNT; i++) {
        sampler_stem_t *st = &sampler_stems[i];
        sampler_stem_path_build(st->path, sizeof(st->path),
                                sampler_current_recording, sampler_stem_names[i]);
        st->file = fopen(st->path, "wb");
        if (!st->file) {
            char msg[380];
            snprintf(msg, sizeof(msg), "Sampler: failed to open stem file: %s", st->path);
            s_host.log(msg);
            st->path[0] = '\0';
            continue;
        }
        sampler_write_wav_header(st->file, 0);
        opened++;
    }
    return opened;
}

/* Finish one stem: trim the preroll, stamp the header, close, and DELETE the
 * file if the stem never carried audio. Returns 1 if a file was kept. */
static int sampler_worker_finish_stem(sampler_stem_t *st, uint32_t preroll_frames,
                                      uint32_t master_frames, const char *name) {
    if (!st->file) return 0;
    if (st->silent || st->samples_written == 0) {
        fclose(st->file);
        st->file = NULL;
        if (st->path[0]) unlink(st->path);
        return 0;
    }
    /*
     * Six streams share one ring size and one drain pass, so they should end
     * the same length. They can still diverge: each capture drops its block
     * independently when its own ring is full, so sustained write backpressure
     * could drop a block from one and not another — and a stem that is one
     * block short is offset against the master for the rest of the file, which
     * is exactly the artifact nobody would attribute to the writer.
     *
     * Not repaired here, because padding or truncating guesses WHERE the gap
     * was. Reported instead, so a desync is a log line rather than a mystery.
     */
    if (master_frames && st->samples_written != master_frames) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Sampler: stem %s is %d frames off the master (%u vs %u) — "
                 "writer fell behind; stem is not sample-aligned",
                 name, (int)st->samples_written - (int)master_frames,
                 st->samples_written, master_frames);
        s_host.log(msg);
    }

    if (preroll_frames > 0)
        sampler_wav_trim_front(st->file, preroll_frames, &st->samples_written);
    uint32_t data_size = st->samples_written * SAMPLER_NUM_CHANNELS *
                         (SAMPLER_BITS_PER_SAMPLE / 8);
    sampler_write_wav_header(st->file, data_size);
    fclose(st->file);
    st->file = NULL;
    chown_to_ableton(st->path);
    return 1;
}

/* Close and delete every open stem file without keeping any of them. Used by
 * the preroll-cancel path, which throws the whole take away. */
static void sampler_worker_discard_stems(void) {
    for (int i = 0; i < SAMPLER_STEM_COUNT; i++) {
        sampler_stem_t *st = &sampler_stems[i];
        if (st->file) { fclose(st->file); st->file = NULL; }
        if (st->path[0]) { unlink(st->path); st->path[0] = '\0'; }
    }
}

void sampler_worker_prepare(void) {
    int custom = sampler_pending_custom;
    int preroll = sampler_pending_preroll;
    sampler_pending_custom = 0;

    if (custom) {
        char path_buf[256] = "";
        if (custom == 1) {
            snprintf(path_buf, sizeof(path_buf), "%s", sampler_pending_path);
        } else {
            FILE *pf = fopen(SAMPLER_CMD_PATH_FILE, "r");
            if (pf) {
                if (fgets(path_buf, sizeof(path_buf), pf)) {
                    char *nl = strchr(path_buf, '\n');
                    if (nl) *nl = '\0';
                }
                fclose(pf);
            }
        }
        if (!path_buf[0]) {
            sampler_worker_abort_start("Sampler: start failed - no output path");
            return;
        }
        /* Create the parent directory */
        char dir_buf[256];
        snprintf(dir_buf, sizeof(dir_buf), "%s", path_buf);
        char *last_slash = strrchr(dir_buf, '/');
        if (last_slash) {
            *last_slash = '\0';
            struct stat st;
            if (stat(dir_buf, &st) != 0) {
                const char *mkdir_argv[] = { "mkdir", "-p", dir_buf, NULL };
                s_host.run_command(mkdir_argv);
                chown_to_ableton_recursive(dir_buf);
            }
        }
        snprintf(sampler_current_recording, sizeof(sampler_current_recording),
                 "%s", path_buf);
    } else {
        if (sampler_worker_build_auto_path() != 0) {
            sampler_worker_abort_start("Sampler: start failed - path build");
            return;
        }
        /* Compute fallback timers now that BPM file reads are safe. This
         * call also warms sampler_get_bpm's settings cache, so later RT
         * callers (preroll completion) never touch the filesystem. */
        int bars = sampler_duration_options[sampler_duration_index];
        if (bars > 0) {
            tempo_source_t src;
            float bpm = sampler_get_bpm(&src);
            float seconds = bars * 4.0f * 60.0f / bpm;
            int target = (int)(seconds * 44100.0f / 128.0f);
            if (preroll) sampler_preroll_fallback_target = target;
            else         sampler_fallback_target = target;
        }
    }

    /* sampler_take_stem_mode was LATCHED on the RT arm (sampler_arm_common) —
     * this half consumes it and must not re-read the live setting, or a take
     * armed as Stems could open master-only files for rings that are already
     * filling as stems. Report the one case the RT half could not log. */
    if (sampler_source != SAMPLER_SOURCE_RESAMPLE &&
        sampler_stem_mode != SAVE_STEMS_MASTER)
        s_host.log("Sampler: source is Move Input — stems not available, recording master");

    if (SAVE_STEMS_WANTS_MASTER(sampler_take_stem_mode)) {
        sampler_wav_file = fopen(sampler_current_recording, "wb");
        if (!sampler_wav_file) {
            char msg[300];
            snprintf(msg, sizeof(msg), "Sampler: failed to open WAV file: %s",
                     sampler_current_recording);
            sampler_worker_abort_start(msg);
            return;
        }
        sampler_write_wav_header(sampler_wav_file, 0);
    }

    int stems_opened = 0;
    if (SAVE_STEMS_WANTS_STEMS(sampler_take_stem_mode)) {
        stems_opened = sampler_worker_open_stems();
        if (stems_opened == 0 && !SAVE_STEMS_WANTS_MASTER(sampler_take_stem_mode)) {
            /* Stems-only, and not one stem file opened: there is nowhere for
             * this take to go. Failing here is better than recording into
             * five NULL files and announcing a save. */
            sampler_worker_abort_start("Sampler: no stem file could be opened");
            return;
        }
    }

    if (pthread_create(&sampler_writer_thread, NULL, sampler_writer_thread_func, NULL) != 0) {
        if (sampler_wav_file) { fclose(sampler_wav_file); sampler_wav_file = NULL; }
        sampler_worker_discard_stems();
        sampler_worker_abort_start("Sampler: failed to create writer thread");
        return;
    }
    sampler_writer_running = 1;
    /* The gate was opened on the RT arm. Prepare can only CLOSE it — when not
     * one stem file opened, the rings would fill with nothing draining them. */
    if (stems_opened == 0) sampler_stems_capturing = 0;

    char msg[380];
    const char *what = SAVE_STEMS_WANTS_MASTER(sampler_take_stem_mode)
                         ? (stems_opened ? "master + stems" : "master")
                         : "stems";
    if (custom)
        snprintf(msg, sizeof(msg), "Sampler: recording %s to custom path -> %s",
                 what, sampler_current_recording);
    else if (preroll)
        snprintf(msg, sizeof(msg), "Sampler: preroll recording %s -> %s",
                 what, sampler_current_recording);
    else
        snprintf(msg, sizeof(msg), "Sampler: recording %s started -> %s",
                 what, sampler_current_recording);
    s_host.log(msg);
}

void sampler_tick_preroll(void) {
    if (sampler_state != SAMPLER_PREROLL) return;

    sampler_preroll_fallback_blocks++;
    if (sampler_preroll_fallback_target > 0 && sampler_preroll_fallback_blocks >= sampler_preroll_fallback_target) {
        char pmsg[128];
        snprintf(pmsg, sizeof(pmsg), "Sampler: preroll complete (fallback timer, %u preroll frames to trim)",
                 sampler_preroll_frames_captured);
        s_host.log(pmsg);
        /* Recording machinery already running — just flip state */
        sampler_state = SAMPLER_RECORDING;
        sampler_clock_count = 0;
        sampler_bars_completed = 0;
        sampler_clock_received = 0;
        sampler_fallback_blocks = 0;
        int bars = sampler_duration_options[sampler_duration_index];
        if (bars > 0) {
            sampler_target_pulses = bars * 4 * 24;
            tempo_source_t tsrc;
            float tbpm = sampler_get_bpm(&tsrc);
            float secs = bars * 4.0f * 60.0f / tbpm;
            sampler_fallback_target = (int)(secs * 44100.0f / 128.0f);
        } else {
            sampler_target_pulses = 0;
            sampler_fallback_target = 0;
        }
        sampler_overlay_active = 1;
        sampler_overlay_timeout = 0;
        s_host.overlay_sync();
    }
}

int sampler_get_state(void) {
    /* FINALIZING mirrors as RECORDING: the JS contract is "recording until
     * the file is complete on disk" (modules stop, poll, then read). */
    if (sampler_state == SAMPLER_FINALIZING) return (int)SAMPLER_RECORDING;
    return (int)sampler_state;
}

/* sampler_start_recording / sampler_start_recording_to / sampler_start_preroll
 * were replaced by the sampler_request_* + sampler_worker_prepare split above. */

void sampler_pause_recording(void) {
    if (sampler_state != SAMPLER_RECORDING) return;
    s_host.log("Sampler: pausing recording");
    sampler_state = SAMPLER_PAUSED;
    s_host.overlay_sync();
}

void sampler_resume_recording(void) {
    if (sampler_state != SAMPLER_PAUSED) return;
    s_host.log("Sampler: resuming recording");

    /* Re-apply fade-in ramp to avoid click at resume boundary */
    sampler_fade_in_remaining = SAMPLER_FADE_SAMPLES;

    sampler_state = SAMPLER_RECORDING;
    s_host.overlay_sync();
}

/* Worker half of preroll cancel: tear down whatever sampler_worker_prepare
 * created (events are ordered, so prepare has already run if it was queued)
 * and delete the partial file. State was already set to ARMED by the RT
 * half. The ring buffer is persistent — never freed. */
void sampler_worker_cancel_preroll(void) {
    if (sampler_writer_running) {
        sampler_writer_should_exit = 1;
        sem_post(&sampler_ring_sem);
        pthread_join(sampler_writer_thread, NULL);
        sampler_writer_running = 0;
    }
    sampler_stems_capturing = 0;
    if (sampler_wav_file) {
        fclose(sampler_wav_file);
        sampler_wav_file = NULL;
    }
    sampler_worker_discard_stems();
    if (sampler_current_recording[0]) {
        unlink(sampler_current_recording);
        sampler_current_recording[0] = '\0';
    }
    sampler_take_stem_mode = SAVE_STEMS_MASTER;
    sampler_io_busy = 0;
    s_host.overlay_sync();
}

/* Worker half of stop: join the writer (it drains the ring first), trim
 * preroll frames, finalize the header, close. This used to run on the SPI
 * thread — a guaranteed multi-hundred-ms audio stall on long recordings. */
void sampler_worker_finalize(void) {
    if (!sampler_writer_running) {
        /* Prepare failed (or never ran) — nothing to finalize. */
        if (sampler_state == SAMPLER_FINALIZING) sampler_state = SAMPLER_IDLE;
        sampler_io_busy = 0;
        s_host.overlay_sync();
        return;
    }

    {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Sampler: stopping recording (source=%d blocks=%llu max_peak=%d)",
                 (int)sampler_source,
                 (unsigned long long)sampler_recording_blocks_captured,
                 (int)sampler_recording_max_peak);
        s_host.log(msg);
    }

    /* Signal writer thread to exit; it drains the ring before exiting. */
    sampler_writer_should_exit = 1;
    sem_post(&sampler_ring_sem);

    pthread_join(sampler_writer_thread, NULL);
    sampler_writer_running = 0;

    /* Stems must stop being written before their files are touched. The RT
     * capture predicate already excludes FINALIZING, but this flag is what a
     * concurrent block-in-flight tests, and it is cleared on this side of the
     * writer join above. */
    sampler_stems_capturing = 0;

    int keep_master = SAVE_STEMS_WANTS_MASTER(sampler_take_stem_mode);

    /* The master's length BEFORE its preroll trim, which is what the stems
     * should match — they are trimmed by the same count afterwards. Read here
     * because the trim below overwrites sampler_samples_written. Valid even
     * for a Stems-only take: the writer counts the master ring whether or not
     * it is writing it to a file. */
    uint32_t master_frames_raw = sampler_samples_written;

    /* Trim preroll frames from the front of the WAV file.
     * The file contains [preroll audio | actual recording].
     * We rewrite it to contain only [actual recording]. */
    if (sampler_wav_file && sampler_preroll_frames_captured > 0) {
        if (sampler_wav_trim_front(sampler_wav_file, sampler_preroll_frames_captured,
                                   &sampler_samples_written)) {
            char tmsg[128];
            snprintf(tmsg, sizeof(tmsg), "Sampler: trimmed %u preroll frames (%.1fms)",
                     sampler_preroll_frames_captured,
                     (float)sampler_preroll_frames_captured / SAMPLER_SAMPLE_RATE * 1000.0f);
            s_host.log(tmsg);
        }
    }

    /* Update WAV header with final size */
    if (sampler_wav_file) {
        uint32_t data_size = sampler_samples_written * SAMPLER_NUM_CHANNELS * (SAMPLER_BITS_PER_SAMPLE / 8);
        sampler_write_wav_header(sampler_wav_file, data_size);
        fclose(sampler_wav_file);
        sampler_wav_file = NULL;
        chown_to_ableton(sampler_current_recording);
    }

    /* Stems. Each is trimmed by the SAME preroll frame count as the master —
     * they were captured from the same block, in the same call, so a
     * per-stem count would be the same number computed five more times. */
    int stems_kept = 0;
    if (SAVE_STEMS_WANTS_STEMS(sampler_take_stem_mode)) {
        for (int i = 0; i < SAMPLER_STEM_COUNT; i++)
            stems_kept += sampler_worker_finish_stem(&sampler_stems[i],
                                                     sampler_preroll_frames_captured,
                                                     master_frames_raw,
                                                     sampler_stem_names[i]);
    }

    /* Ring buffer is persistent (allocated once in sampler_init). */

    char msg[320];
    if (keep_master && stems_kept)
        snprintf(msg, sizeof(msg), "Sampler: saved %s + %d stem(s) (%u samples, %.1f sec)",
                 sampler_current_recording, stems_kept, sampler_samples_written,
                 (float)sampler_samples_written / SAMPLER_SAMPLE_RATE);
    else if (stems_kept)
        snprintf(msg, sizeof(msg), "Sampler: saved %d stem(s) for %s (%u samples, %.1f sec)",
                 stems_kept, sampler_current_recording, sampler_samples_written,
                 (float)sampler_samples_written / SAMPLER_SAMPLE_RATE);
    else
        snprintf(msg, sizeof(msg), "Sampler: saved %s (%u samples, %.1f sec)",
                 sampler_current_recording, sampler_samples_written,
                 (float)sampler_samples_written / SAMPLER_SAMPLE_RATE);
    s_host.log(msg);

    /* A Stems-only take that produced no stem at all kept NOTHING — every
     * slot was silent and Move's audio was inside them. Say so: a silent
     * "Sample saved" for a take with no file behind it is the failure this
     * whole subsystem is most likely to produce, and the least visible. */
    if (!keep_master && stems_kept == 0)
        s_host.log("Sampler: stems-only take produced no audio — nothing saved");

    sampler_current_recording[0] = '\0';
    sampler_take_stem_mode = SAVE_STEMS_MASTER;
    sampler_state = SAMPLER_IDLE;
    sampler_io_busy = 0;

    if (!keep_master && stems_kept == 0)      s_host.announce("Nothing to save");
    else if (stems_kept == 1)                 s_host.announce("Sample saved, 1 stem");
    else if (stems_kept > 1)                  { char a[48];
                                                snprintf(a, sizeof(a), "Sample saved, %d stems", stems_kept);
                                                s_host.announce(a); }
    else                                      s_host.announce("Sample saved");

    /* Keep fullscreen active for "saved" message, then timeout */
    sampler_overlay_active = 1;
    sampler_overlay_timeout = SAMPLER_OVERLAY_DONE_FRAMES;
    s_host.overlay_sync();
}

static void sampler_capture_audio_common(const int16_t *audio) {
    if ((sampler_state != SAMPLER_RECORDING && sampler_state != SAMPLER_PREROLL) || !sampler_ring_buffer) return;
    if (!audio) return;

    size_t samples_to_write = SAMPLER_FRAMES_PER_BLOCK * SAMPLER_NUM_CHANNELS;
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;

    /* Write to ring buffer if space available */
    if (sampler_ring_available_write() >= samples_to_write) {
        size_t write_pos = __atomic_load_n(&sampler_ring_write_pos, __ATOMIC_ACQUIRE);
        int32_t block_peak = 0;
        for (size_t i = 0; i < samples_to_write; i++) {
            int16_t sample = audio[i];
            /* Apply fade-in ramp on first block(s) to avoid click */
            if (sampler_fade_in_remaining > 0) {
                int pos = SAMPLER_FADE_SAMPLES - sampler_fade_in_remaining;
                sample = (int16_t)((int32_t)sample * pos / SAMPLER_FADE_SAMPLES);
                sampler_fade_in_remaining--;
            }
            int32_t mag = sample < 0 ? -(int32_t)sample : (int32_t)sample;
            if (mag > block_peak) block_peak = mag;
            sampler_ring_buffer[write_pos] = sample;
            write_pos = (write_pos + 1) % buffer_samples;
        }
        if (block_peak > sampler_recording_max_peak) sampler_recording_max_peak = block_peak;
        sampler_recording_blocks_captured++;
        __atomic_store_n(&sampler_ring_write_pos, write_pos, __ATOMIC_RELEASE);

        /* sem_post never blocks — no lock shared with the SCHED_OTHER
         * writer thread on this FIFO-90 path. */
        sem_post(&sampler_ring_sem);

        /* Track frames captured during preroll for later trimming */
        if (sampler_state == SAMPLER_PREROLL) {
            sampler_preroll_frames_captured += SAMPLER_FRAMES_PER_BLOCK;
        }
    }

    /* Fallback timeout (only during actual recording, not preroll) */
    if (sampler_state != SAMPLER_RECORDING) return;
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
            sampler_request_stop();
        }
    }
}

void sampler_capture_audio(void) {
    /* Select audio source from the SPI mailbox. Used by the MOVE_INPUT path
     * post-ioctl (fresh hardware input). The RESAMPLE path now goes through
     * sampler_capture_audio_from_buffer() so it captures at unity level. */
    const int16_t *audio = NULL;
    uint8_t *gmmap = s_host.global_mmap_addr ? *s_host.global_mmap_addr : NULL;
    uint8_t *hmmap = s_host.hardware_mmap_addr ? *s_host.hardware_mmap_addr : NULL;

    if (sampler_source == SAMPLER_SOURCE_RESAMPLE && gmmap) {
        audio = (const int16_t *)(gmmap + SAMPLER_AUDIO_OUT_OFFSET);
    } else if (sampler_source == SAMPLER_SOURCE_MOVE_INPUT && hmmap) {
        audio = (const int16_t *)(hmmap + SAMPLER_AUDIO_IN_OFFSET);
    }
    sampler_capture_audio_common(audio);
}

void sampler_capture_audio_from_buffer(const int16_t *src) {
    /* Capture from caller-provided unity-level buffer (RESAMPLE path). */
    if (sampler_source != SAMPLER_SOURCE_RESAMPLE) return;
    sampler_capture_audio_common(src);
}

/* Write one block into one stem ring. Mirrors sampler_capture_audio_common's
 * ring write; `fade` is the master's ramp counter SNAPSHOT rather than the
 * live variable, because the master half is the one that consumes it and all
 * six streams must be ramped identically. */
static void sampler_stem_write(sampler_stem_t *st, const int16_t *audio, int fade) {
    size_t samples_to_write = SAMPLER_FRAMES_PER_BLOCK * SAMPLER_NUM_CHANNELS;
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;

    size_t write_pos = __atomic_load_n(&st->write_pos, __ATOMIC_ACQUIRE);
    size_t read_pos  = __atomic_load_n(&st->read_pos, __ATOMIC_ACQUIRE);
    size_t avail = (write_pos >= read_pos)
                     ? (buffer_samples - (write_pos - read_pos) - 1)
                     : (read_pos - write_pos - 1);
    if (avail < samples_to_write) return;

    int silent = 1;
    for (size_t i = 0; i < samples_to_write; i++) {
        int16_t sample = audio ? audio[i] : 0;
        if (fade > 0) {
            int pos = SAMPLER_FADE_SAMPLES - fade;
            sample = (int16_t)((int32_t)sample * pos / SAMPLER_FADE_SAMPLES);
            fade--;
        }
        if (sample) silent = 0;
        st->ring[write_pos] = sample;
        write_pos = (write_pos + 1) % buffer_samples;
    }
    if (!silent) st->silent = 0;
    __atomic_store_n(&st->write_pos, write_pos, __ATOMIC_RELEASE);
}

void sampler_capture_stems(const int16_t *const *stems, int count) {
    if (!sampler_stems_capturing || !stems) return;
    if (sampler_state != SAMPLER_RECORDING && sampler_state != SAMPLER_PREROLL) return;
    /* Snapshot before the master half decrements it. This is why the header
     * requires stems to be captured FIRST for a given block. */
    int fade = sampler_fade_in_remaining;
    if (count > SAMPLER_STEM_COUNT) count = SAMPLER_STEM_COUNT;
    for (int i = 0; i < count; i++) {
        if (!sampler_stems[i].ring) continue;
        /* A NULL entry writes SILENCE rather than skipping the block. Skipping
         * would shorten that one stem by a block and desynchronise it from the
         * others for the rest of the take — the same failure the align capture
         * records under "a starved frame is captured as SILENCE, not skipped". */
        sampler_stem_write(&sampler_stems[i], stems[i], fade);
    }
    /* Stems past `count` still need their block, or an inactive slot's file
     * would run short against the rest. */
    for (int i = count; i < SAMPLER_STEM_COUNT; i++) {
        if (!sampler_stems[i].ring) continue;
        sampler_stem_write(&sampler_stems[i], NULL, fade);
    }
}

void sampler_amend_audio(const int16_t *audio) {
    if (sampler_state != SAMPLER_RECORDING || !sampler_ring_buffer || !audio) return;
    if (sampler_source != SAMPLER_SOURCE_RESAMPLE) return;

    size_t block_samples = SAMPLER_FRAMES_PER_BLOCK * SAMPLER_NUM_CHANNELS;
    size_t buffer_samples = SAMPLER_RING_BUFFER_SAMPLES * SAMPLER_NUM_CHANNELS;
    size_t wp = __atomic_load_n(&sampler_ring_write_pos, __ATOMIC_ACQUIRE);
    /* Mix into the block that was just written by sampler_capture_audio */
    size_t start = (wp + buffer_samples - block_samples) % buffer_samples;
    for (size_t i = 0; i < block_samples; i++) {
        size_t pos = (start + i) % buffer_samples;
        int32_t sum = (int32_t)sampler_ring_buffer[pos] + (int32_t)audio[i];
        if (sum > 32767) sum = 32767;
        if (sum < -32768) sum = -32768;
        sampler_ring_buffer[pos] = (int16_t)sum;
    }
}

void sampler_on_clock(uint8_t status) {
    if (status == 0xF8) {
        /* MIDI Clock tick */
        sampler_clock_active = 1;
        sampler_clock_stale_frames = 0;
        sampler_clock_beat_ticks++;
        shadow_transport_pulses++;

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
                char pmsg[128];
                snprintf(pmsg, sizeof(pmsg), "Sampler: preroll complete via MIDI clock (%u preroll frames to trim)",
                         sampler_preroll_frames_captured);
                s_host.log(pmsg);
                /* Recording machinery already running from preroll start —
                 * just flip state and init recording counters */
                sampler_state = SAMPLER_RECORDING;
                sampler_clock_count = 0;
                sampler_bars_completed = 0;
                sampler_clock_received = 0;
                sampler_fallback_blocks = 0;
                int bars = sampler_duration_options[sampler_duration_index];
                if (bars > 0) {
                    sampler_target_pulses = bars * 4 * 24;
                    tempo_source_t tsrc;
                    float tbpm = sampler_get_bpm(&tsrc);
                    float secs = bars * 4.0f * 60.0f / tbpm;
                    sampler_fallback_target = (int)(secs * 44100.0f / 128.0f);
                } else {
                    sampler_target_pulses = 0;
                    sampler_fallback_target = 0;
                }
                sampler_overlay_active = 1;
                sampler_overlay_timeout = 0;
                s_host.overlay_sync();
            }
        }

        /* Recording-specific: count pulses for auto-stop */
        if (sampler_state == SAMPLER_RECORDING) {
            sampler_clock_received = 1;
            sampler_clock_count++;
            sampler_bars_completed = sampler_clock_count / 96;

            if (sampler_target_pulses > 0 && sampler_clock_count >= sampler_target_pulses) {
                s_host.log("Sampler: target duration reached via MIDI clock");
                sampler_request_stop();
            }
        }
    } else if (status == 0xFA) {
        /* MIDI Start — transport is now playing, from the top. */
        sampler_transport_playing = 1;
        /* Start means bar 1 beat 1, so the free-running count restarts with
         * it. Continue (0xFB) deliberately does NOT reset — it resumes, and a
         * reset there would put the grid a beat out for the rest of the take. */
        shadow_transport_pulses = 0;
        s_host.overlay_sync();
        s_host.log("Sampler: transport_playing=1 (MIDI Start)");
        if (sampler_state == SAMPLER_ARMED) {
            s_host.log("Sampler: triggered by MIDI Start");
            if (sampler_preroll_enabled && sampler_duration_options[sampler_duration_index] > 0) {
                sampler_request_start(1);
            } else {
                sampler_request_start(0);
            }
        }
    }
    else if (status == 0xFC) {
        /* MIDI Stop — transport stopped */
        sampler_transport_playing = 0;
        s_host.overlay_sync();
        s_host.log("Sampler: transport_playing=0 (MIDI Stop)");
        if (sampler_state == SAMPLER_RECORDING) {
            if (sampler_external_stop_only) {
                s_host.log("Sampler: MIDI Stop ignored (external_stop_only)");
            } else {
                s_host.log("Sampler: stopped by MIDI Stop");
                sampler_request_stop();
            }
        } else if (sampler_state == SAMPLER_PREROLL) {
            /* sampler_request_stop handles preroll cancel: state→ARMED here,
             * file teardown on the shim worker. */
            s_host.log("Sampler: preroll cancelled by MIDI Stop");
            sampler_request_stop();
        }
    }
}

/* ============================================================================
 * Skipback
 * ============================================================================ */

static void skipback_stems_reconcile(void);

void skipback_init(int seconds) {
    if (skipback_buffer) return;
    int sec = skipback_clamp_seconds(seconds);
    size_t samples = (size_t)SAMPLER_SAMPLE_RATE * (size_t)sec * (size_t)SAMPLER_NUM_CHANNELS;
    skipback_buffer = (int16_t *)calloc(samples, sizeof(int16_t));
    if (skipback_buffer) {
        skipback_write_pos = 0;
        skipback_buffer_full = 0;
        skipback_seconds_actual = sec;
        skipback_total_samples = samples;
        char msg[96];
        snprintf(msg, sizeof(msg), "Skipback: allocated %ds rolling buffer (%.1f MB)",
                 sec, (double)(samples * sizeof(int16_t)) / (1024.0 * 1024.0));
        s_host.log(msg);
        /* Runs at init, before any capture — no save can be in flight, so the
         * skipback_saving gate skipback_resize needs is not needed here. */
        skipback_stems_reconcile();
    } else {
        s_host.log("Skipback: failed to allocate buffer");
    }
}

int skipback_get_seconds(void) {
    return skipback_seconds_actual;
}

int skipback_stems_get_seconds(void) {
    return skipback_stem_seconds_actual;
}

static void skipback_stems_free(void) {
    for (int i = 0; i < SAMPLER_STEM_COUNT; i++) {
        free(skipback_stem_buffer[i]);
        skipback_stem_buffer[i] = NULL;
    }
    skipback_stem_total_samples = 0;
    skipback_stem_seconds_actual = 0;
    skipback_stem_write_pos = 0;
    skipback_stem_buffer_full = 0;
}

/* Bring the stem rolling buffers into line with the setting and the master's
 * length. Worker thread only — this callocs tens of megabytes.
 *
 * Caller must hold the audio thread off (skipback_saving), exactly as
 * skipback_resize does for the master buffer. */
static void skipback_stems_reconcile(void) {
    int want = SAVE_STEMS_WANTS_STEMS(sampler_stem_mode) && skipback_seconds_actual > 0;
    if (!want) {
        if (skipback_stem_buffer[0]) {
            skipback_stems_free();
            s_host.log("Skipback: stem buffers released");
        }
        return;
    }
    int sec = skipback_seconds_actual;
    if (sec > SKIPBACK_STEM_MAX_SECONDS) sec = SKIPBACK_STEM_MAX_SECONDS;
    if (skipback_stem_buffer[0] && sec == skipback_stem_seconds_actual) return;

    skipback_stems_free();
    size_t samples = (size_t)SAMPLER_SAMPLE_RATE * (size_t)sec * (size_t)SAMPLER_NUM_CHANNELS;
    for (int i = 0; i < SAMPLER_STEM_COUNT; i++) {
        skipback_stem_buffer[i] = (int16_t *)calloc(samples, sizeof(int16_t));
        if (!skipback_stem_buffer[i]) {
            /* All-or-nothing: four of five buffers means one stem is silently
             * absent from the save with nothing to say which. */
            skipback_stems_free();
            s_host.log("Skipback: stem buffer allocation failed — stems disabled");
            return;
        }
    }
    skipback_stem_total_samples = samples;
    skipback_stem_write_pos = 0;
    skipback_stem_buffer_full = 0;
    skipback_stem_seconds_actual = sec;
    char msg[128];
    snprintf(msg, sizeof(msg), "Skipback: allocated %d stem buffers, %ds each (%.1f MB total)",
             SAMPLER_STEM_COUNT, sec,
             (double)(samples * sizeof(int16_t) * SAMPLER_STEM_COUNT) / (1024.0 * 1024.0));
    s_host.log(msg);
}

void skipback_capture_stems(const int16_t *const *stems, int count) {
    if (!skipback_stem_buffer[0] || !stems) return;
    if (__atomic_load_n(&skipback_saving, __ATOMIC_ACQUIRE)) return;

    size_t total_samples = skipback_stem_total_samples;
    if (total_samples == 0) return;
    size_t block_samples = SAMPLER_FRAMES_PER_BLOCK * SAMPLER_NUM_CHANNELS;
    size_t start = skipback_stem_write_pos;
    size_t wp = start;

    if (count > SAMPLER_STEM_COUNT) count = SAMPLER_STEM_COUNT;
    for (int i = 0; i < SAMPLER_STEM_COUNT; i++) {
        const int16_t *src = (i < count) ? stems[i] : NULL;
        wp = start;
        for (size_t j = 0; j < block_samples; j++) {
            /* Silence for an absent stem, not a skip: one shared write_pos
             * keeps all five sample-aligned, and a short one would slide. */
            skipback_stem_buffer[i][wp] = src ? src[j] : 0;
            wp = (wp + 1) % total_samples;
        }
    }

    if (!skipback_stem_buffer_full && wp < start)
        skipback_stem_buffer_full = 1;
    skipback_stem_write_pos = wp;
}

void skipback_capture(int16_t *audio) {
    if (!skipback_buffer || !audio || __atomic_load_n(&skipback_saving, __ATOMIC_ACQUIRE)) return;

    size_t total_samples = skipback_total_samples;
    if (total_samples == 0) return;
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

void skipback_amend(const int16_t *audio) {
    if (!skipback_buffer || !audio || __atomic_load_n(&skipback_saving, __ATOMIC_ACQUIRE)) return;

    size_t total_samples = skipback_total_samples;
    if (total_samples == 0) return;
    size_t block_samples = SAMPLER_FRAMES_PER_BLOCK * SAMPLER_NUM_CHANNELS;
    /* Mix into the block that was just written by skipback_capture */
    size_t start = (skipback_write_pos + total_samples - block_samples) % total_samples;
    for (size_t i = 0; i < block_samples; i++) {
        size_t pos = (start + i) % total_samples;
        int32_t sum = (int32_t)skipback_buffer[pos] + (int32_t)audio[i];
        if (sum > 32767) sum = 32767;
        if (sum < -32768) sum = -32768;
        skipback_buffer[pos] = (int16_t)sum;
    }
}

void skipback_resize(int new_seconds) {
    int sec = skipback_clamp_seconds(new_seconds);

    /* Serialize concurrent resize requests. */
    pthread_mutex_lock(&skipback_resize_mutex);

    if (!skipback_buffer) {
        pthread_mutex_unlock(&skipback_resize_mutex);
        return;
    }

    /* If a save is in progress, defer — caller can retry later. */
    int expected = 0;
    if (!__atomic_compare_exchange_n(&skipback_saving, &expected, 1,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&skipback_resize_mutex);
        s_host.log("Skipback: resize deferred (save in progress)");
        return;
    }
    /* skipback_saving==1 now gates skipback_capture/amend on the audio thread. */

    /* This event is ALSO how a Save Stems change reaches the allocator, so the
     * length being unchanged is not a reason to leave without reconciling the
     * stem buffers — that early return was the whole bug when the setting was
     * flipped without touching Skipback Len. The stem reconcile has to run
     * inside the saving gate, which is why it is here and not above it. */
    if (sec == skipback_seconds_actual) {
        skipback_stems_reconcile();
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&skipback_resize_mutex);
        return;
    }

    size_t old_total = skipback_total_samples;
    int16_t *old_buf = skipback_buffer;
    size_t old_wp = skipback_write_pos;
    int old_full = skipback_buffer_full;

    size_t new_total = (size_t)SAMPLER_SAMPLE_RATE * (size_t)sec * (size_t)SAMPLER_NUM_CHANNELS;
    int16_t *new_buf = (int16_t *)calloc(new_total, sizeof(int16_t));
    if (!new_buf) {
        s_host.log("Skipback: resize allocation failed");
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&skipback_resize_mutex);
        return;
    }

    /* Determine how many samples of valid audio currently exist (linear). */
    size_t valid;
    size_t start_pos;
    if (old_full) {
        valid = old_total;
        start_pos = old_wp;  /* oldest sample */
    } else {
        valid = old_wp;
        start_pos = 0;
    }

    /* Decide what to keep when shrinking: the most recent N samples. */
    size_t keep = (valid < new_total) ? valid : new_total;
    if (keep < valid) {
        size_t skip = valid - keep;
        start_pos = (start_pos + skip) % old_total;
    }

    /* Copy keep samples from old ring (starting at start_pos, wrapping)
     * into new buffer linearly starting at index 0. */
    size_t remaining = keep;
    size_t src = start_pos;
    size_t dst = 0;
    while (remaining > 0) {
        size_t chunk = remaining;
        if (src + chunk > old_total) chunk = old_total - src;
        memcpy(new_buf + dst, old_buf + src, chunk * sizeof(int16_t));
        dst += chunk;
        src = (src + chunk) % old_total;
        remaining -= chunk;
    }

    /* Publish new buffer state. Pointer write is atomic on aligned aarch64,
     * but the audio thread is gated by skipback_saving anyway. */
    skipback_buffer = new_buf;
    skipback_total_samples = new_total;
    skipback_write_pos = (keep < new_total) ? keep : 0;
    skipback_buffer_full = (keep == new_total);
    skipback_seconds_actual = sec;
    __sync_synchronize();

    free(old_buf);

    /* Stems follow the master's length (clamped by SKIPBACK_STEM_MAX_SECONDS).
     * Their contents are DISCARDED on a resize rather than carried across:
     * unlike the master they are five rings sharing one write position, and
     * the honest cheap option is to start them again together. */
    skipback_stems_reconcile();

    char msg[128];
    snprintf(msg, sizeof(msg),
             "Skipback: resized to %ds (kept %.1fs, %.1f MB)",
             sec,
             (double)keep / ((double)SAMPLER_NUM_CHANNELS * (double)SAMPLER_SAMPLE_RATE),
             (double)(new_total * sizeof(int16_t)) / (1024.0 * 1024.0));
    s_host.log(msg);

    __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&skipback_resize_mutex);
}

/* Write one rolling buffer out as a WAV. Returns 1 if a file was created.
 *
 * `write_silent` distinguishes the master (always written, even if the take is
 * silent — the user asked for the last 30 seconds and silence is an answer)
 * from a stem (skipped when silent, so an unloaded slot leaves no file). The
 * silence scan is why a stem can be skipped without a per-block flag: unlike
 * the sampler's rings these buffers are complete and sitting still by the time
 * anything reads them.
 *
 * Runs on the detached skipback writer thread, never on RT. */
static int skipback_write_wav(const char *path, const int16_t *buf,
                              size_t total_samples, size_t wp, int full,
                              int write_silent) {
    if (!buf || total_samples == 0) return 0;

    size_t data_samples = full ? total_samples : wp;
    size_t start_pos    = full ? wp : 0;
    if (data_samples == 0) return 0;

    if (!write_silent) {
        int silent = 1;
        for (size_t i = 0; i < total_samples; i++) {
            if (buf[i]) { silent = 0; break; }
        }
        if (silent) return 0;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        char msg[380];
        snprintf(msg, sizeof(msg), "Skipback: failed to open WAV file: %s", path);
        s_host.log(msg);
        return 0;
    }

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

    size_t pos = start_pos;
    size_t remaining = data_samples;
    while (remaining > 0) {
        size_t chunk = remaining;
        if (pos + chunk > total_samples)
            chunk = total_samples - pos;
        /* Paced: this loop is the 32 MB burst that stalled the DAC once stems
         * made it six files instead of one. */
        sampler_write_paced(f, buf + pos, chunk * sizeof(int16_t));
        pos = (pos + chunk) % total_samples;
        remaining -= chunk;
    }

    fclose(f);
    chown_to_ableton(path);

    uint32_t frames = (uint32_t)(data_samples / SAMPLER_NUM_CHANNELS);
    char msg[380];
    snprintf(msg, sizeof(msg), "Skipback: wrote %s (%.1f sec)",
             path, (float)frames / SAMPLER_SAMPLE_RATE);
    s_host.log(msg);
    return 1;
}

static void *skipback_writer_func(void *arg) {
    (void)arg;
    sampler_io_thread_be_polite();

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
            chown_to_ableton_recursive(skipback_dir);
        }
    }

    /* Generate filename */
    char path[256];
    snprintf(path, sizeof(path), "%s/skipback_%04d%02d%02d_%02d%02d%02d.wav",
             skipback_dir,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    int keep_master = SAVE_STEMS_WANTS_MASTER(sampler_stem_mode);
    int want_stems = SAVE_STEMS_WANTS_STEMS(sampler_stem_mode) &&
                     skipback_stem_buffer[0] && skipback_stem_total_samples > 0;

    /* Nothing anywhere: the rolling buffer has not wrapped and nothing has
     * been written. Checked before any file is created so a failed save does
     * not leave a zero-length WAV behind. */
    if ((skipback_buffer_full ? skipback_total_samples : skipback_write_pos) == 0) {
        s_host.log("Skipback: no audio captured yet");
        s_host.announce("No audio captured yet");
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return NULL;
    }

    int master_ok = 0;
    if (keep_master)
        master_ok = skipback_write_wav(path, skipback_buffer, skipback_total_samples,
                                       skipback_write_pos, skipback_buffer_full, 1);

    /* Stems, named beside the master exactly as the sampler's are. A stem
     * whose buffer is entirely silent is not written at all — same rule as the
     * sampler, so an unloaded slot leaves no file either way. */
    int stems_kept = 0;
    if (want_stems) {
        for (int i = 0; i < SAMPLER_STEM_COUNT; i++) {
            char spath[300];
            sampler_stem_path_build(spath, sizeof(spath), path, sampler_stem_names[i]);
            stems_kept += skipback_write_wav(spath, skipback_stem_buffer[i],
                                             skipback_stem_total_samples,
                                             skipback_stem_write_pos,
                                             skipback_stem_buffer_full, 0);
        }
    }

    if (!master_ok && stems_kept == 0) {
        s_host.log("Skipback: nothing written (no audio in master or stems)");
        s_host.announce("Skipback failed");
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return NULL;
    }

    char msg[380];
    if (master_ok && stems_kept)
        snprintf(msg, sizeof(msg), "Skipback: saved %s + %d stem(s)", path, stems_kept);
    else if (stems_kept)
        snprintf(msg, sizeof(msg), "Skipback: saved %d stem(s) for %s", stems_kept, path);
    else
        snprintf(msg, sizeof(msg), "Skipback: saved %s", path);
    s_host.log(msg);

    skipback_overlay_timeout = SKIPBACK_OVERLAY_FRAMES;
    s_host.overlay_sync();
    if (stems_kept == 1)      s_host.announce("Skipback saved, 1 stem");
    else if (stems_kept > 1)  { char a[48];
                                snprintf(a, sizeof(a), "Skipback saved, %d stems", stems_kept);
                                s_host.announce(a); }
    else                      s_host.announce("Skipback saved");
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

    /* Thread creation is deferred to the shim worker — this runs on the
     * RT gesture path. skipback_saving is already set, so capture pauses
     * immediately and the snapshot is consistent when the writer starts. */
    shim_worker_post(SHIM_EVT_SKIPBACK_SAVE);
    char msg[64];
    snprintf(msg, sizeof(msg), "Skipback: saving last %d seconds...", skipback_seconds_actual);
    s_host.log(msg);
}

/* Worker half: spawn the detached skipback writer (the save itself takes
 * seconds and must not block the worker's event loop). */
void skipback_worker_spawn_save(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, skipback_writer_func, NULL) != 0) {
        s_host.log("Skipback: failed to create writer thread");
        s_host.announce("Skipback failed");
        __atomic_store_n(&skipback_saving, 0, __ATOMIC_RELEASE);
        return;
    }
    pthread_detach(t);
}

/* ============================================================================
 * VU Meter
 * ============================================================================ */

void sampler_update_vu(void) {
    if (!sampler_fullscreen_active && sampler_state != SAMPLER_RECORDING) return;

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
