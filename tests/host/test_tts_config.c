/*
 * tts_config.c: the ONE writer of tts.json.
 *
 * Three writers each rewrote the whole file from their own key list, so any
 * key one of them did not list reverted on that writer's next save. The
 * properties below are what stop that: a writer names the fields it owns and
 * every other field is still there.
 */

#define _GNU_SOURCE   /* mkstemp under -std=c11 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tts_config.h"

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } \
} while (0)

static bool temp_path(char *path) {
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");
    if (fd < 0) return false;
    close(fd);
    unlink(path);
    tts_config_set_path(path);
    return true;
}

static void test_roundtrip(void) {
    char path[] = "/tmp/tts_cfg_XXXXXX";
    if (!temp_path(path)) return;

    tts_config_t c;
    CHECK(!tts_config_load(&c), "a missing file reports false");
    CHECK(strcmp(c.engine, "espeak") == 0 && c.speed == 1.0f && c.pitch == 110.0f && c.volume == 70,
          "...and still fills in the defaults");

    strcpy(c.engine, "flite");
    c.speed = 2.5f;
    c.pitch = 120.0f;
    c.volume = 40;
    CHECK(tts_config_save(&c), "save");

    tts_config_t r;
    CHECK(tts_config_load(&r), "reload");
    CHECK(strcmp(r.engine, "flite") == 0 && r.speed == 2.5f && r.pitch == 120.0f && r.volume == 40,
          "what was saved is what loads");

    /* Out-of-range values are refused, not clamped into something else. */
    FILE *f = fopen(path, "w");
    fputs("{ \"engine\": \"bogus\", \"speed\": 9.0, \"pitch\": 20, \"volume\": 400 }\n", f);
    fclose(f);
    tts_config_load(&r);
    CHECK(strcmp(r.engine, "espeak") == 0, "an unknown engine reads as espeak");
    CHECK(r.speed == 1.0f && r.pitch == 110.0f && r.volume == 70, "out-of-range values keep defaults");

    /* The file shape the old writers produced still loads. */
    f = fopen(path, "w");
    fputs("{\n  \"engine\": \"flite\",\n  \"speed\": 1.20,\n  \"pitch\": 100.0,\n  \"volume\": 60\n}\n", f);
    fclose(f);
    tts_config_load(&r);
    CHECK(strcmp(r.engine, "flite") == 0 && r.speed > 1.19f && r.speed < 1.21f &&
          r.pitch == 100.0f && r.volume == 60,
          "an existing tts.json loads");

    unlink(path);
    tts_config_set_path(NULL);
}

/* A writer names the fields it owns; everything else is carried. */
static void test_update_fields(void) {
    char path[] = "/tmp/tts_cfg_XXXXXX";
    if (!temp_path(path)) return;

    tts_config_t c = {0};
    strcpy(c.engine, "espeak");
    c.speed = 2.0f;
    c.pitch = 130.0f;
    c.volume = 33;
    CHECK(tts_config_save(&c), "seed save");

    /* The dispatcher: the engine and nothing else. The other fields of what
     * it passes are zero, and must not land. */
    tts_config_t d = {0};
    strcpy(d.engine, "flite");
    CHECK(tts_config_update(TTS_CFG_ENGINE, &d), "engine-only update");

    tts_config_t r;
    CHECK(tts_config_load(&r), "reload");
    CHECK(strcmp(r.engine, "flite") == 0, "the engine-only update landed, got %s", r.engine);
    CHECK(r.speed == 2.0f && r.pitch == 130.0f && r.volume == 33,
          "speed, pitch and volume were carried by an update that did not name them");

    /* An engine's setter: speed, pitch, volume. Its engine field is empty. */
    tts_config_t e = {0};
    e.speed = 3.0f;
    e.pitch = 90.0f;
    e.volume = 80;
    CHECK(tts_config_update(TTS_CFG_SPEED | TTS_CFG_PITCH | TTS_CFG_VOLUME, &e), "setter update");

    CHECK(tts_config_load(&r), "reload");
    CHECK(r.speed == 3.0f && r.pitch == 90.0f && r.volume == 80, "the setter's fields landed");
    CHECK(strcmp(r.engine, "flite") == 0, "the engine was carried by the setter, got %s", r.engine);

    unlink(path);
    tts_config_set_path(NULL);
}

/* The writers as they really run: one thread, the dispatcher saving the
 * engine and an engine saving its voice, interleaved. Before tts_config.c
 * each re-parsed the file and rewrote it from its own key list; now each
 * names its fields, and after every save the FILE must hold both owners'
 * latest values. An engine name it cannot write back is ignored rather than
 * stored. */
static void test_interleaved_owners(void) {
    char path[] = "/tmp/tts_cfg_XXXXXX";
    if (!temp_path(path)) return;

    tts_config_t c = {0};
    strcpy(c.engine, "espeak");
    c.speed = 1.0f;
    c.pitch = 110.0f;
    c.volume = 70;
    CHECK(tts_config_save(&c), "seed save");

    int lost = 0;
    for (int i = 0; i < 200; i++) {
        tts_config_t eng = {0};
        strcpy(eng.engine, (i & 1) ? "flite" : "espeak");
        tts_config_update(TTS_CFG_ENGINE, &eng);

        tts_config_t voice = {0};
        voice.speed = 1.0f + (float)(i % 5);
        voice.pitch = 100.0f;
        voice.volume = i % 101;
        tts_config_update(TTS_CFG_SPEED | TTS_CFG_PITCH | TTS_CFG_VOLUME, &voice);

        tts_config_t r;
        tts_config_load(&r);
        if (r.volume != i % 101 || r.speed != voice.speed ||
            strcmp(r.engine, eng.engine) != 0) lost++;
    }
    CHECK(lost == 0, "an owner's value was missing from the file in %d of 200 rounds", lost);

    tts_config_t bogus = {0};
    strcpy(bogus.engine, "openevv");
    tts_config_update(TTS_CFG_ENGINE, &bogus);
    tts_config_t r;
    tts_config_load(&r);
    CHECK(strcmp(r.engine, "flite") == 0,
          "an engine name the file cannot carry replaced the real one: %s", r.engine);

    /* A HAND EDIT survives the next save, once something has re-read it:
     * the engines re-load on init, and the save after that must merge into
     * the edited file rather than into what the cache held before it. */
    FILE *f = fopen(path, "w");
    fprintf(f, "{ \"engine\": \"espeak\", \"speed\": 2.50, \"pitch\": 150.0, \"volume\": 40 }\n");
    fclose(f);
    tts_config_load(&r);                       /* an engine init re-reads it */
    tts_config_t only_vol = {0};
    only_vol.volume = 55;
    tts_config_update(TTS_CFG_VOLUME, &only_vol);
    tts_config_load(&r);
    CHECK(r.speed == 2.5f && r.pitch == 150.0f && strcmp(r.engine, "espeak") == 0 && r.volume == 55,
          "a save after re-reading a hand edit reverted it: engine=%s speed=%.2f pitch=%.1f volume=%d",
          r.engine, r.speed, r.pitch, r.volume);

    unlink(path);
    tts_config_set_path(NULL);
}

int main(void) {
    test_roundtrip();
    test_update_fields();
    test_interleaved_owners();
    if (failures) {
        fprintf(stderr, "test_tts_config: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_tts_config: ok\n");
    return 0;
}
