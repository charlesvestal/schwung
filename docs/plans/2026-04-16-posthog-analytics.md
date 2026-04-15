# Anonymous PostHog Analytics Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add anonymous, fire-and-forget PostHog analytics to Schwung using raw HTTP POSTs (no SDK). Disabled by default via a settings toggle.

**Architecture:** New C module (`src/host/analytics.c/.h`) handles UUID generation, opt-in check, and async curl POSTs. A JS host binding `host_track_event(name, props_json)` exposes tracking to JS for the `module_installed` event. Settings toggle in `menu_settings.mjs` manages the opt-in file.

**Tech Stack:** C (fork/exec curl), QuickJS JS bindings, PostHog HTTP capture API

---

### Task 1: Create analytics C module header

**Files:**
- Create: `src/host/analytics.h`

**Step 1: Write the header**

```c
/*
 * Anonymous PostHog Analytics - fire-and-forget HTTP POSTs
 *
 * Disabled by default. Enabled when /data/UserData/schwung/analytics-opt-in exists.
 * Anonymous UUID stored in /data/UserData/schwung/anonymous-id.
 */

#ifndef ANALYTICS_H
#define ANALYTICS_H

/* Initialize analytics: load or generate anonymous UUID */
void analytics_init(const char *version);

/* Track an event (fire-and-forget, no-op if disabled) */
void analytics_track(const char *event, const char *properties_json);

/* Check if analytics is enabled */
int analytics_enabled(void);

/* Enable/disable analytics (creates/removes opt-in file) */
void analytics_set_enabled(int enabled);

#endif /* ANALYTICS_H */
```

**Step 2: Commit**

```bash
git add src/host/analytics.h
git commit -m "feat: add analytics header for anonymous PostHog tracking"
```

---

### Task 2: Create analytics C module implementation

**Files:**
- Create: `src/host/analytics.c`

**Step 1: Write the implementation**

```c
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
```

**Step 2: Commit**

```bash
git add src/host/analytics.c
git commit -m "feat: implement anonymous PostHog analytics with fire-and-forget curl"
```

---

### Task 3: Wire analytics into schwung_host.c

**Files:**
- Modify: `src/schwung_host.c`

**Step 1: Add include and version loading**

Near the top includes (around line 30), add:
```c
#include "host/analytics.h"
```

**Step 2: Add JS host binding for tracking events**

After `js_host_save_settings` (around line 1786), add:

```c
/* host_track_event(event_name, properties_json) -> void */
static JSValue js_host_track_event(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;

    const char *event = JS_ToCString(ctx, argv[0]);
    if (!event) return JS_UNDEFINED;

    const char *props = NULL;
    if (argc >= 2) {
        props = JS_ToCString(ctx, argv[1]);
    }

    analytics_track(event, props);

    if (props) JS_FreeCString(ctx, props);
    JS_FreeCString(ctx, event);
    return JS_UNDEFINED;
}
```

**Step 3: Add analytics_enabled getter/setter for JS settings**

Add to `js_host_get_setting` (around line 1714), before the final `JS_FreeCString`:
```c
    } else if (strcmp(key, "analytics_enabled") == 0) {
        result = JS_NewInt32(ctx, analytics_enabled());
    }
```

Add to `js_host_set_setting` (around line 1775), before the final `JS_FreeCString`:
```c
    } else if (strcmp(key, "analytics_enabled") == 0) {
        int val;
        if (!JS_ToInt32(ctx, &val, argv[1])) {
            analytics_set_enabled(val ? 1 : 0);
        }
    }
```

**Step 4: Register JS function**

Find where other host functions are registered (search for `"host_save_settings"` in the JS global function registration block). Add nearby:
```c
    JS_SetPropertyStr(ctx, global, "host_track_event",
        JS_NewCFunction(ctx, js_host_track_event, "host_track_event", 2));
```

**Step 5: Initialize analytics and fire app_launched in main()**

After `settings_load(&g_settings, SETTINGS_PATH);` (line 2668), add:

```c
    /* Initialize analytics */
    {
        char version[32] = "unknown";
        FILE *vf = fopen(BASE_DIR "/host/version.txt", "r");
        if (vf) {
            if (fgets(version, sizeof(version), vf)) {
                char *nl = strchr(version, '\n');
                if (nl) *nl = '\0';
            }
            fclose(vf);
        }
        analytics_init(version);
        analytics_track("app_launched", NULL);
    }
```

