/*
 * Unit tests for align_capture — the RT-safe replacement for the shim's
 * fwrite-on-the-SPI-callback audio dumps.
 *
 * The properties worth pinning are the ones that make it safe to call from
 * the audio thread, plus the two failure shapes that would make a capture
 * lie about what it recorded:
 *
 *   - a partial final block is KEPT, not dropped (a short honest file beats
 *     one silently missing its tail)
 *   - a second arm during a capture is REFUSED, not restarted (a restart
 *     truncates the first file, and a short file is indistinguishable from
 *     a starved one — which is exactly the misreading this whole module
 *     exists to prevent)
 */

#include "align_capture.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); failures++; } \
    else         { printf("  ok:   %s\n", (msg)); }             \
} while (0)

static char tmpdir[256];

static void make_tmpdir(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/align_capture_test_%d", (int)getpid());
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", tmpdir);
    if (system(cmd) != 0) { printf("mkdir failed\n"); exit(1); }
}

static void rm_tmpdir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd) != 0) { /* best effort */ }
}

static long file_size(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* ---------------------------------------------------------------------- */

static void test_basic_roundtrip(void)
{
    printf("basic capture round-trip\n");
    align_capture_t ac;
    memset(&ac, 0, sizeof(ac));

    char p0[320], p1[320];
    snprintf(p0, sizeof(p0), "%s/a.pcm", tmpdir);
    snprintf(p1, sizeof(p1), "%s/b.pcm", tmpdir);
    const char *paths[2] = { p0, p1 };

    CHECK(align_capture_arm(&ac, paths, 2, 1000) == 0, "arm succeeds");
    CHECK(align_capture_complete(&ac) == 0, "not complete when just armed");

    int16_t block[100];
    for (int i = 0; i < 100; i++) block[i] = (int16_t)i;

    /* Nothing is written to disk until the worker polls. */
    for (int k = 0; k < 10; k++) {
        CHECK(align_capture_record(&ac, 0, block, 100) == 1, "record stream 0");
        CHECK(align_capture_record(&ac, 1, block, 100) == 1, "record stream 1");
    }
    CHECK(align_capture_complete(&ac) == 1, "complete once both filled");
    CHECK(align_capture_record(&ac, 0, block, 100) == 0, "record refused when full");

    CHECK(align_capture_poll(&ac) == 2, "poll writes both files");
    CHECK(file_size(p0) == 2000, "stream 0 is 1000 int16 = 2000 bytes");
    CHECK(file_size(p1) == 2000, "stream 1 is 1000 int16 = 2000 bytes");
    CHECK(ac.armed == 0, "disarmed after everything is written");
    CHECK(ac.streams[0].buf == NULL, "buffers freed after write");

    /* Content survived the round trip. */
    FILE *f = fopen(p0, "rb");
    assert(f);
    int16_t back[1000];
    size_t got = fread(back, sizeof(int16_t), 1000, f);
    fclose(f);
    CHECK(got == 1000, "read back 1000 samples");
    CHECK(back[0] == 0 && back[99] == 99 && back[100] == 0,
          "content matches what was recorded");
}

static void test_partial_block_is_kept(void)
{
    printf("a partial final block is kept, not dropped\n");
    align_capture_t ac;
    memset(&ac, 0, sizeof(ac));

    char p0[320];
    snprintf(p0, sizeof(p0), "%s/partial.pcm", tmpdir);
    const char *paths[1] = { p0 };

    /* Capacity deliberately not a multiple of the block size. */
    CHECK(align_capture_arm(&ac, paths, 1, 250) == 0, "arm 250");

    int16_t block[100];
    for (int i = 0; i < 100; i++) block[i] = (int16_t)(i + 1);

    CHECK(align_capture_record(&ac, 0, block, 100) == 1, "block 1");
    CHECK(align_capture_record(&ac, 0, block, 100) == 1, "block 2");
    /* Only 50 samples of room left. The block must be truncated, not lost. */
    CHECK(align_capture_record(&ac, 0, block, 100) == 1, "block 3 truncated but accepted");
    CHECK(align_capture_complete(&ac) == 1, "complete");

    CHECK(align_capture_poll(&ac) == 1, "written");
    CHECK(file_size(p0) == 500, "exactly 250 samples on disk, tail kept");
}

static void test_rearm_is_refused(void)
{
    printf("re-arming during a capture is refused\n");
    align_capture_t ac;
    memset(&ac, 0, sizeof(ac));

    char p0[320], p1[320];
    snprintf(p0, sizeof(p0), "%s/first.pcm", tmpdir);
    snprintf(p1, sizeof(p1), "%s/second.pcm", tmpdir);
    const char *first[1]  = { p0 };
    const char *second[1] = { p1 };

    CHECK(align_capture_arm(&ac, first, 1, 1000) == 0, "first arm ok");
    int16_t block[100] = {0};
    align_capture_record(&ac, 0, block, 100);

    CHECK(align_capture_arm(&ac, second, 1, 1000) == -1, "second arm REFUSED");
    CHECK(strcmp(ac.streams[0].path, p0) == 0, "first capture untouched");
    CHECK(ac.streams[0].filled == 100, "first capture kept its samples");

    align_capture_abort(&ac);
    CHECK(file_size(p1) == -1, "refused arm wrote no file");
}

static void test_unarmed_is_inert(void)
{
    printf("an unarmed capture is inert and never crashes\n");
    align_capture_t ac;
    memset(&ac, 0, sizeof(ac));

    int16_t block[16] = {0};
    CHECK(align_capture_record(&ac, 0, block, 16) == 0, "record on unarmed = 0");
    CHECK(align_capture_complete(&ac) == 0, "complete on unarmed = 0");
    CHECK(align_capture_poll(&ac) == 0, "poll on unarmed = 0");
    align_capture_abort(&ac);
    CHECK(1, "abort on unarmed does not crash");

    /* Out-of-range stream indices must be rejected by the COUNT check, not by
     * happening to find a NULL buffer.
     *
     * Arm every stream, so all buffers are non-NULL. Now the only thing
     * standing between `record(ac, ALIGN_CAPTURE_MAX_STREAMS, ...)` and a read
     * off the end of the array is `stream >= ac->stream_count`. Arming just
     * ONE stream hides that: streams[1..3] are zeroed by arm(), so the
     * `!s->buf` early-out catches an in-range index and the bound looks
     * redundant when it is not. Deleting the bound survives that weaker test,
     * which is how this one came to be written. */
    char pf[ALIGN_CAPTURE_MAX_STREAMS][320];
    const char *full[ALIGN_CAPTURE_MAX_STREAMS];
    for (int i = 0; i < ALIGN_CAPTURE_MAX_STREAMS; i++) {
        snprintf(pf[i], sizeof(pf[i]), "%s/inert%d.pcm", tmpdir, i);
        full[i] = pf[i];
    }
    CHECK(align_capture_arm(&ac, full, ALIGN_CAPTURE_MAX_STREAMS, 100) == 0,
          "arm all streams");
    CHECK(align_capture_record(&ac, -1, block, 16) == 0, "stream -1 rejected");
    CHECK(align_capture_record(&ac, ALIGN_CAPTURE_MAX_STREAMS, block, 16) == 0,
          "stream at max rejected with every buffer non-NULL");
    CHECK(align_capture_record(&ac, ALIGN_CAPTURE_MAX_STREAMS + 3, block, 16) == 0,
          "stream well past max rejected");
    /* Nothing above should have disturbed a real stream. */
    CHECK(ac.streams[0].filled == 0, "no in-range stream was written by a bad index");
    align_capture_abort(&ac);

    /* And with a partial arm, an index inside the array but past the count is
     * still refused. */
    const char *one[1];
    char p0[320];
    snprintf(p0, sizeof(p0), "%s/inert.pcm", tmpdir);
    one[0] = p0;
    align_capture_arm(&ac, one, 1, 100);
    CHECK(align_capture_record(&ac, 1, block, 16) == 0, "stream past count rejected");
    align_capture_abort(&ac);
}

static void test_arm_validates(void)
{
    printf("arm rejects bad arguments\n");
    align_capture_t ac;
    memset(&ac, 0, sizeof(ac));
    char p0[320];
    snprintf(p0, sizeof(p0), "%s/v.pcm", tmpdir);
    const char *paths[1] = { p0 };

    CHECK(align_capture_arm(&ac, paths, 0, 100) == -1, "count 0 rejected");
    CHECK(align_capture_arm(&ac, paths, ALIGN_CAPTURE_MAX_STREAMS + 1, 100) == -1,
          "count over max rejected");
    CHECK(align_capture_arm(&ac, paths, 1, 0) == -1, "zero samples rejected");
    CHECK(align_capture_arm(&ac, NULL, 1, 100) == -1, "NULL paths rejected");
    CHECK(ac.armed == 0, "still unarmed after every rejection");

    /* Over-long requests clamp rather than fail. */
    CHECK(align_capture_arm(&ac, paths, 1, ALIGN_CAPTURE_MAX_SAMPLES * 2) == 0,
          "oversized request is clamped, not refused");
    CHECK(ac.streams[0].capacity == ALIGN_CAPTURE_MAX_SAMPLES, "clamped to max");
    align_capture_abort(&ac);
}

/* ---------------------------------------------------------------------- */
/* Producer/consumer handoff: the callback fills while the worker polls.   */

typedef struct {
    align_capture_t *ac;
    volatile int stop;
} producer_arg_t;

static void *producer(void *v)
{
    producer_arg_t *a = (producer_arg_t *)v;
    int16_t block[128];
    for (int i = 0; i < 128; i++) block[i] = (int16_t)i;
    while (!a->stop) {
        if (!align_capture_record(a->ac, 0, block, 128)) break;
    }
    return NULL;
}

static void test_concurrent_handoff(void)
{
    printf("producer fills while consumer polls\n");
    align_capture_t ac;
    memset(&ac, 0, sizeof(ac));

    char p0[320];
    snprintf(p0, sizeof(p0), "%s/concurrent.pcm", tmpdir);
    const char *paths[1] = { p0 };
    const uint32_t cap = 128u * 500u;
    CHECK(align_capture_arm(&ac, paths, 1, cap) == 0, "arm");

    producer_arg_t arg = { &ac, 0 };
    pthread_t th;
    pthread_create(&th, NULL, producer, &arg);

    /* Poll like the worker does, ~200 ms cadence compressed. */
    int wrote = 0;
    for (int i = 0; i < 2000 && !wrote; i++) {
        wrote = align_capture_poll(&ac);
        usleep(500);
    }
    arg.stop = 1;
    pthread_join(th, NULL);

    CHECK(wrote == 1, "worker wrote exactly one file");
    CHECK(file_size(p0) == (long)cap * 2, "file is the full capacity");
    CHECK(ac.armed == 0, "disarmed after the handoff");
}

int main(void)
{
    make_tmpdir();
    printf("align_capture tests\n\n");
    test_basic_roundtrip();
    test_partial_block_is_kept();
    test_rearm_is_refused();
    test_unarmed_is_inert();
    test_arm_validates();
    test_concurrent_handoff();
    rm_tmpdir();

    printf("\n%s\n", failures ? "FAILED" : "all align_capture tests passed");
    return failures ? 1 : 0;
}
