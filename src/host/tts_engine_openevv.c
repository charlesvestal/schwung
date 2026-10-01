/*
 * TTS Engine - openevv (Eloquence) backend
 *
 * openevv (https://github.com/Mudb0y/openevv) is a portable C rebuild of
 * IBM's Embedded ViaVoice / Eloquence behind IBM's own ECI interface. Its
 * engine code is MIT; the language data compiled into libeci.so.1 is IBM's
 * and is NOT licensed -- see THIRD_PARTY_LICENSES.md.
 *
 * All public functions are prefixed openevv_tts_ like the other two backends;
 * the dispatcher in tts_engine_dispatch.c routes to the active one.
 *
 * Three things make this backend shaped differently from eSpeak and Flite:
 *
 *  - It is DLOPENED, never linked. libeci.so.1 ships in lib/ beside the other
 *    TTS libraries, but a device without it (or a build that skipped it) must
 *    still boot a shim, so a missing library is an init failure the
 *    dispatcher answers by falling back to eSpeak -- not an unresolved symbol
 *    that takes MoveOriginal down.
 *
 *  - NO ECI CALL EVER RUNS ON THE SPI CALLBACK. eciNew maps a 256 MB arena and
 *    starts a thread, the arena allocator takes a mutex, and a cancel waits
 *    for the engine to finish the message it is on (~27 ms). One worker,
 *    created SCHED_OTHER on cores 0-2, owns the ECIHand and makes every call;
 *    the engine's own synthesis thread is created from it and so inherits
 *    that schedule rather than MoveOriginal's FIFO 70. What the RT side does
 *    is publish (text, settings) and read the ring -- no locks a non-RT
 *    thread can hold, no file I/O, no logging.
 *
 *  - It cannot abandon an utterance. Interruption is a GENERATION: speak()
 *    bumps req_gen before publishing text, the callback answers eciDataAbort
 *    for any stale generation, the worker waits for the engine to finish the
 *    message it is on (never eciStop -- a cross-thread stop crashed the
 *    device; see worker_speak), and the reader
 *    holds silence until the worker publishes where the new utterance's
 *    audio starts in the ring, then jumps there.
 */

#define _GNU_SOURCE        /* pthread affinity + CPU_SET (RT: keep core 3 free) */

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "unified_log.h"
#include "tts_config.h"
#include "tts_openevv_map.h"
#include "tts_upsample4.h"

#define OPENEVV_LIB "libeci.so.1"
/* The engine is asked for its NATIVE rate and raised to 44100 by
 * tts_upsample4.h in the callback. Asking the engine for 44100 instead runs
 * its own double-precision sinc on the synthesis thread, which measured at
 * eleven times the cost of the speech: 262 ms against 22 for one sentence,
 * all of it in front of the first sample and inside every interruption. */
#define OPENEVV_SAMPLE_RATE_11025 1        /* eciSampleRate code for 11025 Hz */
/* Samples per callback buffer, at 11025: 46 ms of audio, the same slice the
 * old 2048-at-44100 buffer was. The first callback waits for a full one, so
 * this is part of the time to first sound. */
#define OPENEVV_FRAME 512
#define OPENEVV_TEXT_MAX 8192              /* SHADOW_SCREENREADER_TEXT_LEN */

/* ---- the slice of ECI this backend uses --------------------------------
 *
 * Declared here rather than taken from libs/openevv/include/eci.h so the
 * shim compiles whether or not that submodule is checked out: the library is
 * dlopened, and these numbers are IBM's published ABI, which openevv
 * transcribes and cannot change. tests/host/test_tts_openevv_contract.sh
 * holds every one of them to eci.h. */
typedef void *ECIHand;
#define NULL_ECI_HAND ((ECIHand)0)
typedef enum { eciWaveformBuffer = 0 } ECIMessage;
enum { eciDataNotProcessed = 0, eciDataProcessed = 1, eciDataAbort = 2 };
typedef int (*ECICallback)(ECIHand handle, ECIMessage message, int param, void *data);
enum { eciSampleRate = 5 };
enum {
    eciGender = 0, eciHeadSize = 1, eciPitchBaseline = 2, eciPitchFluctuation = 3,
    eciRoughness = 4, eciBreathiness = 5, eciSpeed = 6, eciVolume = 7
};

