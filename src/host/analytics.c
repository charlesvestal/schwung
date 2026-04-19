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
#define OPT_OUT_PATH "/data/UserData/schwung/analytics-opt-out"
#define SNAPSHOT_PATH "/data/UserData/schwung/module-snapshot.txt"
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
    int has_opt_out = (stat(OPT_OUT_PATH, &st) == 0);
    int has_opt_in = (stat(OPT_IN_PATH, &st) == 0);

    /* Conflicting state: clean up everything and let the prompt re-ask */
    if (has_opt_out && has_opt_in) {
        unlink(OPT_OUT_PATH);
        unlink(OPT_IN_PATH);
        unlink("/data/UserData/schwung/analytics-prompted");
        return 0;  /* Disabled until re-prompted */
    }

    if (has_opt_out) return 0;

    /* Not yet prompted: don't send anything until user decides */
    struct stat prompted_st;
    if (stat("/data/UserData/schwung/analytics-prompted", &prompted_st) != 0) return 0;

    return 1;
}

void analytics_set_enabled(int enabled) {
    if (enabled) {
        unlink(OPT_OUT_PATH);
        /* Also create legacy opt-in file for backwards compat */
        FILE *f = fopen(OPT_IN_PATH, "w");
        if (f) { fprintf(f, "1\n"); fclose(f); }
    } else {
        FILE *f = fopen(OPT_OUT_PATH, "w");
        if (f) { fprintf(f, "1\n"); fclose(f); }
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

/* Max modules we track in the snapshot */
#define MAX_SNAPSHOT_MODULES 64

typedef struct {
    char id[64];
    char version[32];
} snapshot_entry_t;

/* Load previous module snapshot from disk */
static int load_snapshot(snapshot_entry_t *entries, int max_entries) {
    FILE *f = fopen(SNAPSHOT_PATH, "r");
    if (!f) return 0;

    int count = 0;
    char line[128];
    while (fgets(line, sizeof(line), f) && count < max_entries) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        strncpy(entries[count].id, line, sizeof(entries[count].id) - 1);
        entries[count].id[sizeof(entries[count].id) - 1] = '\0';
        strncpy(entries[count].version, eq + 1, sizeof(entries[count].version) - 1);
        entries[count].version[sizeof(entries[count].version) - 1] = '\0';
        count++;
    }
    fclose(f);
    return count;
}

/* Save current module snapshot to disk */
static void save_snapshot(const char (*ids)[64], const char (*versions)[32], int count) {
    FILE *f = fopen(SNAPSHOT_PATH, "w");
    if (!f) return;
    for (int i = 0; i < count; i++) {
        fprintf(f, "%s=%s\n", ids[i], versions[i]);
    }
    fclose(f);
}

/* Find an entry by ID in a snapshot */
static const snapshot_entry_t* find_in_snapshot(const snapshot_entry_t *entries, int count, const char *id) {
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].id, id) == 0) return &entries[i];
    }
    return NULL;
}

void analytics_diff_modules(const char (*ids)[64], const char (*versions)[32], int count) {
    if (!analytics_enabled()) return;

    /* Load previous snapshot (skip diff on first run — no snapshot yet) */
    snapshot_entry_t old_entries[MAX_SNAPSHOT_MODULES];
    int old_count = load_snapshot(old_entries, MAX_SNAPSHOT_MODULES);

    if (old_count > 0) {
        /* Compare current vs old — only emit upgrades. module_added was
         * removed as noisy: it fires for sideloads AND first-boot floods
         * and couldn't reliably distinguish the two. Install base is
         * derivable from module_census; Store-flow installs still emit
         * module_installed separately. */
        for (int i = 0; i < count; i++) {
            const snapshot_entry_t *old = find_in_snapshot(old_entries, old_count, ids[i]);
            if (old && strcmp(old->version, versions[i]) != 0) {
                /* Version changed */
                char props[256];
                snprintf(props, sizeof(props),
                    "\"module_id\":\"%s\",\"old_version\":\"%s\",\"new_version\":\"%s\"",
                    ids[i], old->version, versions[i]);
                analytics_track("module_upgraded", props);
            }
        }
    }

    /* Save new snapshot */
    save_snapshot(ids, versions, count);
}
