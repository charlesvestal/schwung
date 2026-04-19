# Menu Style v2 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a `menu_style_v2` feature flag that flips the shared menu helpers (`drawMenuHeader`, `drawMenuList`) to a tamzen-9px header / tamzen-15px list layout with corrected char-width math. Default `false`. Settings toggle persists to `features.json`. Bucket A modules (16 of them) auto-pick up via shared helpers.

**Architecture:** Mirrors the existing `display_mirror_set` / `set_pages_set` pattern in `shadow_ui.c` (SHM field + persistent JSON setter + JS getter). `menu_layout.mjs` reads the flag once at module load via `host_read_file`, branches inside `drawMenuHeader` / `drawMenuList`. Classic path stays byte-identical.

**Tech Stack:** QuickJS (UI), C (shadow_ui setter + SHM struct), shared `.mjs` helpers, tamzen bitmap fonts in `host/fonts/`.

**Design doc:** `docs/plans/2026-04-19-menu-style-v2-design.md`

**Testing:** Schwung has no automated test suite. "Verification" steps mean a hardware deploy via `./scripts/install.sh local --skip-modules --skip-confirmation` and visual walk of menu screens. Use the unified logger (`/data/UserData/schwung/debug.log`) for state inspection.

---

## Task 1: Add `menu_style_v2` to SHM struct

**Files:**
- Modify: `src/host/shadow_constants.h:148`

**Step 1: Edit struct**

Replace `volatile uint8_t reserved[5];` at line 148 with:

```c
    volatile uint8_t menu_style_v2;     /* 0=classic, 1=tamzen v2 menu layout */
    volatile uint8_t reserved[4];
```

**Step 2: Verify compile**

```bash
./scripts/build.sh 2>&1 | tail -20
```

Expected: build succeeds. SHM struct size unchanged (still uses `reserved[]` budget).

**Step 3: Commit**

```bash
git add src/host/shadow_constants.h
git commit -m "feat: add menu_style_v2 field to shadow_control_t SHM"
```

---

## Task 2: Add C-side setter/getter in `shadow_ui.c`

**Files:**
- Modify: `src/shadow/shadow_ui.c` — add new setter functions after the `long_press_shadow_set` block (~line 1820–1890), register bindings near where `display_mirror_set` is registered

**Step 1: Add functions**

After the last `*_set_shm` / `*_get` block in the cluster around line 1820, add:

```c
/* menu_style_v2_set(enabled) - Write to shared memory + persist to features.json */
static JSValue js_menu_style_v2_set(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;

    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->menu_style_v2 = enabled ? 1 : 0;

    const char *config_path = "/data/UserData/schwung/config/features.json";
    char buf[512];
    size_t len = 0;
    FILE *f = fopen(config_path, "r");
    if (f) {
        len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[len] = '\0';

    char *key = strstr(buf, "\"menu_style_v2\"");
    if (key) {
        char *colon = strchr(key, ':');
        if (colon) {
            colon++;
            while (*colon == ' ') colon++;
            char *val_end = colon;
            while (*val_end && *val_end != ',' && *val_end != '\n' && *val_end != '}') val_end++;
            char newbuf[512];
            int prefix_len = (int)(colon - buf);
            int suffix_start = (int)(val_end - buf);
            snprintf(newbuf, sizeof(newbuf), "%.*s%s%s",
                     prefix_len, buf,
                     enabled ? "true" : "false",
                     buf + suffix_start);
            f = fopen(config_path, "w");
            if (f) { fputs(newbuf, f); fclose(f); }
        }
    } else if (len > 0) {
        char *brace = strrchr(buf, '}');
        if (brace) {
            char newbuf[512];
            int prefix_len = (int)(brace - buf);
            snprintf(newbuf, sizeof(newbuf), "%.*s,\n  \"menu_style_v2\": %s\n}",
                     prefix_len, buf, enabled ? "true" : "false");
            f = fopen(config_path, "w");
            if (f) { fputs(newbuf, f); fclose(f); }
        }
    } else {
        /* No file yet — create */
        f = fopen(config_path, "w");
        if (f) {
            fprintf(f, "{\n  \"menu_style_v2\": %s\n}\n", enabled ? "true" : "false");
            fclose(f);
        }
    }

    return JS_UNDEFINED;
}

static JSValue js_menu_style_v2_get(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, shadow_control->menu_style_v2 != 0);
}
```

