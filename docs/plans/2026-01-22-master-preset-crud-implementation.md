# Master Preset CRUD Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement save/load/delete for master FX presets with picker UI and name preview flow.

**Architecture:** Add `/presets_master/` directory handling, mirror slot preset CRUD, scroll left from Master FX settings to open preset picker.

**Tech Stack:** JavaScript (shadow_ui.js), chain_host.c for DSP-side file I/O

---

## Task 1: Add Master Preset State Variables

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add state variables after slot preset state (~line 260)**

```javascript
/* Master preset state */
let masterPresets = [];              // List of {name, index} from /presets_master/
let selectedMasterPresetIndex = 0;   // Index in picker (0 = [New])
let currentMasterPresetName = "";    // Name of loaded preset ("" if new/unsaved)
let inMasterPresetPicker = false;    // True when showing preset picker

/* Master preset CRUD state (reuse pattern from slot presets) */
let masterPendingSaveName = "";
let masterOverwriteTargetIndex = -1;
let masterConfirmingOverwrite = false;
let masterConfirmingDelete = false;
let masterConfirmIndex = 0;
let masterOverwriteFromKeyboard = false;
let masterShowingNamePreview = false;
let masterNamePreviewIndex = 0;
```

**Step 2: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): add master preset state variables"
```

---

## Task 2: Add DSP Commands for Master Presets

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

**Step 1: Add presets_master directory constant**

Near other directory paths, add:
```c
#define PRESETS_MASTER_DIR "/data/UserData/move-anything/presets_master"
```

**Step 2: Add list_master_presets handler in set_param**

Add case in the set_param switch to handle "list_master_presets":
```c
} else if (strcmp(key, "list_master_presets") == 0) {
    /* List all master presets - returns comma-separated names */
    ensure_presets_master_dir();
    list_master_presets(param_response_buf, sizeof(param_response_buf));
    return;
```

**Step 3: Add ensure_presets_master_dir function**

```c
static void ensure_presets_master_dir(void) {
    struct stat st = {0};
    if (stat(PRESETS_MASTER_DIR, &st) == -1) {
        mkdir(PRESETS_MASTER_DIR, 0755);
    }
}
```

**Step 4: Add list_master_presets function**

```c
static void list_master_presets(char *buf, size_t buf_len) {
    buf[0] = '\0';
    DIR *dir = opendir(PRESETS_MASTER_DIR);
    if (!dir) return;

    struct dirent *entry;
    size_t offset = 0;
    int first = 1;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len < 6 || strcmp(name + len - 5, ".json") != 0) continue;

        /* Extract name without .json extension */
        char preset_name[256];
        size_t name_len = len - 5;
        if (name_len >= sizeof(preset_name)) name_len = sizeof(preset_name) - 1;
        memcpy(preset_name, name, name_len);
        preset_name[name_len] = '\0';

        /* Append to buffer */
        size_t needed = strlen(preset_name) + (first ? 0 : 1);
        if (offset + needed >= buf_len - 1) break;

        if (!first) buf[offset++] = ',';
        strcpy(buf + offset, preset_name);
        offset += strlen(preset_name);
        first = 0;
    }
    closedir(dir);
}
```

**Step 5: Add save_master_preset handler**

```c
} else if (strcmp(key, "save_master_preset") == 0) {
    /* Save master preset: value is JSON with custom_name */
    ensure_presets_master_dir();
    save_master_preset(val);
    return;
```

**Step 6: Add save_master_preset function**

```c
static void save_master_preset(const char *json_str) {
    /* Parse JSON to extract custom_name */
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        chain_log("save_master_preset: failed to parse JSON");
        return;
    }

    cJSON *name_obj = cJSON_GetObjectItem(json, "custom_name");
    const char *name = name_obj ? name_obj->valuestring : "Untitled";

    /* Build full preset JSON with wrapper */
    cJSON *preset = cJSON_CreateObject();
    cJSON_AddStringToObject(preset, "name", name);
    cJSON_AddNumberToObject(preset, "version", 1);

    /* Copy master_fx object */
    cJSON *master_fx = cJSON_CreateObject();
    cJSON *fx1 = cJSON_GetObjectItem(json, "fx1");
    cJSON *fx2 = cJSON_GetObjectItem(json, "fx2");
    cJSON *fx3 = cJSON_GetObjectItem(json, "fx3");
    cJSON *fx4 = cJSON_GetObjectItem(json, "fx4");
    if (fx1) cJSON_AddItemToObject(master_fx, "fx1", cJSON_Duplicate(fx1, 1));
    if (fx2) cJSON_AddItemToObject(master_fx, "fx2", cJSON_Duplicate(fx2, 1));
    if (fx3) cJSON_AddItemToObject(master_fx, "fx3", cJSON_Duplicate(fx3, 1));
    if (fx4) cJSON_AddItemToObject(master_fx, "fx4", cJSON_Duplicate(fx4, 1));
    cJSON_AddItemToObject(preset, "master_fx", master_fx);

    char *preset_str = cJSON_PrintUnformatted(preset);
    cJSON_Delete(preset);
    cJSON_Delete(json);

    /* Write to file */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", PRESETS_MASTER_DIR, name);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(preset_str, f);
        fclose(f);
        chain_log("Saved master preset: %s", name);
    }
    free(preset_str);
}
```

**Step 7: Add update_master_preset and delete_master_preset handlers**

Similar pattern to slot presets - update takes "index:json", delete takes index.

**Step 8: Add load_master_preset handler**

Returns JSON for the preset at given index.

**Step 9: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "feat(chain): add master preset DSP commands"
```

