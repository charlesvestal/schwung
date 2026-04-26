# Long-Press Shadow Shortcuts Implementation Plan

> **Status (2026-04-26):** Superseded — long-press is now selectable via
> the `shadow_ui_trigger` enum in features.json (Long Press / Shift+Vol /
> Both). The old `long_press_shadow` bool is read for backward compat
> only. Don't follow the steps in this plan; they describe an obsolete
> shape of the feature.

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add optional long-press detection on Track/Menu/Step2 buttons as an alternative to Shift+Vol combos for entering shadow UI.

**Architecture:** The shim records press timestamps via `clock_gettime(CLOCK_MONOTONIC)` and checks elapsed time each SPI frame. A `long_press_shadow` flag in `shadow_control_t` is set by the shadow UI settings and read by the shim. LED hints light on Shift alone (not Shift+Vol) when enabled.

**Tech Stack:** C (shim), JavaScript (shadow UI settings), shared memory (shadow_control_t)

---

### Task 1: Add `long_press_shadow` field to shadow_control_t

**Files:**
- Modify: `src/host/shadow_constants.h:146` (reserved bytes)

**Step 1: Add the field**

In `shadow_control_t`, replace one reserved byte with the new field. Change:

```c
    volatile uint8_t open_tool_cmd;     /* 0=none, 1=open tool (path in /data/UserData/schwung/open_tool_cmd.json) */
    volatile uint8_t reserved[7];
```

to:

```c
    volatile uint8_t open_tool_cmd;     /* 0=none, 1=open tool (path in /data/UserData/schwung/open_tool_cmd.json) */
    volatile uint8_t long_press_shadow; /* 1=enable long-press Track/Menu/Step2 shortcuts */
    volatile uint8_t reserved[6];
```

**Step 2: Commit**

```bash
git add src/host/shadow_constants.h
git commit -m "feat: add long_press_shadow field to shadow_control_t"
```

---

### Task 2: Add long-press state tracking and detection in the shim

**Files:**
- Modify: `src/schwung_shim.c`

**Step 1: Add feature flag and state variables**

Near line 154 (after `static bool skipback_require_volume = false;`), add:

```c
static bool long_press_shadow_enabled = false; /* Long-press Track/Menu/Step2 shortcuts */
```

Near the other static state variables (around the `shadow_held_track` area), add:

```c
/* Long-press detection state */
#define LONG_PRESS_MS 750

static struct timespec track_press_time[4];  /* When each track button was pressed */
static uint8_t track_longpress_pending[4];   /* 1=pressed, waiting for threshold */
static uint8_t track_longpress_fired[4];     /* 1=already fired, don't fire again */

static struct timespec menu_press_time;
static uint8_t menu_longpress_pending;
static uint8_t menu_longpress_fired;

static struct timespec step2_press_time;
static uint8_t step2_longpress_pending;
static uint8_t step2_longpress_fired;

static inline int long_press_elapsed(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int ms = (int)((now.tv_sec - start->tv_sec) * 1000 +
                    (now.tv_nsec - start->tv_nsec) / 1000000);
    return ms >= LONG_PRESS_MS;
}
```

**Step 2: Parse feature flag from features.json**

In `load_feature_config()` (around line 690, after the `skipback_require_volume` parsing block), add:

```c
    /* Parse long_press_shadow (defaults to false) */
    const char *long_press_key = strstr(config_buf, "\"long_press_shadow\"");
    if (long_press_key) {
        const char *colon = strchr(long_press_key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '\t') colon++;
            if (strncmp(colon, "true", 4) == 0) {
                long_press_shadow_enabled = true;
            }
        }
    }
```

Also, at the end of `load_feature_config()`, sync the flag to shared memory:

```c
    if (shadow_control) {
        shadow_control->long_press_shadow = long_press_shadow_enabled ? 1 : 0;
    }
```

**Step 3: Record press timestamps on button events**

In the Track button handling section (around line 4791, inside the `if (d1 >= 40 && d1 <= 43)` block), after `shadow_update_held_track(d1, pressed);`, add:

```c
                    /* Long-press detection */
                    if (long_press_shadow_enabled && shadow_ui_enabled) {
                        int slot = 43 - d1;
                        if (pressed) {
                            clock_gettime(CLOCK_MONOTONIC, &track_press_time[slot]);
                            track_longpress_pending[slot] = 1;
                            track_longpress_fired[slot] = 0;
                        } else {
                            /* Released before threshold — if shadow UI displayed, dismiss it */
                            if (track_longpress_pending[slot] && !track_longpress_fired[slot] &&
                                shadow_display_mode && shadow_control) {
                                shadow_display_mode = 0;
                                shadow_control->display_mode = 0;
                                shadow_log("Track tap: dismissing shadow UI");
                            }
                            track_longpress_pending[slot] = 0;
                        }
                    }
```

For the Menu button (around line 4537 area where CC_MENU is handled, or add a new block near the Track handling), add detection for CC_MENU press/release:

```c
                /* Menu button long-press detection */
                if (d1 == CC_MENU && long_press_shadow_enabled && shadow_ui_enabled) {
                    if (d2 > 0) {
                        clock_gettime(CLOCK_MONOTONIC, &menu_press_time);
                        menu_longpress_pending = 1;
                        menu_longpress_fired = 0;
                    } else {
                        menu_longpress_pending = 0;
                    }
                }
```

For Step 2 (note 17), in the note handling section near line 5032, add detection that works alongside the existing Shift+Vol+Step2 handler:

```c
                /* Step 2 long-press detection (only with Shift, without Vol) */
                if (d1 == 17 && type == 0x90 && long_press_shadow_enabled && shadow_ui_enabled) {
                    if (d2 > 0 && shadow_shift_held && !shadow_volume_knob_touched) {
                        clock_gettime(CLOCK_MONOTONIC, &step2_press_time);
                        step2_longpress_pending = 1;
                        step2_longpress_fired = 0;
                    } else if (d2 == 0 || type == 0x80) {
                        step2_longpress_pending = 0;
                    }
                }
```

**Step 4: Check long-press thresholds in shim_pre_transfer**

In `shim_pre_transfer()`, in the shortcut indicator LEDs section (around line 4292), add long-press threshold checks. This runs every SPI frame (~344 Hz), which is fine for 750ms detection:

```c
    /* Long-press detection checks */
    if (long_press_shadow_enabled && shadow_ui_enabled && shadow_control) {
        /* Track buttons */
        for (int i = 0; i < 4; i++) {
            if (track_longpress_pending[i] && !track_longpress_fired[i] &&
                !shadow_shift_held && !shadow_volume_knob_touched &&
                long_press_elapsed(&track_press_time[i])) {
                track_longpress_fired[i] = 1;
                track_longpress_pending[i] = 0;
                shadow_control->ui_slot = (uint8_t)i;
                shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_SLOT;
                if (!shadow_display_mode) {
                    shadow_display_mode = 1;
                    shadow_control->display_mode = 1;
                    launch_shadow_ui();
                }
                shadow_log("Track long-press: opening slot settings");
            }
        }
        /* Menu button */
        if (menu_longpress_pending && !menu_longpress_fired &&
            !shadow_shift_held && !shadow_volume_knob_touched &&
            long_press_elapsed(&menu_press_time)) {
            menu_longpress_fired = 1;
            menu_longpress_pending = 0;
            shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_MASTER_FX;
            if (!shadow_display_mode) {
                shadow_display_mode = 1;
                shadow_control->display_mode = 1;
                launch_shadow_ui();
            }
            shadow_log("Menu long-press: opening master FX");
        }
        /* Shift + Step 2 */
        if (step2_longpress_pending && !step2_longpress_fired &&
            shadow_shift_held && !shadow_volume_knob_touched &&
            long_press_elapsed(&step2_press_time)) {
            step2_longpress_fired = 1;
            step2_longpress_pending = 0;
            shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_SETTINGS;
            shadow_display_mode = 1;
            shadow_control->display_mode = 1;
            launch_shadow_ui();
            shadow_log("Shift+Step2 long-press: opening global settings");
        }
    }
```

**Step 5: Cancel long-press if Shift or Vol is pressed mid-hold**

The threshold checks above already skip when `shadow_shift_held` or `shadow_volume_knob_touched` (for Track/Menu). This means if you press Shift while holding a Track button, the long-press won't fire, and the existing Shift+Track behavior takes over.

**Step 6: Commit**

```bash
git add src/schwung_shim.c
git commit -m "feat: add long-press detection for Track, Menu, and Shift+Step2"
```

---

### Task 3: Add Shift+Step13 immediate shortcut (no Vol required)

**Files:**
- Modify: `src/schwung_shim.c`