**Step 2: Register bindings**

Find the cluster where `display_mirror_set` and `display_mirror_get` are registered (`JS_NewCFunction` calls). Add:

```c
JS_SetPropertyStr(ctx, global_obj, "menu_style_v2_set",
    JS_NewCFunction(ctx, js_menu_style_v2_set, "menu_style_v2_set", 1));
JS_SetPropertyStr(ctx, global_obj, "menu_style_v2_get",
    JS_NewCFunction(ctx, js_menu_style_v2_get, "menu_style_v2_get", 0));
```

**Step 3: Initial value from features.json on startup**

Find where `display_mirror` is loaded from features.json at shadow_ui startup (search `"display_mirror"` near init). Add a parallel block reading `menu_style_v2` and setting `shadow_control->menu_style_v2`.

**Step 4: Build**

```bash
./scripts/build.sh 2>&1 | tail -20
```

Expected: build succeeds.

**Step 5: Commit**

```bash
git add src/shadow/shadow_ui.c
git commit -m "feat: add menu_style_v2_set/get bindings + features.json persistence"
```

---

## Task 3: Add flag cache to `menu_layout.mjs`

**Files:**
- Modify: `src/shared/menu_layout.mjs` (top of file, after imports)

**Step 1: Add cache + reload helper**

After the existing `import` block and constants, add:

```js
/* === Menu Style v2 (feature-flagged) === */
let _menuStyleV2 = false;
let _menuStyleLoaded = false;

function _loadMenuStyle() {
    try {
        const raw = (typeof host_read_file === 'function')
            ? host_read_file('/data/UserData/schwung/config/features.json')
            : null;
        if (raw) {
            const cfg = JSON.parse(raw);
            _menuStyleV2 = cfg && cfg.menu_style_v2 === true;
        }
    } catch (e) {
        _menuStyleV2 = false;
    }
    _menuStyleLoaded = true;
}

function isMenuStyleV2() {
    if (!_menuStyleLoaded) _loadMenuStyle();
    return _menuStyleV2;
}

export function reloadMenuStyle() {
    _menuStyleLoaded = false;
    _loadMenuStyle();
}

/* V2 layout constants (override classic when flag is on) */
const V2_TITLE_Y = -1;
const V2_TITLE_RULE_Y = 7;
const V2_LIST_TOP_Y = 6;
const V2_LIST_LINE_HEIGHT = 11;
const V2_LIST_HIGHLIGHT_OFFSET = 3;
const V2_HIGHLIGHT_PADDING = -2;       /* shrink highlight rect by 2px */
const V2_HEADER_FONT = '/data/UserData/schwung/host/fonts/tamzen-9.png';
const V2_LIST_FONT = '/data/UserData/schwung/host/fonts/tamzen-15.png';
const V2_DEFAULT_FONT = '/data/UserData/schwung/host/font.png';
```

**Step 2: Verify no rendering change**

Build, deploy, walk a couple of menu screens. Expected: visually identical to before (flag is off).

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Commit**

```bash
git add src/shared/menu_layout.mjs
git commit -m "feat: add menu_style_v2 flag cache + reload to menu_layout"
```

---

## Task 4: Add v2 branch to `drawMenuHeader`

**Files:**
- Modify: `src/shared/menu_layout.mjs:40-50` (`drawMenuHeader` function)

**Step 1: Replace function body**

```js
export function drawMenuHeader(title, titleRight = "") {
    const v2 = isMenuStyleV2();
    if (v2 && typeof set_font === 'function') set_font(V2_HEADER_FONT);

    const titleY = v2 ? V2_TITLE_Y : TITLE_Y;
    const ruleY = v2 ? V2_TITLE_RULE_Y : TITLE_RULE_Y;

    print(2, titleY, title, 1);

    if (titleRight) {
        const rightW = (typeof text_width === 'function') ? text_width(titleRight) : (titleRight.length * DEFAULT_CHAR_WIDTH);
        const rightX = SCREEN_WIDTH - rightW - 2;
        print(Math.max(2, rightX), titleY, titleRight, 1);
    }

    fill_rect(0, ruleY, SCREEN_WIDTH, 1, 1);

    if (v2 && typeof set_font === 'function') set_font(V2_DEFAULT_FONT);
}
```

