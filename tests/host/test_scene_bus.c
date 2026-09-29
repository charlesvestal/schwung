/*
 * SCENES on the shim's own buses, against fake FX plugins. What matters is
 * what each plugin was SENT: a bus has no chain_mod to ask.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "shadow_scene_bus.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 1e-3f)

typedef struct {
    char module[32];
    char mix[32];
    char mode[32];
    int writes;
} fake_fx_t;

static fake_fx_t fx[3][8];
static const char *PARAMS =
    "[{\"key\":\"mix\",\"name\":\"Mix {wet}\",\"type\":\"float\",\"min\":0,\"max\":2,\"default\":0.5},"
    "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"Hall\",\"Room\",\"Plate\"],\"default\":0},"
    "{\"key\":\"taps\",\"type\":\"int\",\"min\":1,\"max\":8}]";

static void *slot_at(int scope, int pos) { return fx[scope][pos].module[0] ? &fx[scope][pos] : NULL; }
static int positions(int scope) { return scope == 0 ? 8 : 4; }
static const char *module_id(void *s) { return ((fake_fx_t *)s)->module; }
static const char *chain_params(void *s) { (void)s; return PARAMS; }
static int get_param(void *s, const char *k, char *buf, int len) {
    fake_fx_t *f = s;
    if (!strcmp(k, "mix")) return snprintf(buf, len, "%s", f->mix);
    if (!strcmp(k, "mode")) return snprintf(buf, len, "%s", f->mode);
    return -1;
}
static void set_param(void *s, const char *k, const char *v) {
    fake_fx_t *f = s;
    f->writes++;
    if (!strcmp(k, "mix")) snprintf(f->mix, sizeof(f->mix), "%s", v);
    if (!strcmp(k, "mode")) snprintf(f->mode, sizeof(f->mode), "%s", v);
}
static const scene_bus_io_t io = { slot_at, positions, module_id, chain_params, get_param, set_param };

static float mix(int sc, int p) { return (float)atof(fx[sc][p].mix); }

int main(void) {
    scene_bus_meta_t m;
    CHECK(scene_bus_param_meta(PARAMS, "mix", &m) && m.kind == SCENE_KIND_FLOAT && NEAR(m.max, 2) && NEAR(m.def, 0.5),
          "meta: float with range and default (a brace inside a name does not end the object)");
    CHECK(scene_bus_param_meta(PARAMS, "mode", &m) && m.kind == SCENE_KIND_ENUM && m.option_count == 3 && NEAR(m.max, 2),
          "meta: enum from its options");
    CHECK(scene_bus_param_meta(PARAMS, "taps", &m) && m.kind == SCENE_KIND_INT && NEAR(m.min, 1) && NEAR(m.max, 8),
          "meta: int");
    CHECK(!scene_bus_param_meta(PARAMS, "nope", &m), "meta: an undeclared key is not found");
    CHECK(!scene_bus_param_meta(PARAMS, "mi", &m), "meta: a PREFIX of a key is not that key");
    CHECK(scene_bus_option_index(PARAMS, "mode", "Plate") == 2 && scene_bus_option_index(PARAMS, "mode", "x") == -1,
          "option name -> index");

    shadow_scene_bus_bind(&io);
    shadow_scene_bus_reset();
    CHECK(shadow_scene_bus_scope("master_fx", 9) == 0 && shadow_scene_bus_scope("send2", 5) == 2 &&
          shadow_scene_bus_scope("send3", 5) == -1, "scope names");

    snprintf(fx[0][1].module, 32, "cloudseed"); snprintf(fx[0][1].mix, 32, "0.5"); snprintf(fx[0][1].mode, 32, "Hall");
    snprintf(fx[1][0].module, 32, "cloudseed"); snprintf(fx[1][0].mix, 32, "1.0"); snprintf(fx[1][0].mode, 32, "0");

    shadow_scene_bus_set_verb(0, "lock", "0 fx2 mix 0.1 cloudseed");
    shadow_scene_bus_set_verb(0, "lock", "1 fx2 mix 1.9 cloudseed");
    uint16_t rev0 = shadow_scene_bus_rev();
    shadow_scene_bus_tick(0, 1, 0.0f, SCENE_NONE, 0);
    CHECK(NEAR(mix(0, 1), 0.1), "MFX x=0 is scene A: %f", mix(0, 1));
    shadow_scene_bus_tick(0, 1, 0.5f, SCENE_NONE, 0);
    CHECK(NEAR(mix(0, 1), 1.0), "MFX midpoint: %f", mix(0, 1));
    CHECK(shadow_scene_bus_rev() == rev0, "a fader move is not a table change");

    char buf[256];
    CHECK(shadow_scene_bus_read(0, 1, "mix", buf, sizeof(buf)) > 0 && NEAR(atof(buf), 0.5),
          "a plain read of a DRIVEN param answers the base: %s", buf);
    CHECK(shadow_scene_bus_read(0, 1, "mode", buf, sizeof(buf)) < 0, "an undriven param falls through");

    /* One end only: the knob is the other end, and it is LIVE. */
    shadow_scene_bus_set_verb(0, "unlock", "1 fx2 mix");
    shadow_scene_bus_tick(0, 1, 0.5f, SCENE_NONE, 0);
    CHECK(NEAR(mix(0, 1), 0.3), "A only: between A (0.1) and base (0.5): %f", mix(0, 1));
    set_param(&fx[0][1], "mix", "1.1");             /* the knob, straight to the plugin */
    shadow_scene_bus_note_write(0, 1, "mix", "1.1");
    shadow_scene_bus_tick(0, 1, 0.5f, SCENE_NONE, 0);
    CHECK(NEAR(mix(0, 1), 0.6), "turning the knob moves the unlocked end: %f", mix(0, 1));

    /* Neither end: the knob comes back. */
    shadow_scene_bus_tick(4, 5, 0.5f, SCENE_NONE, 0);
    CHECK(NEAR(mix(0, 1), 1.1), "locked in neither end: back to the knob: %f", mix(0, 1));
    CHECK(shadow_scene_bus_read(0, 1, "mix", buf, sizeof(buf)) < 0, "and nothing drives it any more");

    /* Dormant on a module swap, awake on its return. */
    shadow_scene_bus_tick(0, 1, 0.0f, SCENE_NONE, 0);
    snprintf(fx[0][1].module, 32, "psxverb");
    snprintf(fx[0][1].mix, 32, "0.7");
    for (int i = 0; i < 40; i++) shadow_scene_bus_tick(0, 1, 0.0f, SCENE_NONE, 0);
    CHECK(NEAR(mix(0, 1), 0.7),
          "swapped module: released, and the OLD module's base is NOT written into the new one: %f", mix(0, 1));
    shadow_scene_bus_get_verb(0, "dump", buf, sizeof(buf));
    CHECK(strncmp(buf, "0 fx2 mix 0.1", 13) == 0 && strstr(buf, " cloudseed\n") != NULL,
          "...and the lock is kept: %s", buf);

    /* Enum across 0.5 on a send, and the throttle does not drop it. */
    shadow_scene_bus_set_verb(1, "lock", "2 fx1 mode 0 cloudseed");
    shadow_scene_bus_set_verb(1, "lock", "3 fx1 mode 2 cloudseed");
    shadow_scene_bus_tick(2, 3, 0.4f, SCENE_NONE, 0);
    CHECK(atoi(fx[1][0].mode) == 0, "send enum below 0.5: A (%s)", fx[1][0].mode);
    shadow_scene_bus_tick(2, 3, 0.6f, SCENE_NONE, 0);
    usleep(60 * 1000);
    shadow_scene_bus_tick(2, 3, 0.6f, SCENE_NONE, 0);
    CHECK(atoi(fx[1][0].mode) == 2, "send enum past 0.5: B, even through the throttle (%s)", fx[1][0].mode);

    /* The edit arm. */
    shadow_scene_bus_tick(SCENE_NONE, SCENE_NONE, 0.0f, 7, 0);
    snprintf(fx[0][1].module, 32, "cloudseed");
    CHECK(shadow_scene_bus_edit_write(0, 1, "mix", "1.5") == 1, "armed write is consumed as a lock");
    CHECK(NEAR(mix(0, 1), 1.5), "...and heard at once: %f", mix(0, 1));
    CHECK(shadow_scene_bus_read(0, 1, "mix", buf, sizeof(buf)) > 0 && NEAR(atof(buf), 1.5),
          "an armed read answers the lock: %s", buf);
    CHECK(shadow_scene_bus_edit_write(0, 1, "mode", "Plate") == 1, "an enum by name locks");
    shadow_scene_bus_get_verb(0, "dump", buf, sizeof(buf));
    CHECK(strstr(buf, "7 fx2 mode 2 cloudseed") != NULL, "stored as its index");
    CHECK(shadow_scene_bus_edit_write(0, 1, "bypassed", "1") == 0, "bypass never locks");
    CHECK(shadow_scene_bus_edit_write(0, 1, "undeclared", "1") == 0 && shadow_scene_bus_take_flash() == SCENE_FLASH_NA,
          "an undeclared param goes to the base and flashes N/A");
    CHECK(shadow_scene_bus_take_flash() == SCENE_FLASH_NONE, "(one-shot)");
    CHECK(shadow_scene_bus_edit_write(0, 5, "mix", "1") == 0, "an empty position never locks");
    shadow_scene_bus_tick(SCENE_NONE, SCENE_NONE, 0.0f, 7, SCENE_EDIT_UNLOCK);
    CHECK(shadow_scene_bus_edit_write(0, 1, "mode", "Hall") == 1, "Delete+turn is consumed");
    shadow_scene_bus_get_verb(0, "dump", buf, sizeof(buf));
    CHECK(strstr(buf, "7 fx2 mode") == NULL && strstr(buf, "7 fx2 mix") != NULL, "...and unlocks only that param");
    shadow_scene_bus_tick(SCENE_NONE, SCENE_NONE, 0.0f, SCENE_NONE, 0);
    CHECK(shadow_scene_bus_edit_write(0, 1, "mix", "1") == 0, "disarmed: a write is not consumed");

    /* A state read sees the base; the morph is back after it. */
    shadow_scene_bus_tick(0, 1, 0.0f, SCENE_NONE, 0);
    float morphed = mix(0, 1);
    shadow_scene_bus_state_begin(0, 1);
    float during = mix(0, 1);
    shadow_scene_bus_state_end(0, 1);
    CHECK(fabsf(during - morphed) > 0.01f && NEAR(mix(0, 1), morphed),
          "state read: plugin held the base (%f) and the morph (%f) returned", during, mix(0, 1));

    shadow_scene_bus_get_verb(0, "locks", buf, sizeof(buf));
    CHECK(strcmp(buf, "1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0") == 0, "locks per scene: %s", buf);
    CHECK(shadow_scene_bus_get_verb(3, "dump", buf, sizeof(buf)) == -1, "an out-of-range scope is refused");

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
