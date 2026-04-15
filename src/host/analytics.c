/*
 * Anonymous PostHog Analytics
 *
 * Fire-and-forget HTTP POSTs to PostHog. No SDK, no retries.
 * Disabled by default — only active when analytics-opt-in file exists.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <sched.h>
#include "analytics.h"

#define POSTHOG_API_KEY "phc_xkBkpTgLbY9JrNEMCThDLwjnasG9EKGznY3B8myFNQj5"
#define POSTHOG_ENDPOINT "https://us.i.posthog.com/capture/"
#define ANONYMOUS_ID_PATH "/data/UserData/schwung/anonymous-id"
#define OPT_IN_PATH "/data/UserData/schwung/analytics-opt-in"
#define CURL_PATH "/data/UserData/schwung/bin/curl"

static char g_anonymous_id[64] = "";
static char g_version[32] = "";

/* Generate a random UUID v4 string */
static void generate_uuid_v4(char *buf, size_t len) {
    unsigned char bytes[16];

    /* Read from /dev/urandom */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        /* Fallback to time-based seed */
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (int i = 0; i < 16; i++)
            bytes[i] = rand() & 0xFF;
    } else {
        read(fd, bytes, 16);
        close(fd);
    }

    /* Set version 4 and variant bits */
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    snprintf(buf, len,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
}

/* Load or create anonymous ID */
static void load_or_create_id(void) {
    FILE *f = fopen(ANONYMOUS_ID_PATH, "r");
    if (f) {
        if (fgets(g_anonymous_id, sizeof(g_anonymous_id), f)) {
            /* Trim newline */
            char *nl = strchr(g_anonymous_id, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
        if (g_anonymous_id[0]) return;
    }

    /* Generate new UUID */
    generate_uuid_v4(g_anonymous_id, sizeof(g_anonymous_id));

    /* Write to file */
    f = fopen(ANONYMOUS_ID_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", g_anonymous_id);
        fclose(f);
    }
    printf("analytics: generated anonymous id %s\n", g_anonymous_id);
}

void analytics_init(const char *version) {
    if (version) {
        strncpy(g_version, version, sizeof(g_version) - 1);
        g_version[sizeof(g_version) - 1] = '\0';
    }
    load_or_create_id();
    printf("analytics: initialized (enabled=%d, id=%.8s...)\n",
           analytics_enabled(), g_anonymous_id);
}

int analytics_enabled(void) {
    struct stat st;
    return (stat(OPT_IN_PATH, &st) == 0);
}

void analytics_set_enabled(int enabled) {
    if (enabled) {
        FILE *f = fopen(OPT_IN_PATH, "w");
        if (f) {
            fprintf(f, "1\n");
            fclose(f);
        }
    } else {
        unlink(OPT_IN_PATH);
    }
}

void analytics_track(const char *event, const char *properties_json) {
    if (!analytics_enabled()) return;
    if (!g_anonymous_id[0]) return;

    /* Build JSON payload */
    char payload[1024];
    if (properties_json && properties_json[0]) {
        snprintf(payload, sizeof(payload),
            "{\"api_key\":\"%s\",\"event\":\"%s\",\"distinct_id\":\"%s\","
            "\"properties\":{\"ip\":null,\"version\":\"%s\",%s}}",
            POSTHOG_API_KEY, event, g_anonymous_id, g_version, properties_json);
    } else {
        snprintf(payload, sizeof(payload),
            "{\"api_key\":\"%s\",\"event\":\"%s\",\"distinct_id\":\"%s\","
            "\"properties\":{\"ip\":null,\"version\":\"%s\"}}",
            POSTHOG_API_KEY, event, g_anonymous_id, g_version);
    }

    /* Fire-and-forget: fork, child execs curl, parent does NOT wait */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child process */
        /* Reset scheduling to SCHED_OTHER (we may inherit FIFO from parent) */
        struct sched_param sp = { .sched_priority = 0 };
        sched_setscheduler(0, SCHED_OTHER, &sp);

        /* Detach from parent */
        setsid();

        /* Close inherited fds */
        for (int fd = 3; fd < 256; fd++) close(fd);

        /* Redirect stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        const char *argv[] = {
            CURL_PATH, "-fsSLk",
            "--connect-timeout", "5",
            "--max-time", "10",
            "-X", "POST",
            "-H", "Content-Type: application/json",
            "-d", payload,
            POSTHOG_ENDPOINT,
            NULL
        };
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    /* Parent: do NOT waitpid — fire and forget */
}