**Step 2: Deploy and verify (flag still off)**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Walk menu screens. Expected: visually identical to before.

**Step 3: Toggle flag on device, verify v2 header**

```bash
ssh ableton@move.local 'echo "{\"menu_style_v2\": true}" > /data/UserData/schwung/config/features.json && pkill shadow_ui'
```

Re-enter a menu (e.g. Shift+Vol+Step2 → Settings). Expected: title text is now tamzen-9px, sits higher on the screen, rule moved up.

**Step 4: Toggle off, verify rollback**

```bash
ssh ableton@move.local 'echo "{\"menu_style_v2\": false}" > /data/UserData/schwung/config/features.json && pkill shadow_ui'
```

Re-enter menu. Expected: classic header restored.

**Step 5: Commit**

```bash
git add src/shared/menu_layout.mjs
git commit -m "feat: drawMenuHeader v2 branch (tamzen-9px header)"
```

---

## Task 5: Add v2 branch + char-width fix to `drawMenuList`

**Files:**
- Modify: `src/shared/menu_layout.mjs:85-239` (`drawMenuList` function)

**Step 1: Read v2 + probe char width at top of function**

Inside `drawMenuList`, immediately after the function signature destructure but before any other logic, add:

```js
    const v2 = isMenuStyleV2();
    if (v2 && typeof set_font === 'function') set_font(V2_LIST_FONT);

    const charW = (v2 && typeof text_width === 'function')
        ? (text_width("M") || DEFAULT_CHAR_WIDTH)
        : DEFAULT_CHAR_WIDTH;

    const v2LineHeight = v2 ? V2_LIST_LINE_HEIGHT : lineHeight;
    const v2HighlightOffset = v2 ? V2_LIST_HIGHLIGHT_OFFSET : highlightOffset;
    const v2HighlightPadding = v2 ? V2_HIGHLIGHT_PADDING : 0;
    const v2TopY = v2 ? V2_LIST_TOP_Y : (listArea?.topY ?? topY);
```

**Step 2: Replace `lineHeight` usage**

Find and replace within the function body (only inside `drawMenuList`):
- `lineHeight` → `v2LineHeight` (in `itemHeight` and `itemHighlightHeight` calculations)
- `highlightHeight` → `(v2 ? v2LineHeight + v2HighlightPadding : highlightHeight)`
- `highlightOffset` → `v2HighlightOffset`
- `resolvedTopY = listArea?.topY ?? topY` → `resolvedTopY = v2TopY`

**Step 3: Replace 6 `DEFAULT_CHAR_WIDTH` references with `charW`**

Inside `drawMenuList` only (lines 167, 171, 177, 187, 191, 211–212 in classic layout), replace `DEFAULT_CHAR_WIDTH` → `charW`. Keep `DEFAULT_CHAR_WIDTH` everywhere else in the file untouched.

**Step 4: Restore default font before returning**

Add immediately before the function's final `}`:

```js
    if (v2 && typeof set_font === 'function') set_font(V2_DEFAULT_FONT);
```

If the function returns early in any branch, add the restore there too.

**Step 5: Deploy + verify (flag off)**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Walk every Bucket A screen with flag still false: host menu, settings, slot edit, patches list, master FX, store, song mode, chain UI. Expected: zero visual change.

**Step 6: Toggle flag on, walk same screens**

```bash
ssh ableton@move.local 'echo "{\"menu_style_v2\": true}" > /data/UserData/schwung/config/features.json && pkill shadow_ui'
```

Expected:
- Headers in tamzen-9px (smaller)
- List items in tamzen-15px (larger, ~5 per screen still fits)
- Highlight rect tighter around selected row
- Labels truncate at the correct column (no overflow into values)
- Right-aligned values sit at the correct edge

If anything renders broken (text overflow, highlight off-by-N, items hidden), capture which screen and roll back: `echo '{"menu_style_v2": false}' > features.json && pkill shadow_ui`.

**Step 7: Commit**

```bash
git add src/shared/menu_layout.mjs
git commit -m "feat: drawMenuList v2 branch (tamzen-15px list + text_width fix)"
```