/* ---- the library, resolved by name ------------------------------------ */

typedef ECIHand (*eci_new_fn)(void);
typedef ECIHand (*eci_delete_fn)(ECIHand);
typedef int (*eci_set_param_fn)(ECIHand, int, int);
typedef int (*eci_set_output_buffer_fn)(ECIHand, int, short *);
typedef void (*eci_register_callback_fn)(ECIHand, ECICallback, void *);
typedef int (*eci_add_text_fn)(ECIHand, const void *);
typedef int (*eci_synthesize_fn)(ECIHand);
typedef int (*eci_synchronize_fn)(ECIHand);
typedef int (*eci_copy_voice_fn)(ECIHand, int, int);
typedef int (*eci_set_voice_param_fn)(ECIHand, int, int, int);

static struct {
    void *dl;
    eci_new_fn New;
    eci_delete_fn Delete;
    eci_set_param_fn SetParam;
    eci_set_output_buffer_fn SetOutputBuffer;
    eci_register_callback_fn RegisterCallback;
    eci_add_text_fn AddText;
    eci_synthesize_fn Synthesize;
    eci_synchronize_fn Synchronize;
    eci_copy_voice_fn CopyVoice;
    eci_set_voice_param_fn SetVoiceParam;
} eci;

static bool openevv_load_library(void) {
    if (eci.dl) return true;
    void *dl = dlopen(OPENEVV_LIB, RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        unified_log("tts_openevv", LOG_LEVEL_ERROR, "dlopen %s failed: %s",
                   OPENEVV_LIB, dlerror());
        return false;
    }
#define RESOLVE(field, name) \
    do { \
        *(void **)(&eci.field) = dlsym(dl, name); \
        if (!eci.field) { \
            unified_log("tts_openevv", LOG_LEVEL_ERROR, "%s: missing symbol %s", OPENEVV_LIB, name); \
            dlclose(dl); \
            memset(&eci, 0, sizeof(eci)); \
            return false; \
        } \
    } while (0)
    RESOLVE(New, "eciNew");
    RESOLVE(Delete, "eciDelete");
    RESOLVE(SetParam, "eciSetParam");
    RESOLVE(SetOutputBuffer, "eciSetOutputBuffer");
    RESOLVE(RegisterCallback, "eciRegisterCallback");
    RESOLVE(AddText, "eciAddText");
    RESOLVE(Synthesize, "eciSynthesize");
    RESOLVE(Synchronize, "eciSynchronize");
    RESOLVE(CopyVoice, "eciCopyVoice");
    RESOLVE(SetVoiceParam, "eciSetVoiceParam");
#undef RESOLVE
    eci.dl = dl;
    return true;
}

/* ---- the audio ring: ECI's thread writes, the RT reader reads ---------- */

/* Mono at 44100 -- the reader duplicates to L=R. 4 s, and the callback's
 * backpressure (it waits for room, see openevv_callback) keeps it from ever
 * needing more. */
#define RING_SIZE (44100 * 4)
static int16_t ring[RING_SIZE];
static _Atomic int ring_write_pos = 0;     /* ECI callback */
static _Atomic int ring_read_pos = 0;      /* RT reader */

static inline int ring_used(int w, int r) {
    return (w >= r) ? (w - r) : (RING_SIZE - r + w);
}

/* ---- generations ------------------------------------------------------- */

static _Atomic uint32_t req_gen = 0;       /* bumped by every speak() */
static _Atomic uint32_t audio_gen = 0;     /* the utterance audio_start belongs to */
static _Atomic int audio_start = 0;        /* ring position that utterance begins at */
static _Atomic uint32_t utt_gen = 0;       /* what the callback is producing for */
static uint32_t reader_gen = 0;            /* RT reader only */

/* ---- text hand-off: a triple buffer, lock-free for the consumer ---------
 *
 * The producers are MoveOriginal's threads (the debounced screen reader, the
 * Shift+Menu toggle, the PIN scanner), serialised by a spin flag no non-RT
 * thread ever takes. The worker swaps the middle slot out without a lock. */
typedef struct {
    uint32_t gen;
    char text[OPENEVV_TEXT_MAX];
} text_slot_t;

