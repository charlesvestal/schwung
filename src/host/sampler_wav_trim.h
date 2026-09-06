/* sampler_wav_trim.h - dropping the preroll off the front of an open WAV.
 *
 * A header, and a static inline, for the same reason sampler_stem_path.h is:
 * the take and each of its five stems go through this, and a second copy of
 * it is how the two halves drift. It is here rather than in shadow_sampler.c
 * so tests/host can run it against a real file on the dev machine — which is
 * the only thing that would have caught what it did for five months.
 *
 * WHAT IT DID: `f` must be open for READING as well as writing ("w+b"). Both
 * callers opened it "wb". The fread below then returned 0 on the very first
 * pass, the copy loop broke immediately — and the ftruncate ran anyway,
 * cutting `preroll_frames` off the END of a file whose front had never moved.
 * The saved take was the PREROLL, at exactly the right duration, so it read as
 * a recording that started early rather than as a trim that never happened.
 * Nothing failed loudly: the caller's "trimmed N preroll frames" log line was
 * printed on the strength of a return value of 1.
 *
 * So the truncate is conditional on the copy now. A short read leaves the file
 * WHOLE and reports it: a take that kept its preroll can be trimmed by hand,
 * one that has been truncated has lost the audio for good.
 *
 * Header is NOT rewritten here — the caller stamps the final size. Returns 1
 * if the audio moved and *frames was updated.
 *
 * Runs on the shim worker (SCHED_OTHER). The malloc, the seeks and the paced
 * writer are the reason this is not on the RT path. */

#ifndef SAMPLER_WAV_TRIM_H
#define SAMPLER_WAV_TRIM_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Writes `bytes` from `buf` at the stream's current position, returning the
 * count written. The shim passes its paced writer (the trim rewrites the whole
 * file, once per stem — six unpaced rewrites at stop is the burst the skipback
 * save was); a test passes plain fwrite. */
typedef size_t (*sampler_trim_write_fn)(FILE *f, const void *buf, size_t bytes);

/* Optional. NULL is allowed so the unit test can run without a host. */
typedef void (*sampler_trim_log_fn)(const char *msg);

#define SAMPLER_TRIM_CHUNK_BYTES (44100u * 2u * 2u)  /* ~1 second, stereo s16 */

static inline int sampler_wav_trim_front_impl(FILE *f,
                                              size_t header_size,
                                              unsigned bytes_per_frame,
                                              unsigned preroll_frames,
                                              unsigned *frames,
                                              sampler_trim_write_fn write_fn,
                                              sampler_trim_log_fn log_fn) {
    if (!f || !write_fn || preroll_frames == 0 || !frames) return 0;
    unsigned preroll_bytes = preroll_frames * bytes_per_frame;
    unsigned total_bytes = *frames * bytes_per_frame;
    if (preroll_bytes >= total_bytes) return 0;

    unsigned keep_bytes = total_bytes - preroll_bytes;
    unsigned keep_frames = *frames - preroll_frames;

    unsigned char *chunk = (unsigned char *)malloc(SAMPLER_TRIM_CHUNK_BYTES);
    if (!chunk) return 0;
    unsigned remaining = keep_bytes;
    unsigned read_offset = (unsigned)header_size + preroll_bytes;
    unsigned write_offset = (unsigned)header_size;
    int short_read = 0;
    while (remaining > 0) {
        unsigned to_copy = remaining < SAMPLER_TRIM_CHUNK_BYTES
                         ? remaining : SAMPLER_TRIM_CHUNK_BYTES;
        fseek(f, (long)read_offset, SEEK_SET);
        size_t got = fread(chunk, 1, to_copy, f);
        if (got == 0) { short_read = 1; break; }
        fseek(f, (long)write_offset, SEEK_SET);
        if (write_fn(f, chunk, got) != got) { short_read = 1; break; }
        read_offset += (unsigned)got;
        write_offset += (unsigned)got;
        remaining -= (unsigned)got;
    }
    free(chunk);

    if (short_read) {
        fflush(f);
        if (log_fn)
            log_fn("Sampler: preroll trim could not read the take back — "
                   "file kept whole, preroll NOT removed");
        return 0;
    }

    fflush(f);
    ftruncate(fileno(f), (off_t)(header_size + keep_bytes));
    *frames = keep_frames;
    return 1;
}

#endif /* SAMPLER_WAV_TRIM_H */
