/*
 * openevv (Eloquence) screen reader engine: the pure halves.
 *
 *   - tts_openevv_map.h: UTF-8 -> Windows-1252 (what eciAddText reads) and
 *     the shared Speed row's multiplier -> ECI speed.
 *   - tts_config.c: the ONE writer of tts.json. Three writers each rewrote
 *     the whole file from their own key list, so any key one of them did not
 *     list reverted on that writer's next save. The round-trip below is the
 *     property that stops that: load, change one field, save, and every other
 *     field -- the Eloquence voice included -- is still there.
 */

#define _GNU_SOURCE   /* mkstemp under -std=c11 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tts_openevv_map.h"
#include "tts_config.h"

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } \
} while (0)

static void test_speed(void) {
    CHECK(tts_evv_speed_from_mult(1.0f) == 50, "1.0x must land on the preset speed, 50");
    CHECK(tts_evv_speed_from_mult(0.5f) == 25, "0.5x -> 25");
    CHECK(tts_evv_speed_from_mult(5.0f) == 250, "5.0x reaches the ceiling, 250");
    CHECK(tts_evv_speed_from_mult(6.0f) == 250, "above the ceiling clamps to 250");
    CHECK(tts_evv_speed_from_mult(-1.0f) == 0, "below zero clamps to 0");
}

static void test_cp1252(void) {
    char out[64];

    tts_evv_utf8_to_cp1252("Hello, world", out, sizeof(out));
    CHECK(strcmp(out, "Hello, world") == 0, "ASCII passes through, got '%s'", out);

    /* e-acute (U+00E9) is one byte 0xE9 in 1252 */
    tts_evv_utf8_to_cp1252("caf\xC3\xA9", out, sizeof(out));
    CHECK(strcmp(out, "caf\xE9") == 0, "Latin-1 range maps to its own byte");

    /* right single quote U+2019 -> 0x92, en dash U+2013 -> 0x96, euro -> 0x80 */
    tts_evv_utf8_to_cp1252("it\xE2\x80\x99s \xE2\x80\x93 \xE2\x82\xAC", out, sizeof(out));
    CHECK(strcmp(out, "it\x92s \x96 \x80") == 0, "the 0x80..0x9F block maps by table");

    /* Arrow U+2192 has no 1252 byte; control bytes must never reach the engine */
    tts_evv_utf8_to_cp1252("a\xE2\x86\x92" "b\tc\x01", out, sizeof(out));
    CHECK(strcmp(out, "a b c ") == 0, "unmappable and control characters become spaces, got '%s'", out);

    /* A truncated sequence at the end of the string must not read past it */
    tts_evv_utf8_to_cp1252("ab\xE2\x80", out, sizeof(out));
    CHECK(strncmp(out, "ab", 2) == 0 && strlen(out) <= 4, "a truncated sequence stops at the NUL");

    /* The output is bounded and always terminated */
    char tiny[4];
    size_t n = tts_evv_utf8_to_cp1252("abcdefgh", tiny, sizeof(tiny));
    CHECK(n == 3 && tiny[3] == '\0', "a short buffer truncates and terminates");

    /* The backtick is text, never an annotation: eciInputType stays 0. */
    tts_evv_utf8_to_cp1252("`vs 250", out, sizeof(out));
    CHECK(strcmp(out, "`vs 250") == 0, "a backtick is passed as text");
}

static void test_config_roundtrip(void) {
    char path[] = "/tmp/tts_cfg_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");
    if (fd < 0) return;
    close(fd);
    unlink(path);
    tts_config_set_path(path);

    tts_config_t c;
    CHECK(!tts_config_load(&c), "a missing file reports false");
    CHECK(strcmp(c.engine, "espeak") == 0 && c.speed == 1.0f && c.volume == 70,
          "...and still fills in the defaults");
    CHECK(c.evv.voice == 1 && c.evv.pitch == 65, "the default Eloquence voice is preset 1");

    strcpy(c.engine, "openevv");
    c.speed = 2.5f;
    c.pitch = 120.0f;
    c.volume = 40;
    c.evv = (tts_evv_voice_t){ 6, 1, 56, 89, 35, 7, 40 };
    CHECK(tts_config_save(&c), "save");

    /* What an eSpeak setter does: load, change ITS fields, save. */
    tts_config_t e;
    tts_config_load(&e);
    e.speed = 1.5f;
    e.volume = 55;
    CHECK(tts_config_save(&e), "second save");

    tts_config_t r;
    CHECK(tts_config_load(&r), "reload");
    CHECK(strcmp(r.engine, "openevv") == 0, "the engine survives another writer's save, got %s", r.engine);
    CHECK(r.speed == 1.5f && r.volume == 55, "the second writer's own fields landed");
    CHECK(r.pitch == 120.0f, "pitch survives");
    CHECK(r.evv.voice == 6 && r.evv.gender == 1 && r.evv.head == 56 && r.evv.pitch == 89 &&
          r.evv.inflection == 35 && r.evv.rough == 7 && r.evv.breath == 40,
          "the Eloquence voice survives a save by an engine that does not know it");

    /* "pitch" must never be read out of "evv_pitch" -- the keys are matched
     * with their quotes. A file with ONLY evv_pitch keeps the Hz default. */
    FILE *f = fopen(path, "w");
    fputs("{ \"evv_pitch\": 12 }\n", f);
    fclose(f);
    tts_config_load(&r);
    CHECK(r.pitch == 110.0f && r.evv.pitch == 12, "evv_pitch is not read as pitch");

    /* Out-of-range values are refused, not clamped into something else. */
    f = fopen(path, "w");
    fputs("{ \"engine\": \"bogus\", \"evv_voice\": 9, \"evv_gender\": 3, \"volume\": 400 }\n", f);
    fclose(f);
    tts_config_load(&r);
    CHECK(strcmp(r.engine, "espeak") == 0, "an unknown engine reads as espeak");
    CHECK(r.evv.voice == 1 && r.evv.gender == 0 && r.volume == 70, "out-of-range values keep defaults");

    /* The pre-openevv file shape still loads. */
    f = fopen(path, "w");
    fputs("{\n  \"engine\": \"flite\",\n  \"speed\": 1.20,\n  \"pitch\": 100.0,\n  \"volume\": 60\n}\n", f);
    fclose(f);
    tts_config_load(&r);
    CHECK(strcmp(r.engine, "flite") == 0 && r.speed > 1.19f && r.speed < 1.21f &&
          r.pitch == 100.0f && r.volume == 60 && r.evv.voice == 1,
          "an old tts.json loads, with the Eloquence voice defaulted");

    unlink(path);
    tts_config_set_path(NULL);
}

static void test_clamp(void) {
    tts_evv_voice_t v = { 0, 7, 200, 101, 255, 100, 0 };
    tts_evv_voice_clamp(&v);
    CHECK(v.voice == 1 && v.gender == 1 && v.head == 100 && v.pitch == 100 &&
          v.inflection == 100 && v.rough == 100 && v.breath == 0, "clamp");
}

int main(void) {
    test_speed();
    test_cp1252();
    test_config_roundtrip();
    test_clamp();
    if (failures) {
        fprintf(stderr, "test_tts_openevv: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: test_tts_openevv\n");
    return 0;
}