#define SLOT_DIRTY 4
static text_slot_t slots[3];
static int slot_back = 0;                  /* producers (under the spin flag) */
static int slot_front = 1;                 /* worker */
static _Atomic int slot_middle = 2;
static atomic_flag producer_lock = ATOMIC_FLAG_INIT;

/* ---- settings -----------------------------------------------------------
 *
 * Written from the SPI path, read by the worker. The voice is seven bytes, so
 * it rides a seqlock rather than a lock: a single writer, and a reader that
 * retries across a write. */
static volatile bool tts_enabled = false;
static volatile bool tts_disabling = false;
static volatile bool tts_disabling_had_audio = false;
static volatile int tts_volume = 70;
static volatile float tts_speed = 1.0f;
static volatile float tts_pitch = 110.0f;  /* kept for tts.json; ECI uses evv.pitch */

static tts_evv_voice_t evv_voice = TTS_EVV_DEFAULT_VOICE;
static _Atomic uint32_t evv_seq = 0;       /* odd while a write is in progress */
static _Atomic uint32_t config_seq = 0;    /* bumped by any change worth saving */
static bool config_loaded = false;

static void evv_voice_read(tts_evv_voice_t *out) {
    for (;;) {
        uint32_t a = atomic_load_explicit(&evv_seq, memory_order_acquire);
        if (a & 1) continue;
        *out = evv_voice;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&evv_seq, memory_order_relaxed) == a) return;
    }
}

