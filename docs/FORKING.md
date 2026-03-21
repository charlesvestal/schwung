# Forking Schwung

Guide for maintaining a fork of Schwung that tracks upstream while adding custom features.

## Flag Allocation

### ui_flags (shadow_control_t)

The `ui_flags` field is a single `uint8_t` (8 bits) in `shadow_control_t`. All 8 bits are currently allocated by upstream:

| Bit | Mask | Flag | Purpose |
|-----|------|------|---------|
| 0 | 0x01 | JUMP_TO_SLOT | Jump to slot settings on open |
| 1 | 0x02 | JUMP_TO_MASTER_FX | Jump to Master FX on open |
| 2 | 0x04 | JUMP_TO_OVERTAKE | Jump to overtake module menu |
| 3 | 0x08 | SAVE_STATE | Save all state (shutdown imminent) |
| 4 | 0x10 | JUMP_TO_SCREENREADER | Jump to screen reader settings |
| 5 | 0x20 | SET_CHANGED | Set changed, reload slot state |
| 6 | 0x40 | JUMP_TO_SETTINGS | Jump to Global Settings |
| 7 | 0x80 | JUMP_TO_TOOLS | Jump to Tools menu |

### Fork extension flags

Forks that need additional flags should use the `reserved16` field in `shadow_control_t` (currently at byte offset 10, after `ui_patch_index`). To use it as fork-specific UI flags:

```c
/* In your fork's header */
#define FORK_UI_FLAG_MY_FEATURE  0x0001  /* Bit 0 of reserved16 */
#define FORK_UI_FLAG_OTHER       0x0002  /* Bit 1 of reserved16 */

/* Read/write via shadow_control_t */
uint16_t fork_flags = control->reserved16;
control->reserved16 |= FORK_UI_FLAG_MY_FEATURE;
```

This gives forks 16 additional flag bits without modifying the upstream struct layout.

## Struct Layout Rules

`shadow_control_t` must be exactly `CONTROL_BUFFER_SIZE` (64) bytes. It is mapped into shared memory between the shim and shadow UI processes.

### Rules

1. **Never shrink the struct** - both shim and shadow UI map the same shared memory
2. **Add new fields by consuming `reserved` bytes from the end** - the `reserved[19]` array at the end is the expansion area
3. **Keep byte alignment in mind** - `uint16_t` fields need 2-byte alignment, `uint32_t` and `float` need 4-byte alignment
4. **Never reorder existing fields** - the shim and shadow UI may be different versions during an upgrade

### Example: adding a new field

```c
/* BEFORE: */
volatile uint8_t set_pages_enabled;
volatile uint8_t reserved[19];  /* 19 bytes available */

/* AFTER: consuming 1 byte from reserved */
volatile uint8_t set_pages_enabled;
volatile uint8_t my_new_field;     /* New field */
volatile uint8_t reserved[18];     /* 18 bytes remaining */
```

For multi-byte fields, consume from the end and adjust alignment:
```c
volatile uint8_t set_pages_enabled;
volatile uint8_t reserved[15];     /* Padding for alignment */
volatile uint32_t my_new_counter;  /* New 4-byte field at end */
```

## Build Script Conventions

### Labeled sections

Each compilation target in `scripts/build.sh` should be in a labeled section with a descriptive echo and comment:

```bash
# Build My Custom Feature
echo "Building my custom feature..."
"${CROSS_PREFIX}gcc" -g -O3 \
    src/host/my_feature.c \
    src/host/unified_log.c \
    -o build/my-feature \
    -Isrc -Isrc/host \
    -lrt
```

### Common flags

Most targets need these flags:
- `-Isrc -Isrc/host` for header resolution
- `src/host/unified_log.c` for unified logging (or link `unified_log.o` for C++ targets)
- `-lrt` for shared memory (`shm_open`)

### Adding build targets in a fork

Place fork-specific targets in a clearly labeled block to simplify merge conflicts:

```bash
# === Fork: Jupiter-specific targets ===

echo "Building companion server..."
"${CROSS_PREFIX}gcc" -g -O3 \
    src/host/companion_server.c \
    -o build/companion-server

# === End fork-specific targets ===
```

## Store Category Extension

### Adding a custom category

1. Add to `CATEGORIES` in `src/shared/store_utils.mjs`:
```javascript
export const CATEGORIES = [
    // ... upstream categories ...
    { id: 'my_category', name: 'My Category' }
];
```

2. Add install directory mapping in `getInstallSubdir()`:
```javascript
case 'my_category': return 'my_category';
```

3. Add to `scripts/install.sh` module directory filter (the `case` that skips known directories):
```bash
chain|controller|store|...|my_category|other) continue ;;
```

4. Add to the awk catalog parser in `install.sh`:
```bash
else if (ctype == "my_category") subdir = "my_category"
```

## Shadow UI View System

### Architecture

The shadow UI (`src/shadow/shadow_ui.js`) is loaded as an ES module by `shadow_ui.c`. It uses QuickJS with full ES module support, enabling `import`/`export` with both absolute and relative paths.

Views are organized as modular `.mjs` files in `src/shadow/`:

```
src/shadow/
  shadow_ui.js              <- Core: state, routing, MIDI dispatch, tick/draw
  shadow_ui_slots.mjs       <- Slot settings, slot list views
  shadow_ui_patches.mjs     <- Patch browser, patch management
  shadow_ui_master_fx.mjs   <- Master FX chain view
  shadow_ui_tools.mjs       <- Tools menu, file browser, wav player
  shadow_ui_store.mjs       <- Module store views
  shadow_ui_settings.mjs    <- Host settings, global settings
  shadow_ui_overtake.mjs    <- Overtake module loading/lifecycle
```

### Import patterns

**Shared utilities** use absolute paths (resolved on device):
```javascript
import { drawMenuHeader } from '/data/UserData/schwung/shared/menu_layout.mjs';
```

**Local view modules** use relative paths:
```javascript
import { drawSlots, handleSlotsJog } from './shadow_ui_slots.mjs';
```

### View registration

Each view module exports:
- **View constants**: String identifiers for its views
- **Draw functions**: Called from `tick()` for rendering
- **Enter functions**: Called to transition into the view (set up state)
- **Handle functions**: Called from MIDI dispatchers for input processing

The core `shadow_ui.js` imports these and routes to them in its `tick()` and `onMidiMessageInternal()` dispatch switches.

### Adding a view in a fork

1. Create `src/shadow/shadow_ui_myview.mjs`:
```javascript
import { drawMenuHeader, drawMenuList, drawMenuFooter }
    from '/data/UserData/schwung/shared/menu_layout.mjs';

/* View-specific state */
let myIndex = 0;

/* View constant */
export const VIEW_MY_FEATURE = "myfeature";

export function enterMyFeature(ctx) {
    myIndex = 0;
    ctx.setView(VIEW_MY_FEATURE);
}

export function drawMyFeature(ctx) {
    clear_screen();
    drawMenuHeader("My Feature");
    // ... render using ctx for shared state
    drawMenuFooter("Back: exit");
}

export function handleMyFeatureJog(ctx, delta) {
    myIndex = Math.max(0, myIndex + delta);
}

export function handleMyFeatureSelect(ctx) {
    // handle jog click
}

export function handleMyFeatureBack(ctx) {
    ctx.setView(ctx.VIEWS.SLOTS);
}
```

2. Import in `shadow_ui.js`:
```javascript
import { VIEW_MY_FEATURE, drawMyFeature, enterMyFeature,
         handleMyFeatureJog, handleMyFeatureSelect, handleMyFeatureBack
       } from './shadow_ui_myview.mjs';
```

3. Add cases to the dispatch switches in core:
```javascript
// In tick() view switch:
case VIEW_MY_FEATURE: drawMyFeature(ctx); break;

// In handleJog():
case VIEW_MY_FEATURE: handleMyFeatureJog(ctx, delta); break;

// In handleSelect():
case VIEW_MY_FEATURE: handleMyFeatureSelect(ctx); break;

// In handleBack():
case VIEW_MY_FEATURE: handleMyFeatureBack(ctx); break;
```

4. Add the `.mjs` file to the shadow copy in `scripts/build.sh`:
```bash
# Copy shadow UI files
cp ./src/shadow/shadow_ui.js ./build/shadow/
cp ./src/shadow/*.mjs ./build/shadow/ 2>/dev/null || true
```

### The ctx object

View functions receive a `ctx` parameter providing access to shared state and utilities:

```javascript
ctx.view            // Current view string
ctx.setView(v)      // Change the current view
ctx.selectedSlot    // Currently focused slot (0-3)
ctx.slots           // Array of slot objects
ctx.getSlotParam(slot, key)    // Read a DSP parameter
ctx.setSlotParam(slot, key, v) // Write a DSP parameter
ctx.VIEWS           // All view constants
```

## Merge Strategy for Forks

### Reducing merge conflicts

1. **Keep fork changes in separate files** where possible (new .mjs modules, new C files)
2. **Use labeled blocks in build.sh** for fork-specific targets
3. **Add custom views as new .mjs files** instead of editing shadow_ui.js directly
4. **Use `reserved16` for custom flags** instead of modifying `ui_flags`
5. **Add custom categories** instead of repurposing existing ones

### Subtree-based forks

If your fork uses git subtree to embed move-anything:

```bash
# Add upstream as remote
git remote add upstream https://github.com/charlesvestal/move-everything.git

# Pull upstream changes
git subtree pull --prefix=move-anything-my-version upstream main --squash

# After pull, check for dropped build targets
diff <(grep -E '^\$\{CROSS_PREFIX\}g' move-anything-my-version/scripts/build.sh) \
     <(git show upstream/main:scripts/build.sh | grep -E '^\$\{CROSS_PREFIX\}g')
```

### Common issues after upstream merge

| Symptom | Likely Cause |
|---------|-------------|
| display-server / web-shim crash | Missing `unified_log.c` linkage in build.sh |
| Link subscriber build fails | Missing `unified_log.o` or `-Isrc -Isrc/host` flags |
| Tools crash or missing | WAV Player DSP not compiled, tools dir missing |
| Module Store can't install category | Missing category in store_utils.mjs or install.sh |
| New menu features not working | drawMenuList parameters dropped during merge |