---

## Task 6: Add settings toggle in `shadow_ui.js`

**Files:**
- Modify: `src/shadow/shadow_ui.js:805-864` (add to GLOBAL_SETTINGS_SECTIONS), `~10006` (add toggle handler)

**Step 1: Add to "Display" section**

In `GLOBAL_SETTINGS_SECTIONS[0].items` (the "Display" section starting at line 808), append:

```js
{ key: "menu_style_v2", label: "Menu Style v2", type: "bool" }
```

**Step 2: Add toggle handler**

After the `display_mirror` toggle handler (line ~10006-10011), add:

```js
if (setting.key === "menu_style_v2" && typeof menu_style_v2_set === "function") {
    const current = typeof menu_style_v2_get === "function" ? menu_style_v2_get() : false;
    menu_style_v2_set(!current ? 1 : 0);
    /* Live-reload cache so toggle takes effect without restart */
    try {
        const ml = require('/data/UserData/schwung/shared/menu_layout.mjs');
        if (ml && typeof ml.reloadMenuStyle === 'function') ml.reloadMenuStyle();
    } catch (e) {}
    return;
}
```

If `require` is not available (ES modules), instead import `reloadMenuStyle` at the top of `shadow_ui.js` from `menu_layout.mjs` and call directly.

**Step 3: Add value getter for the bool display**

Find `getMasterFxSettingValue` (search for the `display_mirror` value-fetch case). Add a parallel `menu_style_v2` case that returns `menu_style_v2_get() ? "On" : "Off"`.

**Step 4: Deploy + verify**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Press Shift+Vol+Step2 → Display → "Menu Style v2". Toggle with jog wheel. Expected:
- Toggling immediately updates the displayed value (On / Off)
- Backing out and re-entering a menu shows the new layout
- Value persists across `pkill shadow_ui`

**Step 5: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: add Menu Style v2 toggle to global settings"
```

---

## Task 7: Hardware validation pass

**Files:**
- (no code changes — testing only)

**Step 1: Set flag on**

```bash
ssh ableton@move.local 'echo "{\"menu_style_v2\": true}" > /data/UserData/schwung/config/features.json && pkill shadow_ui'
```

**Step 2: Walk every Bucket A screen**

Visit each, look for visual breakage (overflow, mis-truncated labels, off-screen items, mis-aligned values):

- [ ] Host menu (module list)
- [ ] Settings (Shift+Vol+Step2) — Display, Audio, Screen Reader, Set Pages, Services, Updates
- [ ] Shadow slots (Shift+Vol+Track 1-4) — slot settings, MIDI receive/forward, volume
- [ ] Patches list (per slot)
- [ ] Master FX (Shift+Vol+Menu)
- [ ] Module Store
- [ ] Tools menu (Shift+Vol+Step13)
- [ ] Song Mode
- [ ] Chain UI (load any patch with sound generator + audio FX)
- [ ] Chain freeverb sub-UI

**Step 3: Capture before/after for archive**

If `ui-test` capture flow is wired up to write JSON, capture the same screens with flag on vs off.

**Step 4: Document any regressions**

If any screen breaks, file the issue in `docs/plans/2026-04-19-menu-style-v2-regressions.md` with screen name and symptom. Do not flip default in code until regressions are resolved.

**Step 5: Commit captures (if any)**

```bash
git add docs/plans/2026-04-19-menu-style-v2-*.md
git commit -m "docs: menu-style-v2 hardware validation results"
```

---

## Task 8: Flip default to true (only after Task 7 passes)

**Files:**
- Modify: `src/shared/menu_layout.mjs` `_loadMenuStyle` function

**Step 1: Default to true when no features.json exists**

Change the catch / unset path so absence of the key means `true` instead of `false`. Or update the build's seeded `features.json` template to include `"menu_style_v2": true`.

**Step 2: Deploy fresh, verify**

Wipe local features.json on a test device, deploy, verify v2 is on by default.

**Step 3: Update CLAUDE.md and MANUAL.md**

Add a note about the new setting under Global Settings section.

**Step 4: Commit + open PR**

```bash
git add src/shared/menu_layout.mjs CLAUDE.md MANUAL.md
git commit -m "feat: enable Menu Style v2 by default"
```