static void evv_voice_write(const tts_evv_voice_t *v) {
    atomic_fetch_add_explicit(&evv_seq, 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    evv_voice = *v;
    atomic_fetch_add_explicit(&evv_seq, 1, memory_order_release);
}

/* ---- worker ------------------------------------------------------------ */

static pthread_t worker_thread;
static bool worker_started = false;
static sem_t worker_wake;
static _Atomic bool want_active = false;   /* false: the worker drops its handle */
static bool initialized = false;

static short eci_frame[OPENEVV_FRAME];

/* 11025 -> 44100. The filter is designed once by the worker before the first
 * instance exists; the state belongs to whoever is producing -- the engine's
 * thread inside the callback, or the worker after eciSynchronize has returned
 * (so never both at once) -- and is reset when the utterance changes. */
static tts_up4_filter_t up_filter;
static tts_up4_state_t up_state;
static uint32_t up_gen = 0;                /* the utterance up_state holds */
static int16_t up_out[4 * OPENEVV_FRAME];
static bool up_designed = false;

static void worker_poke(void) {
    if (worker_started) sem_post(&worker_wake);   /* never blocks */
}

static bool ring_room(int n) {
    int w = atomic_load_explicit(&ring_write_pos, memory_order_relaxed);
    int r = atomic_load_explicit(&ring_read_pos, memory_order_acquire);
    return RING_SIZE - 1 - ring_used(w, r) >= n;
}

/* One producer at a time: see up_state. */
static void ring_push(const int16_t *s, int n) {
    int w = atomic_load_explicit(&ring_write_pos, memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        ring[w] = s[i];
        w = (w + 1) % RING_SIZE;
    }
    atomic_store_explicit(&ring_write_pos, w, memory_order_release);
}

static int openevv_callback(ECIHand h, ECIMessage msg, int param, void *data) {
    (void)h; (void)data;
    if (msg != eciWaveformBuffer || param <= 0) return eciDataProcessed;

    uint32_t mine = atomic_load_explicit(&utt_gen, memory_order_relaxed);
    int n = param > OPENEVV_FRAME ? OPENEVV_FRAME : param;

    /* A full ring is WAITED OUT here, on the engine's own thread, never
     * answered with eciDataNotProcessed: that answer costs a flat 30 ms
     * sleep inside the engine before the buffer is offered again
     * (docs/api.md), and an interruption landing in that sleep waited for
     * all of it. The ring holds 4 s and the engine runs far ahead of
     * realtime, so any line longer than that parked the engine there. A
     * stale generation is noticed within a millisecond instead. */
    for (;;) {
        if (mine != atomic_load_explicit(&req_gen, memory_order_acquire) ||
            !atomic_load_explicit(&want_active, memory_order_relaxed))
            return eciDataAbort;
        if (ring_room(4 * n)) break;
        const struct timespec ms = { 0, 1000 * 1000 };
        nanosleep(&ms, NULL);
    }

    if (up_gen != mine) {
        tts_up4_reset(&up_state);          /* no tail of the last utterance */
        up_gen = mine;
    }
    tts_up4_run(&up_filter, &up_state, eci_frame, n, up_out);
    ring_push(up_out, 4 * n);
    return eciDataProcessed;
}

static ECIHand worker_open(void) {
    if (!up_designed) {
        tts_up4_design(&up_filter);        /* transcendentals: here, never on audio */
        up_designed = true;
    }
    ECIHand h = eci.New();
    if (h == NULL_ECI_HAND) {
        unified_log("tts_openevv", LOG_LEVEL_ERROR, "eciNew failed");
        return NULL_ECI_HAND;
    }
    /* Refused would leave the engine at a rate the upsampler was not built
     * for, which is speech at the wrong speed rather than an error -- so it
     * is a failure, not a warning. 11025 is the engine's default anyway. */
    if (eci.SetParam(h, eciSampleRate, OPENEVV_SAMPLE_RATE_11025) < 0) {
        unified_log("tts_openevv", LOG_LEVEL_ERROR, "eciSampleRate 11025 refused");
        eci.Delete(h);
        return NULL_ECI_HAND;
    }
    eci.RegisterCallback(h, openevv_callback, NULL);
    if (!eci.SetOutputBuffer(h, OPENEVV_FRAME, eci_frame)) {
        unified_log("tts_openevv", LOG_LEVEL_ERROR, "eciSetOutputBuffer refused");
        eci.Delete(h);
        return NULL_ECI_HAND;
    }
    unified_log("tts_openevv", LOG_LEVEL_INFO, "openevv instance ready (11025 Hz, upsampled to 44100)");
    return h;
}

static int applied_preset = -1;

static void worker_apply_voice(ECIHand h) {
    tts_evv_voice_t v;
    evv_voice_read(&v);
    tts_evv_voice_clamp(&v);
    if (applied_preset != v.voice) {
        eci.CopyVoice(h, v.voice, 0);
        applied_preset = v.voice;
    }
    eci.SetVoiceParam(h, 0, eciGender, v.gender);
    eci.SetVoiceParam(h, 0, eciHeadSize, v.head);
    eci.SetVoiceParam(h, 0, eciPitchBaseline, v.pitch);
    eci.SetVoiceParam(h, 0, eciPitchFluctuation, v.inflection);
    eci.SetVoiceParam(h, 0, eciRoughness, v.rough);
    eci.SetVoiceParam(h, 0, eciBreathiness, v.breath);
    eci.SetVoiceParam(h, 0, eciSpeed, tts_evv_speed_from_mult(tts_speed));
    /* Full scale; the shared Volume row is applied at read time, as it is
     * for eSpeak and Flite, so all three answer the row identically. */
    eci.SetVoiceParam(h, 0, eciVolume, 100);
}

static uint32_t saved_config_seq = 0;

static void worker_save_config(void) {
    uint32_t seq = atomic_load_explicit(&config_seq, memory_order_acquire);
    if (seq == saved_config_seq) return;
    tts_config_t cfg;
    tts_config_load(&cfg);                 /* keeps the engine choice as it is */
    cfg.speed = tts_speed;
    cfg.pitch = tts_pitch;
    cfg.volume = tts_volume;
    evv_voice_read(&cfg.evv);
    if (tts_config_save(&cfg)) {
        saved_config_seq = seq;
    } else {
        unified_log("tts_openevv", LOG_LEVEL_ERROR, "Failed to save TTS config");
        saved_config_seq = seq;            /* do not retry every wake */
    }
}

/* Take the newest published text, if any. */
static text_slot_t *worker_take_text(void) {
    if (!(atomic_load_explicit(&slot_middle, memory_order_acquire) & SLOT_DIRTY))
        return NULL;
    slot_front = atomic_exchange_explicit(&slot_middle, slot_front, memory_order_acq_rel) & 3;
    return &slots[slot_front];
}

static void worker_speak(ECIHand h, const text_slot_t *slot) {
    static char cp1252[OPENEVV_TEXT_MAX];
    uint32_t gen = slot->gen;
    if (gen != atomic_load_explicit(&req_gen, memory_order_acquire)) return;  /* already stale */

    if (tts_evv_utf8_to_cp1252(slot->text, cp1252, sizeof(cp1252)) == 0) return;

    worker_apply_voice(h);

    /* Where this utterance begins, THEN whose it is: the reader jumps to
     * audio_start only once it sees audio_gen match what it last asked for. */
    atomic_store_explicit(&utt_gen, gen, memory_order_relaxed);
    atomic_store_explicit(&audio_start,
                          atomic_load_explicit(&ring_write_pos, memory_order_acquire),
                          memory_order_relaxed);
    atomic_store_explicit(&audio_gen, gen, memory_order_release);

    if (!eci.AddText(h, cp1252) || !eci.Synthesize(h)) {
        unified_log("tts_openevv", LOG_LEVEL_ERROR, "openevv refused the text");
        return;
    }

    /* NEVER eciStop here. It unwinds the engine from THIS thread while the
     * rules are mid-walk on the engine's own, which openevv documents as not
     * correct (docs/status.md, "a landing place jumped to from a thread that
     * never planted it") -- on the device it was a SIGSEGV with a garbage pc
     * on the engine's 128 KB thread stack, taking MoveOriginal with it, when
     * the jog outran the speech. The callback already answers eciDataAbort
     * for a stale generation, ON the engine's thread, and all three ways of
     * cancelling cost the same because each waits for the current message to
     * finish (docs/api.md, "Waiting and stopping"). So just wait.
     *
     * And wait with eciSynchronize, never a sleep-and-poll on eciSpeaking:
     * the engine hands over about ONE buffer per eciSpeaking call, so a 5 ms
     * poll paced delivery at a buffer per 5 ms -- measured, the same sentence
     * took 1055 ms polled at 5 ms, 435 at 1 ms and 262 under eciSynchronize,
     * and the poll sat in front of the first sample too. A stale utterance
     * still ends promptly, because the callback is answering eciDataAbort. */
    eci.Synchronize(h);

    /* The filter holds the last few ms of the utterance (its delay); push
     * silence through so the end of the word is heard rather than cut. Safe
     * here: once eciSynchronize returns no callback is running. */
    if (up_gen == gen && gen == atomic_load_explicit(&req_gen, memory_order_acquire) &&
        ring_room(4 * TTS_UP4_PHASE_TAPS)) {
        static const int16_t zeros[TTS_UP4_PHASE_TAPS];
        tts_up4_run(&up_filter, &up_state, zeros, TTS_UP4_PHASE_TAPS, up_out);
        ring_push(up_out, 4 * TTS_UP4_PHASE_TAPS);
    }
}

static void *openevv_worker(void *arg) {
    (void)arg;
    ECIHand h = NULL_ECI_HAND;

    for (;;) {
        bool active = atomic_load_explicit(&want_active, memory_order_acquire);

        if (active && h == NULL_ECI_HAND) {
            h = worker_open();
            applied_preset = -1;
        } else if (!active && h != NULL_ECI_HAND) {
            /* No eciStop -- see worker_speak. want_active is already false,
             * so the callback is answering eciDataAbort; wait it out. */
            eci.Synchronize(h);
            eci.Delete(h);
            h = NULL_ECI_HAND;
            unified_log("tts_openevv", LOG_LEVEL_INFO, "openevv instance released");
        }

        worker_save_config();

        text_slot_t *slot = worker_take_text();
        if (slot && h != NULL_ECI_HAND && active) {
            worker_speak(h, slot);
            continue;                      /* something newer may be waiting */
        }

        while (sem_wait(&worker_wake) != 0) { /* EINTR */ }
    }
    return NULL;
}

static bool openevv_start_worker(void) {
    if (worker_started) return true;
    if (sem_init(&worker_wake, 0, 0) != 0) return false;

    /* EXPLICIT non-RT schedule: the caller may be one of MoveOriginal's
     * FIFO-70 threads, and the default PTHREAD_INHERIT_SCHED would hand the
     * worker -- and through it the engine's own thread -- that priority and
     * possibly core 3. Same as schwung_trace.c's exporter. */
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    struct sched_param sp = { .sched_priority = 0 };
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus); CPU_SET(1, &cpus); CPU_SET(2, &cpus);
    pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);  /* best-effort */
    int rc = pthread_create(&worker_thread, &attr, openevv_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        sem_destroy(&worker_wake);
        return false;
    }
    worker_started = true;
    return true;
}