**Step 1: Add Shift+Step13 without Vol requirement**

In the Step 13 handler (around line 5048), the existing code checks `shadow_shift_held && shadow_volume_knob_touched`. Add a parallel path that fires when long-press mode is enabled and Shift is held without Vol:

```c
                /* Shift + Step 13 (note 28) = jump to Tools menu */
                if (d1 == 28 && type == 0x90 && d2 > 0) {
                    if (shadow_shift_held && shadow_volume_knob_touched && shadow_control && shadow_ui_enabled) {
                        /* Existing: Shift+Vol+Step13 — always works */
                        shadow_block_plain_volume_hide_until_release = 1;
                        shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_TOOLS;
                        shadow_display_mode = 1;
                        shadow_control->display_mode = 1;
                        launch_shadow_ui();
                        uint8_t *sh = shadow + MIDI_IN_OFFSET;
                        sh[j] = 0; sh[j+1] = 0; sh[j+2] = 0; sh[j+3] = 0;
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                    } else if (long_press_shadow_enabled && shadow_shift_held &&
                               !shadow_volume_knob_touched && shadow_control && shadow_ui_enabled) {
                        /* New: Shift+Step13 without Vol — immediate, no long-press needed */
                        shadow_control->ui_flags |= SHADOW_UI_FLAG_JUMP_TO_TOOLS;
                        shadow_display_mode = 1;
                        shadow_control->display_mode = 1;
                        launch_shadow_ui();
                        uint8_t *sh = shadow + MIDI_IN_OFFSET;
                        sh[j] = 0; sh[j+1] = 0; sh[j+2] = 0; sh[j+3] = 0;
                        src[j] = 0; src[j+1] = 0; src[j+2] = 0; src[j+3] = 0;
                        shadow_log("Shift+Step13: opening tools");
                    }
                }
```

**Step 2: Commit**

```bash
git add src/schwung_shim.c
git commit -m "feat: add Shift+Step13 immediate tools shortcut (no Vol required)"
```

---

### Task 4: Update LED hints to light on Shift alone

**Files:**
- Modify: `src/schwung_shim.c`

**Step 1: Update shortcut indicator LED logic**

In `shim_pre_transfer()` around line 4292, update the LED hint section. Currently it only lights Step 13 on Shift+Vol. Change to also light Step 2 and Step 13 on Shift alone when long-press is enabled:

```c
    /* === SHORTCUT INDICATOR LEDS ===
     * When Shift+Vol held, light step icon LEDs (CCs 16-31 = icons below steps).
     * When long-press enabled, also light on Shift alone (Step 2 + Step 13).
     * Uses shadow_queue_led which gets flushed by shadow_flush_pending_leds above. */
    {
        static int shortcut_leds_on = 0;       /* Shift+Vol LEDs */
        static int longpress_leds_on = 0;      /* Shift-only LEDs (long-press mode) */

        int want_shiftvol = shadow_shift_held && shadow_volume_knob_touched;
        int want_longpress = long_press_shadow_enabled && shadow_shift_held && !shadow_volume_knob_touched;

        if (want_shiftvol && !shortcut_leds_on) {
            shadow_queue_led(0x0B, 0xB0, 28, 118);  /* Step 13 icon = LightGrey (Tools) */
            shortcut_leds_on = 1;
        } else if (!want_shiftvol && shortcut_leds_on) {
            shadow_queue_led(0x0B, 0xB0, 28, 0);    /* Step 13 icon = off */
            shortcut_leds_on = 0;
        }

        if (want_longpress && !longpress_leds_on) {
            shadow_queue_led(0x0B, 0xB0, 17, 118);  /* Step 2 icon = LightGrey (Settings) */
            shadow_queue_led(0x0B, 0xB0, 28, 118);  /* Step 13 icon = LightGrey (Tools) */
            longpress_leds_on = 1;
        } else if (!want_longpress && longpress_leds_on) {
            shadow_queue_led(0x0B, 0xB0, 17, 0);    /* Step 2 icon = off */
            if (!shortcut_leds_on) {
                shadow_queue_led(0x0B, 0xB0, 28, 0);  /* Step 13 icon = off (only if Shift+Vol not on) */
            }
            longpress_leds_on = 0;
        }
    }
```

**Step 2: Commit**

```bash
git add src/schwung_shim.c
git commit -m "feat: light Step 2 and Step 13 LEDs on Shift when long-press enabled"
```

