/* The quantized sampler's preroll trim, run against a real file.
 *
 * The take is recorded THROUGH the preroll — that is what makes it
 * sample-accurate to the downbeat — and the preroll frames are then cut off
 * the front on stop. When that cut silently does nothing, what lands on the
 * card is the preroll, at exactly the right length, and the user reports it as
 * "the sampler is ignoring my preroll" rather than as a file that was never
 * rewritten. That is the shape this test exists to fail on.
 *
 * The content assertions matter more than the length one: a wrong
 * implementation that truncates without copying gets the LENGTH right and the
 * AUDIO wrong, so a test that only counted frames would have passed for the
 * whole time this was broken. */

#include "../../src/host/sampler_wav_trim.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

static int fails = 0;
static char last_log[256];

static size_t plain_write(FILE *f, const void *buf, size_t bytes) {
    return fwrite(buf, 1, bytes, f);
}
static void capture_log(const char *msg) {
    snprintf(last_log, sizeof(last_log), "%s", msg);
}

#define HDR 44u
#define BPF 4u   /* stereo s16 */

/* Byte i of frame n is (n & 0xff) — so the content of any frame identifies
 * which frame of the ORIGINAL recording it is. */
static void fill(unsigned char *p, unsigned frames) {
    for (unsigned n = 0; n < frames; n++)
        for (unsigned b = 0; b < BPF; b++)
            p[n * BPF + b] = (unsigned char)(n & 0xff);
}

/* Write a header + `frames` of identifiable audio, then hand back a stream.
 * `writable_only` reopens it O_WRONLY — the mode both callers used, and the
 * one fopen cannot express without also truncating what we just wrote. */
static FILE *make_file(const char *path, int writable_only, unsigned frames) {
    FILE *w = fopen(path, "wb");
    unsigned char hdr[HDR];
    memset(hdr, 'H', HDR);
    fwrite(hdr, 1, HDR, w);
    unsigned char *audio = malloc(frames * BPF);
    fill(audio, frames);
    fwrite(audio, 1, frames * BPF, w);
    free(audio);
    fclose(w);
    if (writable_only) {
        int fd = open(path, O_WRONLY);
        return fd < 0 ? NULL : fdopen(fd, "wb");
    }
    return fopen(path, "r+b");
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

int main(void) {
    const char *path = "/tmp/schwung_test_trim.wav";

    /* 1. The ordinary trim, on a file opened the way the sampler opens it.
     *    Two chunks' worth so the copy loop runs more than once. */
    {
        unsigned frames = (SAMPLER_TRIM_CHUNK_BYTES / BPF) * 2 + 777;
        unsigned preroll = 5000;
        FILE *f = make_file(path, 0, frames);
        unsigned n = frames;
        int moved = sampler_wav_trim_front_impl(f, HDR, BPF, preroll, &n,
                                                plain_write, capture_log);
        fflush(f);
        expect(moved == 1, "trim reports that it moved the audio");
        expect(n == frames - preroll, "frame count drops by the preroll");
        expect(file_size(path) == (long)(HDR + (frames - preroll) * BPF),
               "file is header + kept audio");

        /* The kept audio must START at original frame `preroll` and run to the
         * end of the take. A truncate-without-copy passes the two assertions
         * above and fails both of these. */
        fseek(f, HDR, SEEK_SET);
        unsigned char first[BPF], last[BPF];
        expect(fread(first, 1, BPF, f) == BPF, "first kept frame is readable");
        expect(first[0] == (unsigned char)(preroll & 0xff),
               "file now BEGINS at the first post-preroll frame");
        fseek(f, (long)(HDR + (n - 1) * BPF), SEEK_SET);
        expect(fread(last, 1, BPF, f) == BPF, "last kept frame is readable");
        expect(last[0] == (unsigned char)((frames - 1) & 0xff),
               "file still ENDS on the last recorded frame");
        fclose(f);
    }

    /* 2. The regression itself: a WRITE-ONLY stream, which is what both
     *    callers passed for five months. The read fails, so nothing can be
     *    copied — and the file must come back WHOLE rather than truncated to
     *    the right length around the wrong audio. */
    {
        unsigned frames = 20000, preroll = 5000;
        FILE *f = make_file(path, 1, frames);   /* write-only, as both callers were */
        unsigned n = frames;
        last_log[0] = '\0';
        int moved = sampler_wav_trim_front_impl(f, HDR, BPF, preroll, &n,
                                                plain_write, capture_log);
        fclose(f);
        expect(moved == 0, "a trim that could not read reports FAILURE");
        expect(n == frames, "frame count is left alone when nothing moved");
        expect(file_size(path) == (long)(HDR + frames * BPF),
               "the take is kept WHOLE — never truncated around unmoved audio");
        expect(last_log[0] != '\0', "the failure is logged, not silent");
    }

    /* 3. Degenerate inputs the caller can produce: a take shorter than its own
     *    preroll (stopped during the count-in) and a take with no preroll. */
    {
        unsigned frames = 100, preroll = 500;
        FILE *f = make_file(path, 0, frames);
        unsigned n = frames;
        expect(sampler_wav_trim_front_impl(f, HDR, BPF, preroll, &n,
                                           plain_write, capture_log) == 0,
               "preroll >= take is a no-op");
        expect(n == frames, "no-op leaves the frame count alone");
        expect(sampler_wav_trim_front_impl(f, HDR, BPF, 0, &n,
                                           plain_write, capture_log) == 0,
               "zero preroll is a no-op");
        fclose(f);
    }

    remove(path);
    if (fails == 0) printf("test_sampler_wav_trim: all checks passed\n");
    return fails ? 1 : 0;
}
