# Safe Native Resample Bridge Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make the native resample bridge safe by only activating it when Move's sample source is "output" (resampling), with inotify for instant detection, polling fallback, feedback detection safety net, and raw_audio_in for audio-input plugins.

**Architecture:** Three layers of protection. Layer 1: inotify watch on Move's Settings.json detects source changes instantly, with 1s polling fallback; bridge only applies when source is "output". Layer 2: raw_audio_in gives audio-input plugins (linein, etc.) pre-bridge hardware audio so they can never self-feed. Layer 3: per-frame feedback energy detector auto-kills bridge if feedback is detected. The old off/mix/overwrite mode is replaced by a simple enabled boolean in shadow_config.json.

**Tech Stack:** C (inotify API, shared memory), JavaScript (Shadow UI settings)

---

### Task 1: Simplify bridge mode to boolean

Replace the three-mode enum with a simple enabled/disabled boolean.

**Files:**
- Modify: `src/host/shadow_resample.h`
- Modify: `src/host/shadow_resample.c`
- Modify: `src/move_anything_shim.c`
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Update the header**

In `src/host/shadow_resample.h`, replace the mode enum and related declarations:

```c
// REMOVE:
typedef enum {
    NATIVE_RESAMPLE_BRIDGE_OFF = 0,
    NATIVE_RESAMPLE_BRIDGE_MIX,
    NATIVE_RESAMPLE_BRIDGE_OVERWRITE
} native_resample_bridge_mode_t;

// REPLACE WITH:
/* Bridge is simply enabled or disabled by the user.
 * Actual application is gated on sampleRecordingSource == "output". */
```

Replace the extern:
```c
// REMOVE:
extern volatile native_resample_bridge_mode_t native_resample_bridge_mode;

// REPLACE WITH:
extern volatile int resample_bridge_enabled;
```

Remove these function declarations:
```c
const char *native_resample_bridge_mode_name(native_resample_bridge_mode_t mode);
native_resample_bridge_mode_t native_resample_bridge_mode_from_text(const char *text);
int native_resample_bridge_source_allows_apply(native_resample_bridge_mode_t mode);
```

Add new declarations:
```c
/* Source tracking from Settings.json (inotify + poll) */
void resample_source_init_watcher(void);
void resample_source_check(void);  /* Call from D-Bus loop iteration */

/* Is bridge currently allowed to apply? (enabled AND source is output) */
int resample_bridge_should_apply(void);
```

**Step 2: Update shadow_resample.c**

Replace the mode global:
```c
// REMOVE:
volatile native_resample_bridge_mode_t native_resample_bridge_mode = NATIVE_RESAMPLE_BRIDGE_OFF;

// REPLACE WITH:
volatile int resample_bridge_enabled = 0;
static volatile int resample_source_is_output = 0;
```

Replace `native_resample_bridge_mode_name()` and `native_resample_bridge_mode_from_text()` with:
```c
/* No mode helpers needed — just a bool now */
```

Update `native_resample_bridge_load_mode_from_shadow_config()` to read `"resample_bridge_enabled"` as a boolean instead of `"resample_bridge_mode"` as a string. Also add backward-compat: if `"resample_bridge_mode"` exists with value "2" or "overwrite", treat as enabled=true.