---

### Task 5: Add "Shortcuts" section to Global Settings UI

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add section to GLOBAL_SETTINGS_SECTIONS**

In `GLOBAL_SETTINGS_SECTIONS` (around line 800), add a new section before the "updates" section (after "services"):

```javascript
    {
        id: "shortcuts", label: "Shortcuts",
        items: [
            { key: "long_press_shadow", label: "Long Press", type: "bool" }
        ]
    },
```

**Step 2: Add get/set handlers for the setting**

In the `getGlobalSettingValue` function (around line 9844 area), add:

```javascript
    if (setting.key === "long_press_shadow") {
        return (shadow_control_get_long_press_shadow ? shadow_control_get_long_press_shadow() : false) ? "On" : "Off";
    }
```

Wait — looking at the pattern, `set_pages_enabled` uses `set_pages_get()` / `set_pages_set()` which are C host bindings exposed to JS. For this setting, we need to read/write the `shadow_control->long_press_shadow` field.

The simplest approach: use the existing `shadow_config.json` + `syncSettingsFromConfigFile` pattern. The setting is persisted in `shadow_config.json`, synced to `shadow_control->long_press_shadow` via the periodic sync, and the shim reads the shm field.

In the `getGlobalSettingValue` function, add:

```javascript
    if (setting.key === "long_press_shadow") {
        return longPressShadowEnabled ? "On" : "Off";
    }
```

In the `adjustGlobalSetting` function, add:

```javascript
    if (setting.key === "long_press_shadow") {
        longPressShadowEnabled = !longPressShadowEnabled;
        saveLongPressConfig();
        return;
    }
```

**Step 3: Add state variable and save/load functions**

Near the other global setting state variables:

```javascript
let longPressShadowEnabled = false;
```

Add save function (follow the pattern of `saveBrowserPreviewConfig`):

```javascript
function saveLongPressConfig() {
    try {
        const configPath = "/data/UserData/schwung/config/features.json";
        const content = host_read_file(configPath);
        let config = {};
        if (content) {
            try { config = JSON.parse(content); } catch (e) {}
        }
        config.long_press_shadow = longPressShadowEnabled;
        host_write_file(configPath, JSON.stringify(config, null, 2));
    } catch (e) {}
}
```

Add loading in the init section (where other features.json values are loaded):

```javascript
function loadLongPressConfig() {
    try {
        const configPath = "/data/UserData/schwung/config/features.json";
        const content = host_read_file(configPath);
        if (content) {
            const config = JSON.parse(content);
            if (config.long_press_shadow !== undefined) {
                longPressShadowEnabled = !!config.long_press_shadow;
            }
        }
    } catch (e) {}
}
```

Call `loadLongPressConfig()` from `init()`.

**Step 4: Add sync to shadow_config.json for web UI**

In `syncSettingsFromConfigFile()`, add:

```javascript
        /* Long-press shadow shortcuts */
        if (c.long_press_shadow !== undefined && c.long_press_shadow !== longPressShadowEnabled) {
            longPressShadowEnabled = !!c.long_press_shadow;
            saveLongPressConfig();  /* Persist to features.json so shim picks it up on next reload */
        }
```

**Step 5: Sync flag to shadow_control shm**

The shim reads `shadow_control->long_press_shadow`. The JS side needs to write it. Add to the periodic sync (or after save):

Since the shim reads features.json on startup, and the shadow_control_t field is the runtime flag, we need the shim to also watch the field. The simplest approach: after saving to features.json, also set the shm field directly.

In `saveLongPressConfig()`, after writing features.json, add:

```javascript
    /* Update shm flag so shim picks up the change immediately */
    if (typeof shadow_set_param === "function") {
        shadow_set_param(0, "long_press_shadow", longPressShadowEnabled ? "1" : "0");
    }
```

Then in the shim's param handler (in `shadow_chain_mgmt.c` or the shim's param dispatch), handle this key to set `shadow_control->long_press_shadow`.

**Alternative (simpler):** Since `shadow_control_t` is shared memory, check if the JS side can write directly. Looking at patterns, the `set_pages_set_shm` function does this. We'd need a similar `long_press_shadow_set` host binding.

Actually, the simplest path: add a `long_press_shadow_set`/`long_press_shadow_get` host binding pair in `schwung_host.c` that reads/writes `shadow_control->long_press_shadow`, mirroring how `set_pages_set_shm`/`set_pages_get` work. Also have it update the `long_press_shadow_enabled` flag in the shim's local static.

