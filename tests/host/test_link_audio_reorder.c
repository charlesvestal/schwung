/* link_audio_reorder.h: packets come out in sequence order whatever order
 * they arrive in, in-order delivery is untouched, and losses, stale
 * duplicates and restarts are counted rather than played. */
#include <stdio.h>
#include <string.h>
#include "link_audio_reorder.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define FR 125
static int out[256];        /* sequence numbers in emit order */
static int n_out;

static void emit(void *ctx, const int16_t *s, size_t frames)
{
    (void)ctx;
    CHECK(frames == FR);
    /* every sample of a packet carries its sequence number */
    for (size_t i = 0; i < frames * 2; i++) CHECK(s[i] == s[0]);
    out[n_out++] = s[0];
}

static void feed(la_reorder_t *r, const int *seq, int n)
{
    static int16_t buf[FR * 2];
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < FR * 2; k++) buf[k] = (int16_t)seq[i];
        la_reorder_push(r, (uint64_t)seq[i], buf, FR, emit, NULL);
    }
}

static int same(const int *want, int n)
{
    if (n_out != n) return 0;
    for (int i = 0; i < n; i++) if (out[i] != want[i]) return 0;
    return 1;
}

int main(void)
{
    static la_reorder_t r;

    /* In order: straight through, nothing held. */
    memset(&r, 0, sizeof r); n_out = 0;
    { int in[] = {10, 11, 12, 13}; feed(&r, in, 4);
      int want[] = {10, 11, 12, 13}; CHECK(same(want, 4)); CHECK(!r.held); }

    /* The measured case: a swapped pair is put back in order. */
    memset(&r, 0, sizeof r); n_out = 0;
    { int in[] = {1, 2, 4, 3, 5, 6}; feed(&r, in, 6);
      int want[] = {1, 2, 3, 4, 5, 6}; CHECK(same(want, 6));
      CHECK(r.reordered == 1 && r.lost == 0 && r.late == 0); }

    /* A real loss: the held packet goes out when the next one arrives. */
    memset(&r, 0, sizeof r); n_out = 0;
    { int in[] = {1, 2, 4, 5, 6}; feed(&r, in, 5);
      int want[] = {1, 2, 4, 5, 6}; CHECK(same(want, 5));
      CHECK(r.lost == 1 && r.reordered == 0); }

    /* A bigger gap is counted and played through. */
    memset(&r, 0, sizeof r); n_out = 0;
    { int in[] = {1, 5, 6}; feed(&r, in, 3);
      int want[] = {1, 5, 6}; CHECK(same(want, 3)); CHECK(r.lost == 3); }

    /* A stale duplicate is dropped, during a hold and outside one. */
    memset(&r, 0, sizeof r); n_out = 0;
    { int in[] = {1, 2, 3, 2, 5, 1, 4, 6}; feed(&r, in, 8);
      int want[] = {1, 2, 3, 4, 5, 6}; CHECK(same(want, 6));
      CHECK(r.late == 2 && r.reordered == 1); }

    /* A restart (sequence jumps far back) resynchronises. */
    memset(&r, 0, sizeof r); n_out = 0;
    { int in[] = {200, 201, 3, 4}; feed(&r, in, 4);
      int want[] = {200, 201, 3, 4}; CHECK(same(want, 4)); CHECK(r.resyncs == 1); }

    if (failures) { printf("test_link_audio_reorder: %d failure(s)\n", failures); return 1; }
    printf("test_link_audio_reorder: PASS\n");
    return 0;
}