```c
void native_resample_bridge_load_mode_from_shadow_config(void)
{
    const char *config_path = "/data/UserData/move-anything/shadow_config.json";
    FILE *f = fopen(config_path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8192) { fclose(f); return; }

    char *json = malloc((size_t)size + 1);
    if (!json) { fclose(f); return; }

    size_t nread = fread(json, 1, (size_t)size, f);
    fclose(f);
    json[nread] = '\0';

    /* New key: resample_bridge_enabled (bool) */
    char *enabled_key = strstr(json, "\"resample_bridge_enabled\"");
    if (enabled_key) {
        char *colon = strchr(enabled_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            resample_bridge_enabled = (strncmp(colon, "true", 4) == 0 || *colon == '1') ? 1 : 0;
        }
    } else {
        /* Backward compat: old resample_bridge_mode */
        char *mode_key = strstr(json, "\"resample_bridge_mode\"");
        if (mode_key) {
            char *colon = strchr(mode_key, ':');
            if (colon) {
                colon++;
                while (*colon == ' ' || *colon == '\t' || *colon == '"') colon++;
                /* "2", "overwrite", or "1", "mix" all mean enabled */
                resample_bridge_enabled = (*colon != '0' && strncmp(colon, "off", 3) != 0) ? 1 : 0;
            }
        }
    }

    if (host.log) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Resample bridge: %s (from config)",
                 resample_bridge_enabled ? "enabled" : "disabled");
        host.log(msg);
    }

    /* ... keep existing link_audio_routing / link_audio_publish parsing ... */
    free(json);
}
```

Replace `native_resample_bridge_source_allows_apply()` with:
```c
int resample_bridge_should_apply(void)
{
    return resample_bridge_enabled && resample_source_is_output;
}
```

Simplify `native_resample_bridge_apply()` — remove mode branching, always use the overwrite makeup path (the old "overwrite" behavior, which is what we want when bridging):
```c
void native_resample_bridge_apply(void)
{
    unsigned char *mmap_addr = host.global_mmap_addr ? *host.global_mmap_addr : NULL;
    if (!mmap_addr || !native_total_mix_snapshot_valid) return;

    if (!resample_bridge_should_apply()) {
        /* Diag logging (throttled) */
        static int skip_counter = 0;
        if (native_resample_diag_is_enabled() && skip_counter++ % 200 == 0) {
            if (host.log) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Bridge skip: enabled=%d src_output=%d",
                         (int)resample_bridge_enabled, (int)resample_source_is_output);
                host.log(msg);
            }
        }
        return;
    }

    int16_t *dst = (int16_t *)(mmap_addr + RESAMPLE_AUDIO_IN_OFFSET);
    int16_t compensated_snapshot[FRAMES_PER_BLOCK * 2];
    native_resample_bridge_apply_overwrite_makeup(
        native_total_mix_snapshot, compensated_snapshot, FRAMES_PER_BLOCK * 2);
    memcpy(dst, compensated_snapshot, RESAMPLE_AUDIO_BUFFER_SIZE);

    /* Diag logging (throttled) */
    static int apply_counter = 0;
    if (native_resample_diag_is_enabled() && apply_counter++ % 200 == 0) {
        native_audio_metrics_t src_m, dst_m;
        native_compute_audio_metrics(native_total_mix_snapshot, &src_m);
        native_compute_audio_metrics(dst, &dst_m);
        if (host.log) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Bridge apply: src_rms=(%.4f,%.4f) dst_rms=(%.4f,%.4f) mv=%.3f",
                     src_m.rms_l, src_m.rms_r, dst_m.rms_l, dst_m.rms_r,
                     (double)(host.shadow_master_volume ? *host.shadow_master_volume : 0.0f));
            host.log(msg);
        }
    }
}
```

Remove `native_resample_diag_log_skip()` and `native_resample_diag_log_apply()` (inlined above) and make `native_resample_diag_is_enabled()` non-static so it can be used in the simplified apply.

**Step 3: Update shim param handling**

In `src/move_anything_shim.c`, find the `master_fx:resample_bridge` param handler (~line 2487-2505) and simplify:

```c
if (strcmp(fx_key, "resample_bridge") == 0) {
    if (is_set) {
        int val = atoi(shadow_param->value);
        if (val != resample_bridge_enabled) {
            resample_bridge_enabled = val ? 1 : 0;
            shadow_log(resample_bridge_enabled
                ? "Resample bridge: enabled" : "Resample bridge: disabled");
        }
        shadow_param->error = 0;
        shadow_param->result_len = 0;
    } else {
        int len = snprintf(shadow_param->value, sizeof(shadow_param->value),
                          "%d", (int)resample_bridge_enabled);
        shadow_param->result_len = len;
        shadow_param->error = 0;
    }
    shadow_param->ready = 1;
    return;
}
```

