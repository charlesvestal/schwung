/*
 * A fake tts_engine.h backend for test_tts_sidecar_roundtrip: links into the
 * real tts_sidecar.c in place of the eSpeak/Flite dispatcher. Each utterance
 * yields FAKE_FRAMES frames whose every sample is `utterance * 100 + volume` (int16, so up to utterance 326),
 * so the consumer can tell WHICH utterance a sample belongs to and whether a
 * setting reached the engine before the speak it preceded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tts_engine.h"

#define FAKE_FRAMES 20000

/* The "saved config" the engine loads at init -- deliberately not the
 * client's defaults, so a test can see whose values the shim ended up with. */
static bool enabled = true;
static int volume = 55;
static float speed = 1.5f, pitch = 120.0f;
static int engine_flite = 1;
static int utterance = 0, left = 0;

/* Every setter call is appended to $FAKE_TTS_LOG, so a test can assert
 * WHEN the sidecar touched the engine, not only what the audio says. */
static void note(const char *what)
{
    const char *p = getenv("FAKE_TTS_LOG");
    if (!p) return;
    FILE *f = fopen(p, "a");
    if (f) { fprintf(f, "%s\n", what); fclose(f); }
}

bool tts_init(int sr) { (void)sr; return true; }
void tts_cleanup(void) {}
bool tts_speak(const char *t)
{
    if (!enabled || !t || !*t) return false;
    utterance++;
    left = FAKE_FRAMES;
    return true;
}
bool tts_is_speaking(void) { return left > 0; }
int tts_get_audio(int16_t *out, int max)
{
    int n = left < max ? left : max;
    for (int i = 0; i < n * 2; i++) out[i] = (int16_t)(utterance * 100 + volume);
    left -= n;
    return n;
}
void tts_set_volume(int v) { note("volume"); volume = v; }
void tts_set_speed(float s) { note("speed"); speed = s; }
void tts_set_pitch(float p) { note("pitch"); pitch = p; }
void tts_set_enabled(bool e) { note("enabled"); enabled = e; }
bool tts_get_enabled(void) { return enabled; }
int tts_get_volume(void) { return volume; }
float tts_get_speed(void) { return speed; }
float tts_get_pitch(void) { return pitch; }
void tts_set_engine(const char *n) { note("engine"); engine_flite = n && strcmp(n, "flite") == 0; }
const char *tts_get_engine(void) { return engine_flite ? "flite" : "espeak"; }
