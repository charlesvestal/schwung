#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

extern midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host);

static int g_clock_status = 0;

static int test_get_clock_status(void) {
    return g_clock_status;
}

static void fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

int main(void) {
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.get_clock_status = test_get_clock_status;

    midi_fx_api_v1_t *api = move_midi_fx_init(&host);
    if (!api) fail("move_midi_fx_init returned NULL");
    if (!api->create_instance || !api->set_param || !api->get_param || !api->destroy_instance) {
        fail("API missing required callbacks");
    }

    void *inst = api->create_instance(".", NULL);
    if (!inst) fail("create_instance returned NULL");

    api->set_param(inst, "sync", "clock");

    char buf[256];
    int len;

    g_clock_status = MOVE_CLOCK_STATUS_UNAVAILABLE;
    memset(buf, 0, sizeof(buf));
    len = api->get_param(inst, "error", buf, sizeof(buf));
    if (len <= 0 || strstr(buf, "Enable MIDI Clock Out") == NULL) {
        fail("expected unavailable warning for sync=clock");
    }

    g_clock_status = MOVE_CLOCK_STATUS_STOPPED;
    memset(buf, 0, sizeof(buf));
    len = api->get_param(inst, "error", buf, sizeof(buf));
    if (len <= 0 || strstr(buf, "transport stopped") == NULL) {
        fail("expected stopped warning for sync=clock");
    }

    g_clock_status = MOVE_CLOCK_STATUS_RUNNING;
    memset(buf, 0, sizeof(buf));
    len = api->get_param(inst, "error", buf, sizeof(buf));
    if (len != 0 || buf[0] != '\0') {
        fail("expected no warning when clock is running");
    }

    api->destroy_instance(inst);
    printf("PASS: arp clock status warnings\n");
    return 0;
}