**Step 4: Update Shadow UI**

In `src/shadow/shadow_ui.js`, change the settings entry (~line 712):

```javascript
// REPLACE:
{ key: "resample_bridge", label: "Sample Src", type: "enum",
  options: ["Native", "ME Mix"], values: [0, 2] },

// WITH:
{ key: "resample_bridge", label: "Resample Bridge", type: "bool" },
```

Update `saveConfig` (~line 4470) to save `resample_bridge_enabled` instead of `resample_bridge_mode`.

Update `applyConfig` (~line 4650-4652) to set the boolean param.

**Step 5: Build and test**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Verify: Settings menu shows "Resample Bridge: On/Off". Toggling it works. Bridge only applies when enabled (check diag log with `touch /data/UserData/move-anything/native_resample_diag_on`).

**Step 6: Commit**

```bash
git add src/host/shadow_resample.h src/host/shadow_resample.c src/move_anything_shim.c src/shadow/shadow_ui.js
git commit -m "refactor: simplify resample bridge to enabled boolean

Replace off/mix/overwrite mode with simple enabled/disabled toggle.
Always use overwrite-makeup path when active. Backward-compatible
with old shadow_config.json resample_bridge_mode values."
```

---

### Task 2: Add inotify + poll source tracking from Settings.json

Watch Move's Settings.json for `sampleRecordingSource` changes. inotify for instant detection, 1s poll as fallback.

**Files:**
- Modify: `src/host/shadow_resample.h`
- Modify: `src/host/shadow_resample.c`
- Modify: `src/host/shadow_dbus.c`

**Step 1: Add inotify and poll implementation to shadow_resample.c**

Add includes at top:
```c
#include <sys/inotify.h>
#include <fcntl.h>
#include <time.h>
```

Add static state:
```c
/* Settings.json inotify watcher */
static int settings_inotify_fd = -1;
static int settings_watch_fd = -1;
static time_t settings_last_poll = 0;
#define SETTINGS_POLL_INTERVAL_SEC 1
#define MOVE_SETTINGS_PATH "/data/UserData/settings/Settings.json"
```

Add the parse function:
```c
static void resample_parse_settings_json(void)
{
    FILE *f = fopen(MOVE_SETTINGS_PATH, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) { fclose(f); return; }

    char *json = malloc((size_t)size + 1);
    if (!json) { fclose(f); return; }

    size_t nread = fread(json, 1, (size_t)size, f);
    fclose(f);
    json[nread] = '\0';

    /* Find "sampleRecordingSource": "output" / "input" / "usb" */
    char *key = strstr(json, "\"sampleRecordingSource\"");
    int new_is_output = 0;
    if (key) {
        char *colon = strchr(key, ':');
        if (colon) {
            char *quote1 = strchr(colon, '"');
            if (quote1) {
                quote1++;
                char *quote2 = strchr(quote1, '"');
                if (quote2) {
                    size_t vlen = (size_t)(quote2 - quote1);
                    new_is_output = (vlen == 6 && strncmp(quote1, "output", 6) == 0);
                }
            }
        }
    }

    if (new_is_output != resample_source_is_output) {
        resample_source_is_output = new_is_output;
        if (host.log) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Resample source: %s (from Settings.json)",
                     new_is_output ? "output (bridge allowed)" : "not output (bridge blocked)");
            host.log(msg);
        }
    }

    free(json);
}
```