---

## Task 3: Add Master Preset Picker UI

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add loadMasterPresetList function**

```javascript
function loadMasterPresetList() {
    /* Get comma-separated list from DSP */
    const listStr = getSlotParam(0, "list_master_presets") || "";
    masterPresets = [];

    if (listStr) {
        const names = listStr.split(",");
        for (let i = 0; i < names.length; i++) {
            masterPresets.push({ name: names[i], index: i });
        }
    }
}
```

**Step 2: Add enterMasterPresetPicker function**

```javascript
function enterMasterPresetPicker() {
    loadMasterPresetList();
    inMasterPresetPicker = true;
    selectedMasterPresetIndex = 0;  // Start at [New]
    needsRedraw = true;
}
```

**Step 3: Add drawMasterPresetPicker function**

```javascript
function drawMasterPresetPicker() {
    drawHeader("Master Presets");

    /* Build items: [New] + presets */
    const items = [{ name: "[New]", index: -1 }];
    for (let i = 0; i < masterPresets.length; i++) {
        items.push(masterPresets[i]);
    }

    drawMenuList({
        items: items,
        selectedIndex: selectedMasterPresetIndex,
        getLabel: (item) => item.name,
        listArea: { topY: LIST_TOP_Y, bottomY: LIST_BOTTOM_Y }
    });

    drawFooter("Back: cancel");
}
```

**Step 4: Update scroll left handler in MASTER_FX view**

In jog handler, when at FX1 (selectedMasterFxComponent === 0) and scrolling up (delta < 0):

```javascript
if (selectedMasterFxComponent === 0 && delta < 0) {
    enterMasterPresetPicker();
    return;
}
```

**Step 5: Add click handler for preset picker**

```javascript
if (inMasterPresetPicker) {
    if (selectedMasterPresetIndex === 0) {
        /* [New] - clear master FX and exit picker */
        clearMasterFx();
        currentMasterPresetName = "";
        inMasterPresetPicker = false;
    } else {
        /* Load selected preset */
        const preset = masterPresets[selectedMasterPresetIndex - 1];
        loadMasterPreset(preset.name);
        currentMasterPresetName = preset.name;
        inMasterPresetPicker = false;
    }
    needsRedraw = true;
    break;
}
```

**Step 6: Add back handler for preset picker**

```javascript
if (inMasterPresetPicker) {
    inMasterPresetPicker = false;
    needsRedraw = true;
    break;
}
```

