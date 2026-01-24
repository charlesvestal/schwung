# Module Asset Requirements - Design

## Overview

Add support for modules that require external assets (ROM files, patches, soundfonts) with:
1. Scrollable descriptions in the module store
2. Post-install messages explaining what assets are needed
3. Error overlays when modules load without required assets

## Affected Modules

- **Dexed**: Requires .syx patch files (Dexed voice banks)
- **SF2**: Requires .sf2 soundfont files
- **Mini-JV**: Requires ROM files from original hardware

## Design

### 1. Extended release.json Format

Each module's `release.json` becomes the source of truth for store metadata:

```json
{
  "version": "0.2.1",
  "download_url": "https://github.com/.../dexed-module.tar.gz",
  "repo_url": "https://github.com/charlesvestal/move-anything-dx7",
  "name": "Dexed",
  "description": "Dexed FM synth with full 6-operator synthesis. Loads standard .syx patch banks.",
  "requires": ".syx patch files (Dexed voice banks)",
  "post_install": "Place .syx files in modules/dexed/patches/"
}
```

**Fields:**
- `version`, `download_url` - existing, required
- `repo_url` - new, optional, for "More Info" link
- `name` - new, module's display name (fallback to catalog if missing)
- `description` - new, full description for store display
- `requires` - new, optional, short string shown in store
- `post_install` - new, optional, message shown after install completes

The catalog (`module-catalog.json`) becomes minimal - just module IDs, repo URLs, and component_type for categorization.

### 2. Store UI - Scrollable Module Detail

Module detail screen shows scrollable description with fixed Install button:

```
┌──────────────────────────┐
│ Dexed         v0.2.1 │
├──────────────────────────┤
│   Dexed FM synth            │
│   emulation with full    │
│   6-operator synthesis.  │
│                          │▼
├──────────────────────────┤
│   [Install]              │
└──────────────────────────┘
```

**Behavior:**
- Description text scrolls without line highlighting
- Up/down arrows on right edge indicate more content
- Install button fixed at bottom, always visible
- Jog scrolls text; once last line visible, next jog selects Install
- Jog click on Install performs action
- Short descriptions don't scroll - jog down goes directly to Install

**After scrolling to end:**
```
┌──────────────────────────┐
│ Dexed         v0.2.1 │
├──────────────────────────┤
│   Loads .syx patches.    │▲
│   ⚠ Requires: .syx files │
│                          │
│                          │
├──────────────────────────┤
│ > [Install]              │
└──────────────────────────┘
```

Works in both standalone Module Store and Shadow UI Store Picker.

### 3. Post-Install Message

After successful install, if `post_install` is present, show overlay:

```
┌──────────────────────────┐
│ ┌──────────────────────┐ │
│ │                      │ │
│ │  Install Complete    │ │
│ │                      │ │
│ │  Place .syx files in │ │
│ │  modules/dexed/patches │ │
│ │                      │ │
│ │       [OK]           │ │
│ └──────────────────────┘ │
└──────────────────────────┘
```

**Behavior:**
- Centered overlay with border (similar to existing `drawStatusOverlay`)
- Shows "Install Complete" title
- Shows `post_install` message (word-wrapped, ~18 chars/line, max 3-4 lines)
- Jog click or Back dismisses, returns to store
- If no `post_install` field, skip overlay

### 4. Missing Assets Detection (Plugin API)

Add optional `get_error` function to plugin API:

```c
typedef struct plugin_api_v1 {
    uint32_t api_version;
    int (*on_load)(const char *module_dir, const char *json_defaults);
    void (*on_unload)(void);
    void (*on_midi)(const uint8_t *msg, int len, int source);
    void (*set_param)(const char *key, const char *val);
    int (*get_param)(const char *key, char *buf, int buf_len);
    void (*render_block)(int16_t *out_interleaved_lr, int frames);

    /* New - optional error message when on_load fails */
    int (*get_error)(char *buf, int buf_len);
} plugin_api_v1_t;
```

**Host behavior:**
```c
int result = plugin->on_load(module_dir, defaults);
if (result != 0) {
    char error[256] = "Module failed to load";
    if (plugin->get_error) {
        plugin->get_error(error, sizeof(error));
    }
    show_error_overlay(error);
    return;
}
```

**Module implementation example (Dexed):**
```c
static char load_error[256] = "";

int on_load(const char *dir, const char *defaults) {
    if (!find_syx_files(dir)) {
        snprintf(load_error, sizeof(load_error),
            "No .syx patches found.\nPlace files in modules/dexed/patches/");
        return -1;
    }
    return 0;
}

int get_error(char *buf, int len) {
    strncpy(buf, load_error, len);
    return 0;
}
```

**Error overlay:** Same style as post-install overlay with "Load Error" title.

## Implementation Order

1. **release.json + store_utils.mjs** - Extend format, fetch new fields
2. **Store UI scrollable detail** - New component for text + fixed action
3. **Post-install overlay** - Show after successful install
4. **Plugin API get_error** - Add to API header
5. **Host error handling** - Call get_error, show overlay
6. **Update Dexed, SF2, Mini-JV** - Add release.json fields and get_error

## Files to Modify

**Main repo (move-anything):**
- `src/shared/store_utils.mjs` - Fetch new release.json fields
- `src/modules/store/ui.js` - Scrollable detail, post-install overlay
- `src/shadow/shadow_ui.js` - Same changes for shadow store picker
- `src/host/plugin_api_v1.h` - Add get_error function pointer
- `src/move_anything.c` - Call get_error on load failure, show overlay
- `src/shared/menu_layout.mjs` - Add post-install/error overlay helpers

**External module repos:**
- `move-anything-dx7/release.json` - Add description, requires, post_install
- `move-anything-dx7/src/dsp/plugin.c` - Implement get_error
- `move-anything-sf2/release.json` - Add fields
- `move-anything-sf2/src/dsp/plugin.c` - Implement get_error
- `move-anything-jv880/release.json` - Add fields
- `move-anything-jv880/src/dsp/plugin.c` - Implement get_error