**Step 6: Fire module_loaded event**

In `js_host_load_module()` (around line 1405), after the successful load block, before `return result == 0 ? JS_TRUE : JS_FALSE;` (line 1435), add:

```c
    if (result == 0) {
        const module_info_t *loaded = mm_get_current_module(&g_module_manager);
        if (loaded) {
            char props[256];
            snprintf(props, sizeof(props), "\"module_id\":\"%s\"", loaded->id);
            analytics_track("module_loaded", props);
        }
    }
```

**Step 7: Commit**

```bash
git add src/schwung_host.c
git commit -m "feat: wire analytics into host startup, module loading, and JS bindings"
```

---

### Task 4: Add analytics to build system

**Files:**
- Modify: `Makefile` or `scripts/build.sh` (whichever compiles C sources)

**Step 1: Find and update the build configuration**

Search for where `.c` files are compiled. Add `src/host/analytics.c` to the list of source files. The object `analytics.o` needs to be linked into the final `schwung` binary.

**Step 2: Commit**

```bash
git add scripts/build.sh  # or Makefile
git commit -m "build: add analytics.c to compilation"
```

---

### Task 5: Fire module_installed event from JS

**Files:**
- Modify: `src/shared/store_utils.mjs`

**Step 1: Add tracking call after successful install**

In `installModule()` (line 365), just before `return { success: true, error: null };`, add:

```javascript
    /* Track module installation */
    if (globalThis.host_track_event) {
        const props = `"module_id":"${mod.id}","module_version":"${mod.latest_version || 'unknown'}"`;
        globalThis.host_track_event('module_installed', props);
    }
```

**Step 2: Commit**

```bash
git add src/shared/store_utils.mjs
git commit -m "feat: track module_installed event via PostHog"
```

---

### Task 6: Add Analytics toggle to Settings menu

**Files:**
- Modify: `src/host/menu_settings.mjs`

**Step 1: Add Analytics toggle to settings items**

In `getSettingsItems()` (line 80), add before the About submenu entry (line 138):

```javascript
        createToggle('Analytics', {
            get: () => !!(host_get_setting('analytics_enabled')),
            set: (v) => {
                host_set_setting('analytics_enabled', v ? 1 : 0);
            }
        }),
```

Note: No `host_save_settings()` needed — analytics uses its own opt-in file, not settings.txt.

**Step 2: Commit**

```bash
git add src/host/menu_settings.mjs
git commit -m "feat: add Analytics toggle to Settings menu (default off)"
```

---

### Task 7: Build and test

**Step 1: Build**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung
./scripts/build.sh
```

Expected: Clean build with analytics.c compiled and linked.

**Step 2: Deploy and test on device**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Verify analytics is off by default**

```bash
ssh ableton@move.local "ls /data/UserData/schwung/analytics-opt-in"
# Expected: No such file (analytics off by default)
```

**Step 4: Verify anonymous ID was created**

```bash
ssh ableton@move.local "cat /data/UserData/schwung/anonymous-id"
# Expected: UUID v4 string
```

**Step 5: Enable analytics in Settings, verify file**

Navigate to Settings > Analytics > toggle On.

```bash
ssh ableton@move.local "cat /data/UserData/schwung/analytics-opt-in"
# Expected: file exists with "1"
```

**Step 6: Load a module and check PostHog**

Load any module from the menu. Check PostHog dashboard for `app_launched` and `module_loaded` events.

**Step 7: Commit any fixes**

---

### Task 8: Install a module from Store and verify module_installed event

**Step 1:** With analytics enabled, install any module from the Module Store.

**Step 2:** Check PostHog dashboard for `module_installed` event with correct `module_id` and `module_version` properties.

---

## Implementation Notes

- **PostHog API key:** `phc_xkBkpTgLbY9JrNEMCThDLwjnasG9EKGznY3B8myFNQj5` (public capture key, safe to commit)
- **Endpoint:** `https://us.i.posthog.com/capture/`
- **IP suppression:** `"ip": null` in properties tells PostHog not to store sender IP
- **Fire-and-forget:** Child process does `setsid()` + exec curl, parent never calls `waitpid`
- **SCHED_OTHER reset:** Child resets from any inherited FIFO scheduling before exec (per realtime safety rules)
- **Default off:** No `analytics-opt-in` file exists on fresh install, so no events fire until user enables in Settings
- **No dependencies:** Pure curl HTTP POST, no SDK or library additions