Add init and check functions:
```c
void resample_source_init_watcher(void)
{
    /* Read initial state */
    resample_parse_settings_json();
    settings_last_poll = time(NULL);

    /* Set up inotify */
    settings_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (settings_inotify_fd < 0) {
        if (host.log) host.log("Resample: inotify_init failed, using poll-only");
        return;
    }

    settings_watch_fd = inotify_add_watch(settings_inotify_fd,
        MOVE_SETTINGS_PATH, IN_CLOSE_WRITE | IN_MODIFY);
    if (settings_watch_fd < 0) {
        if (host.log) host.log("Resample: inotify_add_watch failed, using poll-only");
        close(settings_inotify_fd);
        settings_inotify_fd = -1;
        return;
    }

    if (host.log) host.log("Resample: inotify watcher active on Settings.json");
}

void resample_source_check(void)
{
    int did_inotify = 0;

    /* Check inotify (non-blocking) */
    if (settings_inotify_fd >= 0) {
        char buf[256];
        ssize_t len = read(settings_inotify_fd, buf, sizeof(buf));
        if (len > 0) {
            /* Got event(s) — re-read settings */
            resample_parse_settings_json();
            settings_last_poll = time(NULL);
            did_inotify = 1;
        }
    }

    /* Fallback poll every SETTINGS_POLL_INTERVAL_SEC */
    if (!did_inotify) {
        time_t now = time(NULL);
        if (now - settings_last_poll >= SETTINGS_POLL_INTERVAL_SEC) {
            resample_parse_settings_json();
            settings_last_poll = now;
        }
    }
}
```

**Step 2: Call from D-Bus thread**

In `src/host/shadow_dbus.c`, add to the D-Bus thread function, after the initial setup and before the main loop:

```c
/* Initialize Settings.json watcher for resample source */
resample_source_init_watcher();
```

In the main loop, add after `dbus_connection_read_write`:

```c
while (shadow_dbus_running) {
    dbus_connection_read_write(shadow_dbus_conn, 100);  /* 100ms timeout */

    while (dbus_connection_dispatch(shadow_dbus_conn) == DBUS_DISPATCH_DATA_REMAINS) {
    }

    /* Check Settings.json for sampleRecordingSource changes */
    resample_source_check();
}
```

**Step 3: Update header**

Add `#include <sys/inotify.h>` is NOT needed in the header — it's only in the .c file. Just ensure the two new function declarations are in the header (from Task 1's header changes).

**Step 4: Build and test**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Test: Enable the bridge in settings. Enable diag logging. Change Move's sample source between output/input/usb. Verify bridge apply/skip matches the source. Verify inotify log messages appear.

**Step 5: Commit**

```bash
git add src/host/shadow_resample.h src/host/shadow_resample.c src/host/shadow_dbus.c
git commit -m "feat: inotify + poll source tracking for resample bridge

Watch Settings.json for sampleRecordingSource changes via inotify
(instant) with 1s poll fallback. Bridge only applies when source is
'output' (resampling mode). Eliminates always-on bridge feedback risk."
```

---

### Task 3: Add raw_audio_in for audio-input plugins

Save pre-bridge hardware AUDIO_IN and expose to plugins so audio-input modules (linein, vocoder, etc.) never read bridge content.

**Files:**
- Modify: `src/host/plugin_api_v1.h`
- Modify: `src/move_anything_shim.c`
- Modify: `src/modules/sound_generators/linein/linein.c`

**Step 1: Add raw_audio_in to host API**

In `src/host/plugin_api_v1.h`, add after the `get_clock_status` field (before the modulation callbacks):

```c
    /* Raw hardware audio input (pre-bridge).
     * Points to a buffer with MOVE_FRAMES_PER_BLOCK stereo frames of raw
     * hardware AUDIO_IN, captured before the native resample bridge overwrites
     * the mailbox AUDIO_IN region.  Plugins that need the actual hardware input
     * (e.g. line-in) should read from this instead of mapped_memory+audio_in_offset
     * to avoid feeding back on bridge content.  May be NULL if not available. */
    int16_t *raw_audio_in;
```

**Step 2: Add buffer and capture in shim**

In `src/move_anything_shim.c`, add a static buffer near the other audio buffers:

```c
/* Raw hardware AUDIO_IN saved before bridge overwrites it.
 * Exposed to sub-plugins (e.g. linein) via host_api_v1_t.raw_audio_in
 * so they read actual hardware input instead of bridge content. */
static int16_t raw_audio_in_buffer[FRAMES_PER_BLOCK * 2];
```

