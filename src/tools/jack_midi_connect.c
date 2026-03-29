/*
 * jack_midi_connect — Connect system:midi_capture_ext to all RNBO patcher MIDI inputs.
 *
 * Uses dlopen(libjack) so we don't need libjack at link time (cross-compile friendly).
 * Only touches midi_capture_ext — never system:midi_capture (cable 0).
 * Uses a cache file to skip reconnection when ports haven't changed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>

/* JACK types/constants (avoid needing jack headers at build time) */
typedef void jack_client_t;
typedef int jack_status_t;
typedef unsigned long jack_port_id_t;
enum { JackNoStartServer = 0x01 };
enum { JackPortIsInput = 0x01 };
#define JACK_DEFAULT_MIDI_TYPE "8 bit raw midi"

/* Function pointers loaded at runtime */
static jack_client_t *(*fn_client_open)(const char *, int, jack_status_t *, ...);
static const char **(*fn_get_ports)(jack_client_t *, const char *, const char *, unsigned long);
static int (*fn_connect)(jack_client_t *, const char *, const char *);
static int (*fn_client_close)(jack_client_t *);
static void (*fn_free)(void *);

#define CACHE_FILE "/tmp/rnbo_midi_ports_cache"
#define SOURCE_PORT "system:midi_capture_ext"

static int read_cache(char *buf, size_t bufsz) {
    FILE *f = fopen(CACHE_FILE, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, bufsz - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 0;
}

static void write_cache(const char *data) {
    FILE *f = fopen(CACHE_FILE, "w");
    if (!f) return;
    fputs(data, f);
    fclose(f);
}

int main(void) {
    /* Load libjack at runtime */
    void *lib = dlopen("libjack.so.0", RTLD_NOW);
    if (!lib) lib = dlopen("libjack.so", RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "jack_midi_connect: cannot load libjack: %s\n", dlerror());
        return 1;
    }

    fn_client_open = dlsym(lib, "jack_client_open");
    fn_get_ports   = dlsym(lib, "jack_get_ports");
    fn_connect     = dlsym(lib, "jack_connect");
    fn_client_close = dlsym(lib, "jack_client_close");
    fn_free        = dlsym(lib, "jack_free");

    if (!fn_client_open || !fn_get_ports || !fn_connect || !fn_client_close) {
        fprintf(stderr, "jack_midi_connect: missing JACK symbols\n");
        dlclose(lib);
        return 1;
    }

    jack_status_t status;
    jack_client_t *client = fn_client_open("midi_connect", JackNoStartServer, &status);
    if (!client) {
        dlclose(lib);
        return 1;
    }

    /* Find all MIDI input ports */
    const char **ports = fn_get_ports(client, NULL, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
    if (!ports) {
        fn_client_close(client);
        dlclose(lib);
        return 0;
    }

    /* Build list of patcher MIDI input ports for cache comparison */
    char portlist[4096] = {0};
    int count = 0;

    for (int i = 0; ports[i]; i++) {
        /* Skip system ports — never touch cable 0 (system:midi_capture) */
        if (strncmp(ports[i], "system:", 7) == 0) continue;

        size_t len = strlen(portlist);
        snprintf(portlist + len, sizeof(portlist) - len, "%s\n", ports[i]);
        count++;
    }

    if (count == 0) {
        if (fn_free) fn_free(ports);
        fn_client_close(client);
        dlclose(lib);
        return 0;
    }

    /* Check cache — skip if ports haven't changed */
    char cached[4096] = {0};
    if (read_cache(cached, sizeof(cached)) == 0 && strcmp(cached, portlist) == 0) {
        if (fn_free) fn_free(ports);
        fn_client_close(client);
        dlclose(lib);
        return 0;
    }

    /* Ports changed — connect each one to midi_capture_ext */
    for (int i = 0; ports[i]; i++) {
        if (strncmp(ports[i], "system:", 7) == 0) continue;

        int err = fn_connect(client, SOURCE_PORT, ports[i]);
        if (err == 0) {
            printf("Connected %s -> %s\n", SOURCE_PORT, ports[i]);
        }
        /* err == EEXIST means already connected — silently skip */
    }

    write_cache(portlist);

    if (fn_free) fn_free(ports);
    fn_client_close(client);
    dlclose(lib);
    return 0;
}
