#define _GNU_SOURCE
/* move_info: the module-facing snapshot of Move's set (src/host/move_info.h).
 * Builds from a model, publishes through the real segment code into a plain
 * file, and reads it back the way a module does -- including an OLD module
 * (smaller struct) and a module on a host that has never published. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "move_info_pub.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

const char *move_model_quant_name(int v) { return v == 4 ? "bar" : NULL; }

static move_model_t model(void)
{
    static move_model_t m;
    memset(&m, 0, sizeof m);
    m.valid = 1; m.clock_valid = 1; m.playing = 1; m.song_beats = 12.5;
    m.tempo = 120.0; m.ts_upper = 7; m.ts_lower = 8; m.metronome_on = 1;
    m.master_valid = 1; m.master_db = -6.0; m.selected_track = 2;
    m.groove = 0.25; m.clock_sync = 0; m.input_monitor = 1; m.root_note = 9;
    strcpy(m.scale, "Dorian"); m.global_quant = 4;
    for (int t = 0; t < MM_TRACKS; t++) {
        m.track[t].mixer_valid = 1; m.track[t].volume = -3.0 * t; m.track[t].color_id = 10 + t;
        m.track[t].type = 1; m.track[t].selected = (t == 2);
    }
    m.track[1].muted = 1; m.track[3].soloed = 1;
    strcpy(m.track[0].name, "Drums");
    return m;
}

static void test_build(void)
{
    move_model_t m = model();
    move_info_t o;
    move_info_build(&m, 1, &o);
    CHECK(o.size == sizeof o && o.version == MOVE_INFO_VERSION && o.valid == 1);
    CHECK(o.playing == 1 && o.metronome_on == 1 && o.midi_clock_sync == 0 && o.input_monitoring == 1);
    CHECK(o.root_note == 9 && o.selected_track == 2 && o.global_quant == 4);
    CHECK(strcmp(o.global_quant_name, "bar") == 0 && strcmp(o.scale, "Dorian") == 0);
    CHECK(o.ts_upper == 7 && o.ts_lower == 8 && fabsf(o.tempo - 120.0f) < 1e-4);
    CHECK(fabsf(o.groove - 0.25f) < 1e-6 && fabsf(o.master_db + 6.0f) < 1e-6 && o.song_beats == 12.5);
    CHECK(strcmp(o.track[0].name, "Drums") == 0 && o.track[0].color_id == 10 && o.track[0].type == 1);
    CHECK(o.track[1].muted == 1 && o.track[3].soloed == 1 && o.track[2].selected == 1);
    CHECK(fabsf(o.track[3].volume_db + 9.0f) < 1e-6);

    /* Unknowns are explicit, never 0 that reads as a real value. */
    m.groove = -1; m.clock_sync = 255; m.root_note = -1; m.global_quant = -1; m.scale[0] = 0;
    m.clock_valid = 0; m.master_valid = 0; m.track[0].mixer_valid = 0; m.track[0].color_id = -1;
    move_info_build(&m, 1, &o);
    CHECK(o.groove < 0 && o.midi_clock_sync == 255 && o.root_note == -1 && o.global_quant == -1);
    CHECK(o.global_quant_name[0] == 0 && o.scale[0] == 0 && o.playing == 255 && o.song_beats < 0);
    CHECK(o.master_db <= -1000 && o.track[0].muted == 255 && o.track[0].volume_db <= -1000);
    CHECK(o.track[0].color_id == -1);

    /* Not live: everything unknown, whatever the model held. */
    m = model();
    move_info_build(&m, 0, &o);
    CHECK(o.valid == 0 && o.tempo == 0 && o.root_note == -1 && o.scale[0] == 0 && o.track[0].name[0] == 0);
}

/* The OLD module's view: the struct as version 1 laid it out, but pretend the
 * host has since APPENDED fields (a bigger writer). The reader must copy only
 * what it knows. And a NEW module on an OLD host: fields past the writer's
 * size come back zeroed with out->size telling which. */
static void test_publish_and_compat(void)
{
    unlink(MOVE_INFO_SHM_PATH);
    move_info_t got;
    CHECK(schwung_move_info(&got, sizeof got) == 0);     /* never published: "not provided" */

    move_model_t m = model();
    move_info_publish(&m, 1);
    memset(&got, 0xAB, sizeof got);
    CHECK(schwung_move_info(&got, sizeof got) == 1);
    CHECK(got.valid == 1 && got.root_note == 9 && strcmp(got.scale, "Dorian") == 0);
    uint32_t c1 = got.changes;

    /* Same model again: no change counted (the clock alone is not a change). */
    m.song_beats = 13.0;
    move_info_publish(&m, 1);
    CHECK(schwung_move_info(&got, sizeof got) == 1 && got.changes == c1 && got.song_beats == 13.0);
    /* A real change bumps it. */
    m.root_note = 2;
    move_info_publish(&m, 1);
    CHECK(schwung_move_info(&got, sizeof got) == 1 && got.changes == c1 + 1 && got.root_note == 2);

    /* An older reader asks for fewer bytes: only its prefix is written. */
    char small[40];
    memset(small, 0xCD, sizeof small);
    CHECK(schwung_move_info((move_info_t *)small, 32) == 1);
    CHECK((unsigned char)small[32] == 0xCD);             /* nothing past its size */

    /* A newer reader (bigger struct) on this host: the tail is zero, size says so. */
    struct { move_info_t v1; uint32_t future_field; } big;
    memset(&big, 0xEE, sizeof big);
    CHECK(schwung_move_info((move_info_t *)&big, sizeof big) == 1);
    CHECK(big.v1.size == sizeof(move_info_t) && big.future_field == 0);

    /* The model stops: valid goes to 0 once, values are withdrawn. */
    move_info_set_live(0);
    CHECK(schwung_move_info(&got, sizeof got) == 1 && got.valid == 0 && got.root_note == -1);

    /* The out-of-process path a module takes: map the file and copy. */
    int fd = open(MOVE_INFO_SHM_PATH, O_RDONLY);
    CHECK(fd >= 0);
    const move_info_shm_t *shm = mmap(NULL, sizeof *shm, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    CHECK(shm != MAP_FAILED && move_info_copy(shm, &got, sizeof got) == 1 && got.valid == 0);
    /* A torn segment (odd seq) never yields a copy. */
    move_info_shm_t fake = *shm;
    fake.seq |= 1;
    CHECK(move_info_copy(&fake, &got, sizeof got) == 0);
    /* Not a move-info segment. */
    memcpy(fake.magic, "NOTMINE", 8); fake.seq &= ~1u;
    CHECK(move_info_copy(&fake, &got, sizeof got) == 0);
}

int main(void)
{
    /* The public layout is a contract with module binaries: pin it. */
    CHECK(offsetof(move_info_t, size) == 0 && offsetof(move_info_t, version) == 4);
    CHECK(offsetof(move_info_t, tempo) == 24 && offsetof(move_info_t, song_beats) == 88);
    CHECK(sizeof(move_info_track_t) == 44 && sizeof(move_info_t) == 272);
    test_build();
    test_publish_and_compat();
    if (fails) { printf("test_move_info: %d FAILED\n", fails); return 1; }
    printf("test_move_info: PASS\n");
    return 0;
}