**Step 7: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): add master preset picker UI"
```

---

## Task 4: Add Master Preset Settings Menu

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add MASTER_FX_PRESET_SETTINGS_ITEMS**

```javascript
const MASTER_FX_PRESET_SETTINGS_ITEMS = [
    { key: "fx1", label: "FX 1", type: "slot" },
    { key: "fx2", label: "FX 2", type: "slot" },
    { key: "fx3", label: "FX 3", type: "slot" },
    { key: "fx4", label: "FX 4", type: "slot" },
    { key: "volume", label: "Volume", type: "float", min: 0, max: 1, step: 0.05 },
    { key: "save", label: "[Save]", type: "action" },
    { key: "save_as", label: "[Save As]", type: "action" },
    { key: "delete", label: "[Delete]", type: "action" }
];
```

**Step 2: Add getMasterFxSettingsItems function**

Dynamic items based on whether preset is new or existing:

```javascript
function getMasterFxSettingsItems() {
    if (currentMasterPresetName) {
        /* Existing preset: show all */
        return MASTER_FX_PRESET_SETTINGS_ITEMS;
    }
    /* New: only Save (no Save As or Delete) */
    return MASTER_FX_PRESET_SETTINGS_ITEMS.filter(item =>
        item.key !== "save_as" && item.key !== "delete"
    );
}
```

**Step 3: Update action handlers for Save/Save As/Delete**

Mirror the slot preset logic with name preview flow.

**Step 4: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): add master preset settings with CRUD actions"
```

---

## Task 5: Add Master Preset Name Preview and Overwrite

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add generateMasterPresetName function**

```javascript
function generateMasterPresetName() {
    const parts = [];
    for (let i = 0; i < 4; i++) {
        const key = `fx${i + 1}`;
        const moduleId = masterFxConfig[key]?.module;
        if (moduleId) {
            const abbrev = moduleAbbrevCache[moduleId] || moduleId.toUpperCase().slice(0, 3);
            parts.push(abbrev);
        }
    }
    return parts.length > 0 ? parts.join(" + ") : "Master FX";
}
```

**Step 2: Add drawMasterNamePreview function**

Same pattern as slot name preview.

**Step 3: Add drawMasterConfirmOverwrite function**

Same pattern as slot overwrite confirmation.

**Step 4: Add drawMasterConfirmDelete function**

Same pattern as slot delete confirmation.

**Step 5: Wire up click handlers**

Handle masterShowingNamePreview, masterConfirmingOverwrite, masterConfirmingDelete states.

**Step 6: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): add master preset name preview and confirmations"
```

---

## Task 6: Add clearMasterFx and loadMasterPreset Functions

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add clearMasterFx function**

```javascript
function clearMasterFx() {
    /* Clear all 4 FX slots */
    for (let i = 0; i < 4; i++) {
        setMasterFxSlotModule(i, "");
        masterFxConfig[`fx${i + 1}`].module = "";
    }
    saveMasterFxChainConfig();
}
```

**Step 2: Add loadMasterPreset function**

```javascript
function loadMasterPreset(name) {
    /* Tell DSP to load the preset */
    const json = getSlotParam(0, "load_master_preset:" + name);
    if (!json) return;

    try {
        const preset = JSON.parse(json);
        const fx = preset.master_fx || {};

        /* Apply each FX slot */
        for (let i = 0; i < 4; i++) {
            const key = `fx${i + 1}`;
            const fxConfig = fx[key];
            if (fxConfig && fxConfig.type) {
                /* Find module path from type */
                const opt = MASTER_FX_OPTIONS.find(o => o.id === fxConfig.type);
                if (opt) {
                    setMasterFxSlotModule(i, opt.dspPath || "");
                    masterFxConfig[key].module = opt.id;
                }
            } else {
                setMasterFxSlotModule(i, "");
                masterFxConfig[key].module = "";
            }
        }

        saveMasterFxChainConfig();
    } catch (e) {
        /* Parse error */
    }
}
```

**Step 3: Add buildMasterPresetJson function**

```javascript
function buildMasterPresetJson(name) {
    const preset = {
        custom_name: name,
        fx1: null,
        fx2: null,
        fx3: null,
        fx4: null
    };

    for (let i = 0; i < 4; i++) {
        const key = `fx${i + 1}`;
        const moduleId = masterFxConfig[key]?.module;
        if (moduleId) {
            preset[key] = {
                type: moduleId,
                params: {}  // TODO: capture current params
            };
        }
    }

    return JSON.stringify(preset);
}
```

**Step 4: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): add master preset load/clear/build functions"
```