/* ---- persisted state ---------------------------------------------------- */

#define STATE_PATH "/data/UserData/schwung/config/screen_reader_state.txt"

static void openevv_load_state(void) {
    FILE *f = fopen(STATE_PATH, "r");
    if (!f) return;
    char buf[8];
    if (fgets(buf, sizeof(buf), f)) tts_enabled = (buf[0] == '1');
    fclose(f);
}

static void openevv_save_state_value(int on) {
    FILE *f = fopen(STATE_PATH, "w");
    if (!f) {
        unified_log("tts_openevv", LOG_LEVEL_ERROR, "Failed to save screen reader state");
        return;
    }
    fprintf(f, "%d\n", on ? 1 : 0);
    fclose(f);
}

static void openevv_load_config_once(void) {
    if (config_loaded) return;
    tts_config_t cfg;
    tts_config_load(&cfg);
    tts_speed = cfg.speed;
    tts_pitch = cfg.pitch;
    tts_volume = cfg.volume;
    tts_evv_voice_clamp(&cfg.evv);
    evv_voice_write(&cfg.evv);
    config_loaded = true;
}

/* ---- public API --------------------------------------------------------- */

bool openevv_tts_init(int sample_rate) {
    (void)sample_rate;                     /* always 44100 out: 11025 upsampled */
    if (initialized) return true;

    if (!openevv_load_library()) return false;
    openevv_load_state();
    openevv_load_config_once();
    /* saved_config_seq is left where it is: a voice edited while another
     * engine was active (the web manager can) bumped config_seq with no
     * worker to persist it, and the first wake below saves it now. */

    atomic_store_explicit(&want_active, true, memory_order_release);
    if (!openevv_start_worker()) {
        atomic_store_explicit(&want_active, false, memory_order_release);
        unified_log("tts_openevv", LOG_LEVEL_ERROR, "Failed to start openevv worker");
        return false;
    }
    worker_poke();                         /* open the instance now, off the RT path */

    initialized = true;
    unified_log("tts_openevv", LOG_LEVEL_INFO, "TTS engine (openevv) initialized");
    return true;
}