In the ioctl handler, just after copying AUDIO_IN from hardware to shadow mailbox (the `memcpy` of AUDIO_IN region, around the block that copies hardware mmap to shadow mailbox), add:

```c
/* Save raw hardware AUDIO_IN before bridge overwrites it. */
memcpy(raw_audio_in_buffer,
       hardware_mmap_addr + AUDIO_IN_OFFSET,
       AUDIO_BUFFER_SIZE);
```

This must come BEFORE the `native_resample_bridge_apply()` call.

Wire up the pointer in all host API structs. Find where `shadow_host_api` is populated (around `shadow_inprocess_load_chain`) and add:
```c
shadow_host_api.raw_audio_in = raw_audio_in_buffer;
```

Similarly for `overtake_host_api` in `shadow_overtake_dsp_load`:
```c
overtake_host_api.raw_audio_in = raw_audio_in_buffer;
```

And in `module_manager.c` for the standalone host API:
```c
mm->host_api.raw_audio_in = raw_audio_in_buffer;
```
(Note: module_manager may need the buffer declared extern or passed differently — check if it has access. If not, add `extern int16_t raw_audio_in_buffer[];` or pass via the host_api struct init in the shim.)

**Step 3: Update linein to use raw_audio_in**

In `src/modules/sound_generators/linein/linein.c`, in `v2_render_block()` (~line 962):

```c
// REPLACE:
    int16_t *audio_in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);

// WITH:
    /* Use raw hardware audio input when available (pre-bridge) to avoid
     * feeding back on our own processed output via the native resample bridge. */
    int16_t *audio_in = g_host->raw_audio_in
        ? g_host->raw_audio_in
        : (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
```

**Step 4: Build and test**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Test: Load linein in a shadow slot. Enable bridge. Verify no self-feedback. Verify linein still passes audio correctly.

**Step 5: Commit**

```bash
git add src/host/plugin_api_v1.h src/move_anything_shim.c src/modules/sound_generators/linein/linein.c
git commit -m "feat: raw_audio_in pre-bridge buffer for audio-input plugins

Save hardware AUDIO_IN before bridge overwrites it. Expose via
host_api_v1_t.raw_audio_in. Linein reads raw hardware audio instead
of bridge content, eliminating direct self-feed path."
```

---

### Task 4: Add feedback detection safety net

Per-frame energy growth detector that auto-kills the bridge if feedback is detected.

**Files:**
- Modify: `src/host/shadow_resample.h`
- Modify: `src/host/shadow_resample.c`

**Step 1: Add feedback state to header**

In `src/host/shadow_resample.h`, add:

```c
/* Feedback detection state (read-only for UI) */
extern volatile int resample_bridge_feedback_killed;
```

**Step 2: Implement feedback detector in shadow_resample.c**

Add state:
```c
/* Feedback detection */
volatile int resample_bridge_feedback_killed = 0;
static int feedback_detect_count = 0;
static float feedback_prev_energy = 0.0f;
#define FEEDBACK_GROWTH_THRESHOLD 1.5f  /* energy grew by 50%+ frame-over-frame */
#define FEEDBACK_FRAMES_TO_KILL 3       /* ~8.7ms at 44.1kHz/128 frames */
```