For now, since the shim reads `shadow_control->long_press_shadow` directly from shared memory, we just need the JS to write it there. The host binding approach is cleanest.

**Step 5 (revised): Add host bindings**

In `schwung_host.c`, add bindings (follow `set_pages_get`/`set_pages_set_shm` pattern):

```c
static JSValue js_long_press_shadow_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!host.shadow_control_ptr || !*host.shadow_control_ptr) return JS_FALSE;
    return JS_NewBool(ctx, (*host.shadow_control_ptr)->long_press_shadow);
}

static JSValue js_long_press_shadow_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!host.shadow_control_ptr || !*host.shadow_control_ptr) return JS_UNDEFINED;
    int val = 0;
    JS_ToInt32(ctx, &val, argv[0]);
    (*host.shadow_control_ptr)->long_press_shadow = val ? 1 : 0;
    return JS_UNDEFINED;
}
```

Register them in the global function table.

Then in shadow_ui.js, the get/set handlers become:

```javascript
    if (setting.key === "long_press_shadow") {
        return (typeof long_press_shadow_get === "function" && long_press_shadow_get()) ? "On" : "Off";
    }
```

```javascript
    if (setting.key === "long_press_shadow") {
        const current = typeof long_press_shadow_get === "function" ? long_press_shadow_get() : false;
        if (typeof long_press_shadow_set === "function") {
            long_press_shadow_set(!current ? 1 : 0);
        }
        /* Also persist to features.json */
        saveLongPressConfig();
        return;
    }
```

**Step 6: Commit**

```bash
git add src/shadow/shadow_ui.js src/schwung_host.c
git commit -m "feat: add Long Press setting to Global Settings shortcuts section"
```

---

### Task 6: Add settings-schema.json entry for web UI

**Files:**
- Modify: `src/shared/settings-schema.json`

**Step 1: Add entry**

Add a "shortcuts" section to the schema (follow existing section pattern):

```json
{
    "id": "shortcuts",
    "label": "Shortcuts",
    "items": [
        { "key": "long_press_shadow", "label": "Long Press", "type": "bool" }
    ]
}
```

**Step 2: Commit**

```bash
git add src/shared/settings-schema.json
git commit -m "feat: add shortcuts section to settings schema for web UI"
```

---

### Task 7: Build, deploy, and test

**Step 1: Build**

```bash
./scripts/build.sh
```

**Step 2: Deploy**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Test checklist**

1. **Default off:** Verify long-press Track/Menu does nothing by default
2. **Enable setting:** Open Global Settings (Shift+Vol+Step2) → Shortcuts → Long Press → On
3. **Long-press Track:** Hold Track 1 for ~750ms → shadow UI opens to Slot 1 settings
4. **Track tap dismissal:** With shadow UI displayed, tap Track 2 → shadow UI dismisses, Move switches to track 2
5. **Long-press Menu:** Hold Menu for ~750ms → shadow UI opens to master FX
6. **Shift+long-press Step2:** Hold Shift+Step2 for ~750ms → Schwung global settings opens
7. **Shift+Step2 tap:** Quick Shift+Step2 → opens Move native settings (not Schwung)
8. **Shift+Step13:** Shift+Step13 → tools menu opens immediately
9. **LED hints:** Hold Shift → Step 2 and Step 13 LEDs light up
10. **No conflict with Shift+Vol:** Shift+Vol+Track still works as before
11. **Shift cancels Track long-press:** Hold Track, then press Shift → long-press does not fire
12. **Disable setting:** Turn Long Press off → all long-press behavior gone

**Step 4: Commit any fixes**

---

### Task 8: Update documentation

**Files:**
- Modify: `MANUAL.md` — add long-press shortcuts to the shortcuts section
- Modify: `src/shadow/shadow_ui.js` — no changes needed if already documented in code

**Step 1: Update MANUAL.md**

Add to the shortcuts table:

```
- **Long-press Track 1-4**: Open shadow slot settings (optional, enable in Settings)
- **Long-press Menu**: Open master FX settings (optional)
- **Shift+hold Step2**: Open Schwung global settings (optional)
- **Shift+Step13**: Open tools menu (optional)
```

**Step 2: Commit**

```bash
git add MANUAL.md
git commit -m "docs: add long-press shortcuts to manual"
```