---

## Task 7: Add DSP load_master_preset Handler

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

**Step 1: Add load_master_preset handler**

In get_param, handle keys starting with "load_master_preset:":

```c
if (strncmp(key, "load_master_preset:", 19) == 0) {
    const char *name = key + 19;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", PRESETS_MASTER_DIR, name);

    FILE *f = fopen(path, "r");
    if (!f) {
        buf[0] = '\0';
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len >= buf_len) len = buf_len - 1;
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return len;
}
```

**Step 2: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "feat(chain): add load_master_preset DSP command"
```

---

## Task 8: Add Default Master Presets

**Files:**
- Create: `src/presets_master/Subtle Verb.json`
- Create: `src/presets_master/Tape Warmth.json`
- Create: `src/presets_master/Lo-Fi Master.json`

**Step 1: Create Subtle Verb preset**

```json
{
  "name": "Subtle Verb",
  "version": 1,
  "master_fx": {
    "fx1": { "type": "freeverb", "params": { "wet": 0.15, "room": 0.5, "damp": 0.5 } }
  }
}
```

**Step 2: Create Tape Warmth preset**

```json
{
  "name": "Tape Warmth",
  "version": 1,
  "master_fx": {
    "fx1": { "type": "tapescam", "params": { "saturation": 0.3, "wobble": 0.1 } }
  }
}
```

**Step 3: Create Lo-Fi Master preset**

```json
{
  "name": "Lo-Fi Master",
  "version": 1,
  "master_fx": {
    "fx1": { "type": "tapescam", "params": { "saturation": 0.5, "wobble": 0.2 } },
    "fx2": { "type": "freeverb", "params": { "wet": 0.2, "room": 0.4 } }
  }
}
```

**Step 4: Update build.sh to copy default presets**

Add to scripts/build.sh:
```bash
# Copy default master presets
mkdir -p ./dist/presets_master/
cp src/presets_master/*.json ./dist/presets_master/
```

**Step 5: Update package.sh to include presets**

Add presets_master to the tarball.

**Step 6: Commit**

```bash
git add src/presets_master/ scripts/build.sh scripts/package.sh
git commit -m "feat: add default master presets"
```

---

## Task 9: Wire Up Draw Switch

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Update drawMasterFx function**

Add early returns for picker and CRUD states:

```javascript
function drawMasterFx() {
    /* Handle text entry */
    if (isTextEntryActive()) {
        drawTextEntry();
        return;
    }

    /* Handle preset picker */
    if (inMasterPresetPicker) {
        drawMasterPresetPicker();
        return;
    }

    /* Handle name preview */
    if (masterShowingNamePreview) {
        drawMasterNamePreview();
        return;
    }

    /* Handle overwrite confirmation */
    if (masterConfirmingOverwrite) {
        drawMasterConfirmOverwrite();
        return;
    }

    /* Handle delete confirmation */
    if (masterConfirmingDelete) {
        drawMasterConfirmDelete();
        return;
    }

    /* ... existing FX component list drawing ... */
}
```

**Step 2: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): wire up master preset draw states"
```

---

## Task 10: Test All Flows

**Manual Testing Checklist:**

1. **Scroll left from Master FX → Preset picker appears**
2. **[New] clears all master FX**
3. **Load preset applies FX to slots**
4. **Save (new) → name preview → OK saves**
5. **Save (existing) → overwrite confirm → Yes saves**
6. **Save As → name preview → edit name → OK saves as new**
7. **Delete → confirm → Yes deletes preset**
8. **Back navigates correctly from all states**
9. **Default presets appear in picker**

**Step 1: Test on device**

```bash
./scripts/build.sh && ./scripts/install.sh local
```

**Step 2: Commit test notes**

```bash
git commit --allow-empty -m "test(shadow): verify all master preset CRUD flows"
```

---

## Summary

After completing all tasks:
- Scroll left from Master FX settings → Preset picker
- [New] clears master FX chain
- Load preset applies saved FX configuration
- Save/Save As/Delete with name preview and confirmations
- Default presets shipped: Subtle Verb, Tape Warmth, Lo-Fi Master
- Same UX patterns as slot presets