/*
 * Never joins: this is reached from the SPI path on an engine switch. The
 * worker drops its instance and parks; the thread and the library stay, so a
 * switch back costs an eciNew and nothing else.
 */
void openevv_tts_cleanup(void) {
    if (!initialized) return;
    atomic_store_explicit(&want_active, false, memory_order_release);
    atomic_fetch_add_explicit(&req_gen, 1, memory_order_acq_rel);  /* abort in flight */
    worker_poke();
    initialized = false;
}

bool openevv_tts_speak(const char *text) {
    if (!text || !text[0]) return false;
    if (!tts_enabled || tts_disabling) return false;
    if (!initialized && !openevv_tts_init(44100)) return false;

    /* Bounded: the producers are FIFO threads, and one spinning on a core
     * whose holder it has preempted would never see the flag clear. Two
     * utterances racing inside one copy is not worth a hang to keep both. */
    int spins = 0;
    while (atomic_flag_test_and_set_explicit(&producer_lock, memory_order_acquire)) {
        if (++spins > 4096) return false;
    }
    uint32_t gen = atomic_fetch_add_explicit(&req_gen, 1, memory_order_acq_rel) + 1;
    text_slot_t *s = &slots[slot_back];
    s->gen = gen;
    strncpy(s->text, text, sizeof(s->text) - 1);
    s->text[sizeof(s->text) - 1] = '\0';
    slot_back = atomic_exchange_explicit(&slot_middle, slot_back | SLOT_DIRTY,
                                         memory_order_acq_rel) & 3;
    atomic_flag_clear_explicit(&producer_lock, memory_order_release);

    worker_poke();
    return true;
}

bool openevv_tts_is_speaking(void) {
    return atomic_load(&ring_read_pos) != atomic_load(&ring_write_pos) || tts_disabling;
}