Add detector function:
```c
/* Check for feedback: is AUDIO_IN energy growing rapidly?
 * Called BEFORE writing bridge content, on the current AUDIO_IN. */
static int resample_feedback_check(const int16_t *audio_in)
{
    if (!audio_in) return 0;

    /* Compute RMS energy of current AUDIO_IN */
    double sum = 0.0;
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        float s = (float)audio_in[i] / 32768.0f;
        sum += (double)s * (double)s;
    }
    float energy = sqrtf((float)(sum / (FRAMES_PER_BLOCK * 2)));

    /* Skip detection if energy is very low (silence / noise floor) */
    if (energy < 0.001f) {
        feedback_detect_count = 0;
        feedback_prev_energy = energy;
        return 0;
    }

    /* Check for sustained energy growth */
    if (feedback_prev_energy > 0.001f && energy > feedback_prev_energy * FEEDBACK_GROWTH_THRESHOLD) {
        feedback_detect_count++;
        if (feedback_detect_count >= FEEDBACK_FRAMES_TO_KILL) {
            /* Feedback detected — kill bridge */
            resample_bridge_feedback_killed = 1;
            resample_bridge_enabled = 0;
            if (host.log) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "FEEDBACK DETECTED: bridge auto-disabled (energy %.4f -> %.4f over %d frames)",
                         feedback_prev_energy, energy, feedback_detect_count);
                host.log(msg);
            }
            feedback_detect_count = 0;
            feedback_prev_energy = 0.0f;
            return 1;
        }
    } else {
        feedback_detect_count = 0;
    }

    feedback_prev_energy = energy;
    return 0;
}
```

Integrate into `native_resample_bridge_apply()`, right before writing to dst:

```c
void native_resample_bridge_apply(void)
{
    unsigned char *mmap_addr = host.global_mmap_addr ? *host.global_mmap_addr : NULL;
    if (!mmap_addr || !native_total_mix_snapshot_valid) return;

    if (!resample_bridge_should_apply()) {
        /* Reset feedback state when bridge is off */
        feedback_detect_count = 0;
        feedback_prev_energy = 0.0f;
        /* ... diag logging ... */
        return;
    }

    int16_t *dst = (int16_t *)(mmap_addr + RESAMPLE_AUDIO_IN_OFFSET);

    /* Feedback check on current AUDIO_IN before we overwrite it */
    if (resample_feedback_check(dst)) {
        return;  /* Bridge was killed */
    }

    int16_t compensated_snapshot[FRAMES_PER_BLOCK * 2];
    native_resample_bridge_apply_overwrite_makeup(
        native_total_mix_snapshot, compensated_snapshot, FRAMES_PER_BLOCK * 2);
    memcpy(dst, compensated_snapshot, RESAMPLE_AUDIO_BUFFER_SIZE);

    /* ... diag logging ... */
}
```

**Step 3: Allow re-enabling after feedback kill**

The `resample_bridge_feedback_killed` flag is exposed so the Shadow UI can show a warning. When the user re-enables the bridge via settings, clear the flag:

In the shim param handler for `master_fx:resample_bridge` (from Task 1), add:
```c
if (val) {
    resample_bridge_feedback_killed = 0;  /* Clear feedback kill on manual re-enable */
}
```

**Step 4: Build and test**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Test: This is hard to test safely. Enable diag logging. With bridge enabled and source = output, monitor the log for feedback detection messages. The detector should NOT fire during normal resampling use.

**Step 5: Commit**

```bash
git add src/host/shadow_resample.h src/host/shadow_resample.c src/move_anything_shim.c
git commit -m "feat: feedback detection safety net for resample bridge

Per-frame energy growth detector auto-disables bridge if AUDIO_IN
energy grows >50% over 3 consecutive frames (~9ms). Prevents
runaway feedback if source tracking has a gap. User must manually
re-enable after a feedback kill."
```

---

### Task 5: Clean up monitoring script and docs

**Files:**
- Remove: `scripts/monitor_sample_source.sh` (testing artifact)
- Modify: `MANUAL.md` (update resampling section)
- Modify: `CLAUDE.md` (update resample bridge docs if mentioned)

**Step 1: Remove test script**

```bash
rm scripts/monitor_sample_source.sh
```

**Step 2: Update docs**

Update MANUAL.md's resampling section to describe the new behavior:
- Bridge is now a simple on/off toggle ("Resample Bridge" in Shadow UI settings)
- Only active when Move's sample source is set to resampling ("output")
- Auto-disables if feedback is detected
- No more mix/overwrite modes

**Step 3: Commit**

```bash
git add -A
git commit -m "docs: update manual for simplified resample bridge"
```
