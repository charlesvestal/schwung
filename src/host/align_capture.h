/*
 * align_capture — RT-safe multi-stream audio capture for the SPI callback.
 *
 * WHY THIS EXISTS
 * ---------------
 * The align/main-FX dumps in schwung_shim.c call fopen()/fwrite() directly on
 * the SPI callback. That is a realtime violation on the one thread that must
 * never block (see docs/REALTIME_SAFETY.md), and for a diagnostic it is worse
 * than untidy: a 2.9 s dump does ~1000 buffered writes on the audio thread,
 * so the instrument can perturb the very timing fault it is measuring. On
 * 2026-08-27 the Move->Schwung splice ratio measured 5.61x, 1.84x, 1.01x,
 * 2.58x and 2.94x across five consecutive single-shot dumps of the same
 * configuration. That spread is not something to average over — it has to be
 * removed as a suspect before any number here can be trusted.
 *
 * THE SPLIT
 * ---------
 *   SPI callback :  align_capture_record()  — bounds check + memcpy. Nothing
 *                   else. No allocation, no I/O, no locks.
 *   Worker thread:  align_capture_poll()    — notices a finished stream and
 *                   writes it out.
 *
 * Allocation happens in align_capture_arm(), which the WORKER calls when it
 * consumes the trigger file — never the callback.
 *
 * OWNERSHIP: single producer (SPI callback), single consumer (worker). The
 * handoff is one release-store of `done` per stream against an acquire-load,
 * which is why no lock appears anywhere in here.
 */

#ifndef ALIGN_CAPTURE_H
#define ALIGN_CAPTURE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Streams captured in parallel. Each gets its own buffer and its own file. */
#define ALIGN_CAPTURE_MAX_STREAMS 4

/* Longest capture we will allocate, per stream, in stereo int16 SAMPLES.
 * 30 s @ 44.1 kHz stereo = 2,646,000 samples = 5.3 MB per stream. Four of
 * those is 21 MB, which /data/UserData has room for and / does not — see the
 * device-constraints note in CLAUDE.md about never writing to /tmp. */
#define ALIGN_CAPTURE_MAX_SAMPLES (30u * 44100u * 2u)

typedef struct {
    int16_t  *buf;          /* owned; NULL when this stream is unused      */
    uint32_t  capacity;     /* samples the buffer can hold                 */
    uint32_t  filled;       /* samples written so far (callback-only)      */
    int       done;         /* release-stored by callback, acquired by worker */
    int       written;      /* worker-only: already flushed to disk        */
    char      path[128];    /* destination file                            */
} align_capture_stream_t;

typedef struct {
    align_capture_stream_t streams[ALIGN_CAPTURE_MAX_STREAMS];
    int      stream_count;
    int      armed;         /* release-stored by worker, acquired by callback */
} align_capture_t;

/*
 * Arm a capture. WORKER THREAD ONLY — this allocates.
 *
 * `paths` holds `count` destination filenames; `samples_per_stream` is
 * clamped to ALIGN_CAPTURE_MAX_SAMPLES. Returns 0 on success, -1 on bad
 * arguments or allocation failure (in which case nothing is armed and any
 * partial allocation is released).
 *
 * Arming while already armed is refused rather than silently restarted: a
 * second trigger during a capture would otherwise truncate the first one and
 * produce a short file that looks like a starve.
 */
int align_capture_arm(align_capture_t *ac, const char *const *paths,
                      int count, uint32_t samples_per_stream);

/*
 * Append one block to a stream. SPI-CALLBACK-SAFE: bounds check + memcpy.
 *
 * Returns 1 if the samples were stored, 0 if they were not (not armed, bad
 * stream index, stream already full). A 0 is not an error the caller can do
 * anything about on the audio thread — it means the capture has ended.
 *
 * A stream that fills marks itself done; the worker takes it from there.
 */
int align_capture_record(align_capture_t *ac, int stream,
                         const int16_t *samples, uint32_t n);

/*
 * True once every armed stream has filled. Cheap; callback-safe.
 */
int align_capture_complete(const align_capture_t *ac);

/*
 * Write out any finished-but-unwritten stream and, once all are written,
 * free the buffers and disarm. WORKER THREAD ONLY — this does file I/O.
 *
 * Returns the number of files written by this call (0 is the common case).
 */
int align_capture_poll(align_capture_t *ac);

/*
 * Release everything without writing. Safe to call unarmed. WORKER ONLY.
 */
void align_capture_abort(align_capture_t *ac);

#ifdef __cplusplus
}
#endif

#endif /* ALIGN_CAPTURE_H */
