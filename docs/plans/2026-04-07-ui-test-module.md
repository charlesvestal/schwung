# UI Test Module Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add multi-size bitmap font support (Tamzen) and a tool module for live-tweaking header/menu layout parameters on device.

**Architecture:** Extend `js_display.c` with `set_font(path)` and `get_font_height()` JS bindings so modules can switch between pre-generated Tamzen bitmap fonts at different pixel sizes. Bundle Tamzen BDF fonts converted to the existing PNG strip format. Build a tool module (`ui-test`) with knob-driven parameter adjustment and a transient status overlay.

**Tech Stack:** C (js_display.c bindings), Python (font generation from BDF), JavaScript (tool module UI)

---

### Task 1: Add Tamzen BDF files to the repo

**Files:**
- Create: `fonts/tamzen/Tamzen5x9r.bdf`
- Create: `fonts/tamzen/Tamzen6x12r.bdf`
- Create: `fonts/tamzen/Tamzen7x13r.bdf`
- Create: `fonts/tamzen/Tamzen7x14r.bdf`
- Create: `fonts/tamzen/Tamzen8x15r.bdf`
- Create: `fonts/tamzen/Tamzen8x16r.bdf`
- Create: `fonts/tamzen/Tamzen10x20r.bdf`
- Create: `fonts/tamzen/LICENSE`

**Step 1: Download Tamzen BDF files**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung
mkdir -p fonts/tamzen
# Download from https://github.com/sunaku/tamzen-font
# We need the regular (non-bold) BDF files
```

Tamzen is MIT licensed. Download the 7 regular BDF sizes from the GitHub repo.

**Step 2: Commit**

```bash
git add fonts/tamzen/
git commit -m "feat: add Tamzen bitmap font BDF sources"
```

---

### Task 2: Extend generate_font.py to convert BDF to PNG strips

**Files:**
- Modify: `scripts/generate_font.py`

**Step 1: Add BDF parsing and PNG strip generation**

Add a `generate_from_bdf(bdf_path, output_path)` function that:
1. Parses the BDF file to extract glyph bitmaps for ASCII 32-126
2. Outputs a PNG strip + .dat file in the same format as `generate_deployment_png()`
   - RGBA image, transparent background, white foreground
   - Width = numChars * charW, height = charH
   - .dat sidecar lists characters in order

Add a `--bdf` CLI flag: `python3 generate_font.py --bdf fonts/tamzen/Tamzen8x15r.bdf --deploy-png build/host/fonts/tamzen-15.png`

**Step 2: Test locally**

```bash
python3 scripts/generate_font.py --bdf fonts/tamzen/Tamzen8x15r.bdf --deploy-png /tmp/test-tamzen-15.png
# Verify /tmp/test-tamzen-15.png and /tmp/test-tamzen-15.png.dat exist
```

**Step 3: Commit**

```bash
git add scripts/generate_font.py
git commit -m "feat: add BDF font import to generate_font.py"
```

---

### Task 3: Add font generation to build.sh

**Files:**
- Modify: `scripts/build.sh` (near line 164, after existing font generation)

**Step 1: Add Tamzen font generation**

After the existing font.png generation block, add:

```bash
# Generate Tamzen bitmap fonts at multiple sizes
TAMZEN_SIZES="5x9 6x12 7x13 7x14 8x15 8x16 10x20"
mkdir -p build/host/fonts
for size in $TAMZEN_SIZES; do
    height=$(echo $size | cut -d'x' -f2)
    bdf="fonts/tamzen/Tamzen${size}r.bdf"
    out="build/host/fonts/tamzen-${height}.png"
    if needs_rebuild "$out" "$bdf" scripts/generate_font.py; then
        echo "Generating Tamzen ${size} font..."
        python3 scripts/generate_font.py --bdf "$bdf" --deploy-png "$out"
    fi
done
```

**Step 2: Verify build generates fonts**

```bash
./scripts/build.sh  # Should generate build/host/fonts/tamzen-*.png
ls build/host/fonts/
```

**Step 3: Commit**

```bash
git add scripts/build.sh
git commit -m "feat: generate Tamzen fonts during build"
```

---

### Task 4: Add set_font() and get_font_height() JS bindings

**Files:**
- Modify: `src/host/js_display.c`
- Modify: `src/host/js_display.h`

**Step 1: Add C functions and JS bindings**

In `js_display.c`, add after the existing print/text_width functions:

```c
/* Font switching */
int js_display_set_font(const char *path) {
    Font *new_font = js_display_load_font(path, 1);
    if (!new_font) return 0;
    /* Don't free g_font — keep it cached? For now, just swap. */
    g_font = new_font;
    return 1;
}

