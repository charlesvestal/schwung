/*
 * The shim's TTS client (src/host/tts_client.c) against the REAL sidecar
 * main loop (src/host/tts_sidecar.c) with a fake engine, in two processes
 * over the real /schwung-tts segment -- the shape that runs on the device,
 * minus eSpeak. Proves:
 *   1. the shim adopts the settings the sidecar LOADED (not its defaults),
 *      and sends nothing before they arrive;
 *   2. a setting made before a speak reaches the engine before that speak;
 *   3. a new utterance cuts the old one: once its audio starts, not one
 *      sample of the previous utterance follows;
 *   4. a restarted sidecar republishes, and the client re-adopts.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "tts_engine.h"
#include "tts_shm.h"

static int fails = 0;
#define CHECK(c, ...) do { if (c) { printf("PASS: " __VA_ARGS__); printf("\n"); } \
    else { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *sidecar_bin;

static pid_t spawn(void)
{
    pid_t p = fork();
    if (p == 0) { execl(sidecar_bin, "schwung-tts", (char *)0); _exit(127); }
    return p;
}

static int wait_state(int ms)
{
    for (int i = 0; i < ms; i++) {
        if (tts_take_state_published()) return 1;
        usleep(1000);
    }
    return 0;
}

/* Drain like the mix does (128 frames per ~2.9 ms block), returning the
 * first sample value seen, or -1 on timeout. */
static int first_value(int ms)
{
    int16_t buf[256];
    for (int i = 0; i < ms; i++) {
        int n = tts_get_audio(buf, 128);
        if (n > 0) return buf[0];
        usleep(1000);
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <sidecar>\n", argv[0]); return 2; }
    sidecar_bin = argv[1];
    shm_unlink(SHM_TTS);
    if (!getenv("FAKE_TTS_LOG")) { fprintf(stderr, "FAKE_TTS_LOG unset\n"); return 2; }
    unlink(getenv("FAKE_TTS_LOG"));

    CHECK(tts_init(44100), "client creates the segment");
    CHECK(!tts_speak("too early"), "speak before the sidecar publishes is refused");

    pid_t sc = spawn();
    CHECK(wait_state(3000), "sidecar publishes its state");
    CHECK(tts_get_volume() == 55 && tts_get_enabled() &&
          strcmp(tts_get_engine(), "flite") == 0 &&
          tts_get_speed() == 1.5f && tts_get_pitch() == 120.0f,
          "client adopts the LOADED settings (vol=%d eng=%s)",
          tts_get_volume(), tts_get_engine());

    /* The segment was zero when the sidecar started -- volume 0, speed 0,
     * pitch 0 in the request block. Those are NOT a request: measured on the
     * device, a sidecar that applied them wrote volume=0/speed=0.5/pitch=80
     * into tts.json at every boot until the shim's first real write. */
    usleep(50000);
    {
        FILE *f = fopen(getenv("FAKE_TTS_LOG"), "r");
        int calls = 0;
        char line[64];
        while (f && fgets(line, sizeof(line), f)) calls++;
        if (f) fclose(f);
        CHECK(calls == 0, "the sidecar made no engine setter call before any request (%d calls)", calls);
    }
    CHECK(tts_speak("zero"), "speak accepted");
    int v = first_value(2000);
    CHECK(v == 155, "the zeroed request block was not applied (got %d, want 155 = vol 55)", v);

    tts_set_volume(30);
    CHECK(tts_speak("one"), "speak accepted");
    v = first_value(2000);
    CHECK(v == 230, "utterance 2 arrives with the volume set before it (got %d)", v);

    /* Let utterance 1 keep flowing, then cut it with utterance 2. */
    int16_t buf[256];
    for (int i = 0; i < 20; i++) { tts_get_audio(buf, 128); usleep(1000); }
    CHECK(tts_speak("two"), "second speak accepted");
    int seen2 = 0, stale_after = 0, total = 0;
    for (int i = 0; i < 1500 && total < 4000; i++) {
        int n = tts_get_audio(buf, 128);
        for (int k = 0; k < n * 2; k++) {
            if (buf[k] == 330) seen2 = 1;
            else if (seen2) stale_after++;
        }
        if (seen2) total += n;
        usleep(1000);
    }
    CHECK(seen2, "utterance 3 plays");
    CHECK(stale_after == 0, "no utterance-2 sample after utterance 3 starts (%d stale)", stale_after);

    /* The race the speak hand-off exists for: a cut that lands while the
     * sidecar is mid-pump. The client's own cut empties the ring, but the
     * sidecar may push the OLD utterance once more before it reads the new
     * request; discard_until is what drops that. One clean cut proves
     * little, so cut hundreds of times at varying offsets: after a speak,
     * every sample must belong to an utterance NEWER than any seen before. */
    {
        int newest = 3, stale = 0;
        srand(1);
        for (int round = 0; round < 400 && newest < 300; round++) {
            int before = newest;
            tts_speak("x");
            int spin = rand() % 4000;
            for (int t = 0; t < 6; t++) {
                int n = tts_get_audio(buf, 128);
                for (int k = 0; k < n * 2; k++) {
                    int id = buf[k] / 100;
                    if (id <= before) stale++;
                    else if (id > newest) newest = id;
                }
                usleep(spin / 6 + 200);
            }
        }
        CHECK(stale == 0, "400 cuts at varying offsets: no sample of an older utterance (%d stale)", stale);
        CHECK(newest > 3, "the cuts produced new utterances (newest %d)", newest);
    }

    /* Restart: the new sidecar loads the engine's defaults again. */
    kill(sc, SIGTERM);
    waitpid(sc, NULL, 0);
    sc = spawn();
    CHECK(wait_state(3000), "restarted sidecar republishes");
    CHECK(tts_get_volume() == 55, "client re-adopts after restart (vol=%d)", tts_get_volume());
    CHECK(tts_speak("three"), "speak after restart accepted");
    v = first_value(2000);
    CHECK(v == 155, "first utterance of the new sidecar arrives (got %d)", v);

    kill(sc, SIGTERM);
    waitpid(sc, NULL, 0);
    shm_unlink(SHM_TTS);
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
