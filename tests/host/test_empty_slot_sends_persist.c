/* An empty track's Send A/B come back from the set's shadow_chain_config.json
 * (shadow_load_config_from_dir), each slot taking only its OWN pair. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shadow_set_pages.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int move_model_sync_active(void) { return 0; }
uint32_t move_model_sync_gen(void) { return 0; }
int move_model_sync_settled(void) { return 0; }
int move_model_sync_misaligned(void) { return 0; }
float sampler_read_set_tempo(const char *n) { (void)n; return 0; }

static void s_log(const char *m) { (void)m; }
static int s_run(const char *const argv[]) { (void)argv; return 0; }
static void s_void(void) {}
static void s_slot(int s) { (void)s; }
static int s_ch(int c) { return c; }
static float s_tempo(const char *n) { (void)n; return 120.0f; }
static int s_mutes(const char *n, int m[4], int so[4]) { (void)n; (void)m; (void)so; return 0; }

static shadow_control_t ctl_s;
static shadow_control_t *ctl = &ctl_s;
static shadow_chain_slot_t slots[4];
static volatile int solo;

static void write_cfg(const char *dir, const char *body)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/shadow_chain_config.json", dir);
    FILE *f = fopen(path, "w");
    fputs(body, f);
    fclose(f);
}

int main(void)
{
    set_pages_host_t h = { s_log, s_log, s_void, s_run, s_void, s_mutes, s_tempo, s_slot, s_void, s_ch,
                           slots, &ctl, &solo };
    set_pages_init(&h);

    char dir[] = "/tmp/empty_sends_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }

    /* The shape saveChainConfigToDir writes: pretty-printed, pan before sends. */
    write_cfg(dir,
        "{\n  \"slots\": [\n"
        "    {\n      \"name\": \"\",\n      \"channel\": 1,\n      \"volume\": 1,\n      \"pan\": 0,\n"
        "      \"empty_sends\": [\n        100,\n        0\n      ],\n"
        "      \"forward_channel\": -1,\n      \"muted\": 0,\n      \"soloed\": 0\n    },\n"
        "    {\n      \"name\": \"Untitled\",\n      \"channel\": 2,\n      \"volume\": 1,\n      \"pan\": 0,\n"
        "      \"empty_sends\": [0, 64],\n"
        "      \"forward_channel\": -1,\n      \"muted\": 0,\n      \"soloed\": 0\n    },\n"
        "    {\n      \"name\": \"\",\n      \"channel\": 3,\n      \"volume\": 1,\n"
        "      \"forward_channel\": -1,\n      \"muted\": 0,\n      \"soloed\": 0\n    },\n"
        "    {\n      \"name\": \"\",\n      \"channel\": 4,\n      \"volume\": 1,\n      \"pan\": 0,\n"
        "      \"empty_sends\": [999, -5],\n"
        "      \"forward_channel\": -1,\n      \"muted\": 0,\n      \"soloed\": 0\n    }\n  ]\n}\n");

    memset(slots, 0, sizeof(slots));
    CHECK(shadow_load_config_from_dir(dir) == 1, "config loads");
    CHECK(slots[0].empty_send[0] == 100 && slots[0].empty_send[1] == 0,
          "slot 1 sends %d,%d", slots[0].empty_send[0], slots[0].empty_send[1]);
    CHECK(slots[1].empty_send[0] == 0 && slots[1].empty_send[1] == 64,
          "slot 2 sends %d,%d", slots[1].empty_send[0], slots[1].empty_send[1]);
    /* No field on slot 3: it must NOT pick up slot 4's. */
    CHECK(slots[2].empty_send[0] == 0 && slots[2].empty_send[1] == 0,
          "slot 3 (no field) sends %d,%d", slots[2].empty_send[0], slots[2].empty_send[1]);
    CHECK(slots[3].empty_send[0] == 127 && slots[3].empty_send[1] == 0,
          "slot 4 clamped to 0..127: %d,%d", slots[3].empty_send[0], slots[3].empty_send[1]);

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd)) {}
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: empty-slot sends load per slot\n");
    return 0;
}
