#include "align_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- worker side ------------------------------------------------------- */

void align_capture_abort(align_capture_t *ac)
{
    if (!ac) return;
    /* Disarm FIRST, so a callback already inside align_capture_record cannot
     * pick up a buffer we are about to free. The acquire-load of `armed` in
     * record() pairs with this release-store. */
    __atomic_store_n(&ac->armed, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < ALIGN_CAPTURE_MAX_STREAMS; i++) {
        free(ac->streams[i].buf);
        ac->streams[i].buf = NULL;
    }
    memset(ac->streams, 0, sizeof(ac->streams));
    ac->stream_count = 0;
}

int align_capture_arm(align_capture_t *ac, const char *const *paths,
                      int count, uint32_t samples_per_stream)
{
    if (!ac || !paths) return -1;
    if (count <= 0 || count > ALIGN_CAPTURE_MAX_STREAMS) return -1;
    if (samples_per_stream == 0) return -1;

    /* Refuse rather than restart. A second trigger mid-capture would truncate
     * the first, and a short file is indistinguishable from a starved one. */
    if (__atomic_load_n(&ac->armed, __ATOMIC_ACQUIRE)) return -1;

    if (samples_per_stream > ALIGN_CAPTURE_MAX_SAMPLES)
        samples_per_stream = ALIGN_CAPTURE_MAX_SAMPLES;

    memset(ac->streams, 0, sizeof(ac->streams));
    ac->stream_count = count;

    for (int i = 0; i < count; i++) {
        if (!paths[i]) { align_capture_abort(ac); return -1; }
        ac->streams[i].buf =
            (int16_t *)malloc((size_t)samples_per_stream * sizeof(int16_t));
        if (!ac->streams[i].buf) { align_capture_abort(ac); return -1; }
        ac->streams[i].capacity = samples_per_stream;
        ac->streams[i].filled   = 0;
        ac->streams[i].done     = 0;
        ac->streams[i].written  = 0;
        snprintf(ac->streams[i].path, sizeof(ac->streams[i].path),
                 "%s", paths[i]);
    }

    /* Publish last: everything above must be visible before the callback can
     * observe armed != 0. */
    __atomic_store_n(&ac->armed, 1, __ATOMIC_RELEASE);
    return 0;
}

int align_capture_poll(align_capture_t *ac)
{
    if (!ac) return 0;
    if (!__atomic_load_n(&ac->armed, __ATOMIC_ACQUIRE)) return 0;

    int written_now = 0;
    int all_written = 1;

    for (int i = 0; i < ac->stream_count; i++) {
        align_capture_stream_t *s = &ac->streams[i];
        if (s->written) continue;

        /* Acquire against the callback's release-store of `done`, so the
         * samples it wrote are visible to us before we read them. */
        if (!__atomic_load_n(&s->done, __ATOMIC_ACQUIRE)) {
            all_written = 0;
            continue;
        }

        FILE *f = fopen(s->path, "wb");
        if (f) {
            fwrite(s->buf, sizeof(int16_t), s->filled, f);
            fclose(f);
            written_now++;
        }
        /* Mark written even if fopen failed — retrying every 200 ms worker
         * tick against a full or read-only filesystem would spin forever. */
        s->written = 1;
    }

    if (all_written) align_capture_abort(ac);
    return written_now;
}

/* ---- SPI callback side ------------------------------------------------- */
/* Everything below runs on the audio thread. Bounds check + memcpy only. */

int align_capture_record(align_capture_t *ac, int stream,
                         const int16_t *samples, uint32_t n)
{
    if (!ac || !samples || n == 0) return 0;
    if (!__atomic_load_n(&ac->armed, __ATOMIC_ACQUIRE)) return 0;
    if (stream < 0 || stream >= ac->stream_count) return 0;

    align_capture_stream_t *s = &ac->streams[stream];
    if (!s->buf) return 0;
    if (__atomic_load_n(&s->done, __ATOMIC_RELAXED)) return 0;

    uint32_t room = s->capacity - s->filled;
    if (n > room) {
        /* Take the partial block rather than dropping it: a capture that ends
         * mid-block is honest, one that silently omits the tail is not. */
        n = room;
    }
    if (n) {
        memcpy(s->buf + s->filled, samples, (size_t)n * sizeof(int16_t));
        s->filled += n;
    }

    if (s->filled >= s->capacity) {
        /* Release: the worker's acquire-load of `done` must see every sample
         * written above. */
        __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
    }
    return n > 0;
}

int align_capture_complete(const align_capture_t *ac)
{
    if (!ac) return 0;
    if (!__atomic_load_n(&ac->armed, __ATOMIC_ACQUIRE)) return 0;
    for (int i = 0; i < ac->stream_count; i++) {
        if (!__atomic_load_n(&ac->streams[i].done, __ATOMIC_ACQUIRE)) return 0;
    }
    return 1;
}