/* The RT mix path. No locks, no file I/O, no logging. */
int openevv_tts_get_audio(int16_t *out_buffer, int max_frames) {
    if (!out_buffer || max_frames <= 0) return 0;
    if (!tts_enabled && !tts_disabling) return 0;

    uint32_t want = atomic_load_explicit(&req_gen, memory_order_acquire);
    uint32_t have = atomic_load_explicit(&audio_gen, memory_order_acquire);
    int avail = 0;
    int r = atomic_load_explicit(&ring_read_pos, memory_order_relaxed);

    if (have == want) {
        if (reader_gen != have) {
            /* A new utterance: skip whatever the old one left unread. */
            r = atomic_load_explicit(&audio_start, memory_order_relaxed);
            atomic_store_explicit(&ring_read_pos, r, memory_order_release);
            reader_gen = have;
        }
        avail = ring_used(atomic_load_explicit(&ring_write_pos, memory_order_acquire), r);
    }
    /* else: a newer utterance is on its way; hold silence rather than play
     * the tail of the one it replaced. */

    if (tts_disabling && avail > 0) tts_disabling_had_audio = true;
    if (tts_disabling && tts_disabling_had_audio && avail == 0 && have == want) {
        /* OFF was persisted by openevv_tts_set_enabled (non-RT). */
        tts_enabled = false;
        tts_disabling = false;
        tts_disabling_had_audio = false;
        return 0;
    }

    int frames = avail < max_frames ? avail : max_frames;
    float scale = tts_volume / 100.0f;
    for (int i = 0; i < frames; i++) {
        int32_t s = (int32_t)(ring[r] * scale);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out_buffer[2 * i] = (int16_t)s;
        out_buffer[2 * i + 1] = (int16_t)s;
        r = (r + 1) % RING_SIZE;
    }
    atomic_store_explicit(&ring_read_pos, r, memory_order_release);
    return frames;
}

/* The setters below are called before every utterance from the SPI path, so
 * they only record and poke; the worker persists. */

void openevv_tts_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (tts_volume == volume) return;
    tts_volume = volume;
    atomic_fetch_add_explicit(&config_seq, 1, memory_order_release);
    worker_poke();
}

void openevv_tts_set_speed(float speed) {
    if (speed < 0.5f) speed = 0.5f;
    if (speed > 6.0f) speed = 6.0f;
    if (tts_speed == speed) return;
    tts_speed = speed;
    atomic_fetch_add_explicit(&config_seq, 1, memory_order_release);
    worker_poke();
}

void openevv_tts_set_pitch(float pitch_hz) {
    if (pitch_hz < 80.0f) pitch_hz = 80.0f;
    if (pitch_hz > 180.0f) pitch_hz = 180.0f;
    if (tts_pitch == pitch_hz) return;
    tts_pitch = pitch_hz;
    atomic_fetch_add_explicit(&config_seq, 1, memory_order_release);
    worker_poke();
}

void openevv_tts_set_voice(const tts_evv_voice_t *voice) {
    if (!voice) return;
    tts_evv_voice_t v = *voice, cur;
    tts_evv_voice_clamp(&v);
    evv_voice_read(&cur);
    if (memcmp(&v, &cur, sizeof(v)) == 0) return;
    evv_voice_write(&v);
    atomic_fetch_add_explicit(&config_seq, 1, memory_order_release);
    worker_poke();
}

void openevv_tts_get_voice(tts_evv_voice_t *out) {
    if (!out) return;
    openevv_load_config_once();
    evv_voice_read(out);
}

void openevv_tts_set_enabled(bool enabled) {
    if (enabled == tts_enabled && !tts_disabling) return;

    if (enabled && tts_disabling) {
        tts_disabling = false;
        tts_disabling_had_audio = false;
        tts_enabled = true;
        openevv_save_state_value(1);
        return;
    }
    if (enabled && !tts_enabled) {
        tts_enabled = true;
        tts_disabling = false;
        openevv_save_state_value(1);
        return;
    }
    if (!enabled && tts_enabled && !tts_disabling) {
        openevv_save_state_value(0);
        tts_disabling_had_audio = false;
        openevv_tts_speak("screen reader off");
        tts_disabling = true;
    }
}

bool openevv_tts_get_enabled(void) { return tts_enabled; }
int  openevv_tts_get_volume(void) { return tts_volume; }
float openevv_tts_get_speed(void) { return tts_speed; }
float openevv_tts_get_pitch(void) { return tts_pitch; }