int js_display_get_font_height(void) {
    if (!g_font) {
        g_font = js_display_load_font("/data/UserData/schwung/host/font.png", 1);
    }
    if (!g_font) return 0;
    if (g_font->is_ttf) return g_font->ttf_height;
    /* For bitmap fonts, return height from first non-empty char */
    for (int i = 0; i < 256; i++) {
        if (g_font->charData[i].data) return g_font->charData[i].height;
    }
    return 0;
}
```

Add JS binding functions:

```c
JSValue js_display_bind_set_font(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewBool(ctx, 0);
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NewBool(ctx, 0);
    int result = js_display_set_font(path);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, result);
}

JSValue js_display_bind_get_font_height(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, js_display_get_font_height());
}
```

Register in `js_display_register_bindings()`:

```c
JS_SetPropertyStr(ctx, global_obj, "set_font",
    JS_NewCFunction(ctx, js_display_bind_set_font, "set_font", 1));
JS_SetPropertyStr(ctx, global_obj, "get_font_height",
    JS_NewCFunction(ctx, js_display_bind_get_font_height, "get_font_height", 0));
```

**Step 2: Add declarations to js_display.h**

```c
int js_display_set_font(const char *path);
int js_display_get_font_height(void);
JSValue js_display_bind_set_font(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_display_bind_get_font_height(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
```

**Step 3: Commit**

```bash
git add src/host/js_display.c src/host/js_display.h
git commit -m "feat: add set_font() and get_font_height() JS bindings"
```

---

### Task 5: Create the ui-test tool module

**Files:**
- Create: `src/modules/ui-test/module.json`
- Create: `src/modules/ui-test/ui.js`

**Step 1: Create module.json**

```json
{
    "id": "ui-test",
    "name": "UI Test",
    "version": "0.0.1",
    "description": "Live UI layout prototyping tool",
    "component_type": "tool",
    "standalone": true
}
```

**Step 2: Create ui.js**

The module has one page (expandable later) with 8 knobs controlling:

| Knob | CC | Parameter | Range |
|------|-----|-----------|-------|
| 1 | 71 | Header font size | Cycle through [9,12,13,14,15,16,20] |
| 2 | 72 | Header Y position | 0-20 |
| 3 | 73 | Divider rule Y | 5-30 |
| 4 | 74 | Menu font size | Cycle through [9,12,13,14,15,16,20] |
| 5 | 75 | Menu top Y | 10-50 |
| 6 | 76 | Line height | 6-25 |
| 7 | 77 | Visible item count | 1-8 |
| 8 | 78 | Selected item index | 0-(count-1) |

Status overlay: shows "Param: value" for 1 second after any knob change, drawn at bottom of screen in the default (small) font.

The module switches fonts before drawing the header vs menu items, then switches back to the default font for the status overlay.

Font paths on device: `/data/UserData/schwung/host/fonts/tamzen-{9,12,13,14,15,16,20}.png`
Default font: `/data/UserData/schwung/host/font.png`

```javascript
import { shouldFilterMessage } from '../../shared/input_filter.mjs';

const FONT_SIZES = [9, 12, 13, 14, 15, 16, 20];
const FONT_BASE = "/data/UserData/schwung/host/fonts/tamzen-";
const DEFAULT_FONT = "/data/UserData/schwung/host/font.png";
const DUMMY_ITEMS = ["Preset 1", "Strings Pad", "Bass Lead", "Warm Keys", "Pluck Arp", "Bright Saw", "Sub Bass", "Pad Wash"];

/* Parameters */
let headerFontIdx = 2;    // 13px
let headerY = 2;
let ruleY = 12;
let menuFontIdx = 0;      // 9px
let menuTopY = 15;
let lineHeight = 10;
let visibleCount = 5;
let selectedItem = 0;

/* Status overlay */
let statusText = "";
let statusTimeout = 0;
const STATUS_DURATION = 44; // ~1 second at 44 ticks/sec

function showStatus(name, value) {
    statusText = `${name}: ${value}`;
    statusTimeout = STATUS_DURATION;
}

function fontPath(idx) {
    return FONT_BASE + FONT_SIZES[idx] + ".png";
}

function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

globalThis.init = function() {
    drawScreen();
};

globalThis.tick = function() {
    if (statusTimeout > 0) {
        statusTimeout--;
    }
    drawScreen();
};

function drawScreen() {
    clear_screen();

    // Draw header in header font
    set_font(fontPath(headerFontIdx));
    print(2, headerY, "Header Text", 1);

    // Divider rule
    fill_rect(0, ruleY, 128, 1, 1);

    // Draw menu items in menu font
    set_font(fontPath(menuFontIdx));
    const count = Math.min(visibleCount, DUMMY_ITEMS.length);
    let startIdx = 0;
    if (selectedItem > count - 2) {
        startIdx = selectedItem - (count - 2);
    }

    for (let i = 0; i < count; i++) {
        const idx = startIdx + i;
        if (idx >= DUMMY_ITEMS.length) break;
        const y = menuTopY + i * lineHeight;
        const isSelected = idx === selectedItem;
        const prefix = isSelected ? "> " : "  ";

        if (isSelected) {
            const fontH = get_font_height();
            fill_rect(0, y - 1, 128, fontH + 2, 1);
            print(4, y, prefix + DUMMY_ITEMS[idx], 0);
        } else {
            print(4, y, prefix + DUMMY_ITEMS[idx], 1);
        }
    }

    // Status overlay (in default small font)
    if (statusTimeout > 0 && statusText) {
        set_font(DEFAULT_FONT);
        const textW = text_width(statusText);
        const boxW = textW + 8;
        const boxX = Math.floor((128 - boxW) / 2);
        const boxY = 52;
        fill_rect(boxX, boxY, boxW, 12, 0);
        draw_rect(boxX, boxY, boxW, 12, 1);
        print(boxX + 4, boxY + 2, statusText, 1);
    }
}

globalThis.onMidiMessageInternal = function(data) {
    if (!data || data.length < 3) return;
    if (shouldFilterMessage(data)) return;

    const status = data[0] & 0xF0;
    const cc = data[1];
    const val = data[2];

    // Back button
    if (status === 0xB0 && cc === 51 && val > 0) {
        set_font(DEFAULT_FONT); // Restore default before exit
        host_exit_module();
        return;
    }

    // Knobs (CC 71-78)
    if (status === 0xB0 && cc >= 71 && cc <= 78) {
        const knob = cc - 71;
        handleKnob(knob, val);
    }
};

function handleKnob(knob, val) {
    const norm = val / 127;

    switch (knob) {
        case 0: { // Header font size
            const idx = clamp(Math.round(norm * (FONT_SIZES.length - 1)), 0, FONT_SIZES.length - 1);
            if (idx !== headerFontIdx) {
                headerFontIdx = idx;
                showStatus("Hdr Font", FONT_SIZES[idx] + "px");
            }
            break;
        }
        case 1: { // Header Y
            const v = clamp(Math.round(norm * 20), 0, 20);
            if (v !== headerY) { headerY = v; showStatus("Hdr Y", v); }
            break;
        }
        case 2: { // Rule Y
            const v = clamp(Math.round(norm * 25 + 5), 5, 30);
            if (v !== ruleY) { ruleY = v; showStatus("Rule Y", v); }
            break;
        }
        case 3: { // Menu font size
            const idx = clamp(Math.round(norm * (FONT_SIZES.length - 1)), 0, FONT_SIZES.length - 1);
            if (idx !== menuFontIdx) {
                menuFontIdx = idx;
                showStatus("Menu Font", FONT_SIZES[idx] + "px");
            }
            break;
        }
        case 4: { // Menu top Y
            const v = clamp(Math.round(norm * 40 + 10), 10, 50);
            if (v !== menuTopY) { menuTopY = v; showStatus("Menu Y", v); }
            break;
        }
        case 5: { // Line height
            const v = clamp(Math.round(norm * 19 + 6), 6, 25);
            if (v !== lineHeight) { lineHeight = v; showStatus("Line H", v); }
            break;
        }
        case 6: { // Visible count
            const v = clamp(Math.round(norm * 7 + 1), 1, 8);
            if (v !== visibleCount) { visibleCount = v; showStatus("Items", v); }
            break;
        }
        case 7: { // Selected item
            const maxIdx = DUMMY_ITEMS.length - 1;
            const v = clamp(Math.round(norm * maxIdx), 0, maxIdx);
            if (v !== selectedItem) { selectedItem = v; showStatus("Sel", v); }
            break;
        }
    }
}
```

**Step 3: Commit**

```bash
git add src/modules/ui-test/
git commit -m "feat: add ui-test tool module for layout prototyping"
```

---

### Task 6: Build, deploy, and test

**Step 1: Build**

```bash
./scripts/build.sh
```

Verify:
- `build/host/fonts/tamzen-*.png` files exist (7 sizes)
- No compilation errors from js_display.c changes

**Step 2: Deploy**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Test on device**

1. Enter shadow mode (Shift+Vol+Track1)
2. Open Tools menu (Shift+Vol+Step13)
3. Select "UI Test"
4. Turn knobs — verify header/menu font sizes change, layout parameters adjust
5. Verify status overlay appears for ~1 second then fades
6. Press Back to exit

**Step 4: Commit any fixes**
