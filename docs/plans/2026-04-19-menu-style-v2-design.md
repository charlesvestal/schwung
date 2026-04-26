# Menu Style v2 — Design

**Date:** 2026-04-19
**Source:** UI test capture `capture_2.json` (tamzen-9px header, tamzen-15px list)
**Goal:** Adopt the larger-font menu layout from `ui-test` capture as the default for all menus, behind a feature flag for safe rollout.

## Problem

`src/shared/menu_layout.mjs` hardcodes the classic 5×7 font layout: `LIST_LINE_HEIGHT=9`, `LIST_TOP_Y=15`, `TITLE_RULE_Y=12`, `LIST_HIGHLIGHT_OFFSET=1`, and assumes `DEFAULT_CHAR_WIDTH=6`. The captured v2 style uses tamzen-9px for headers and tamzen-15px for list items with different spacing (`top_y=6`, `line_height=11`, `highlight.padding=-2`, `highlight.y_offset=3`).

We want to ship v2 as the default eventually, but flip safely with rollback.

## Bucket summary (from module audit)

- **Bucket A — auto-updates (16 modules):** call `drawMenuList`/`drawMenuHeader`/`drawHierarchicalMenu`/`drawStackMenu`. Built-ins (host menus, shadow UI, chain, store, song-mode, freeverb chain) plus 9 external modules (control, radiogarden, webstream, fourtrack, stretch, samplerobot/autosample, sidcontrol, airplay). Plus all 38L synth stubs (jv880, hera, moog, nusaw, obxd, surge, virus, sf2, sfz, dx7, rex, hush1) which render via Chain → covered by Chain's flip.
- **Bucket B — needs migration:** controller, rnbo-runner, file-browser (via `filepath_browser.mjs`), several overtake modules. Out of scope for v1.
- **Bucket C — bespoke:** m8, signalscope, performance-fx, waveform-editor. Stay classic indefinitely.

## Design

### Feature flag

- New flag: `menu_style_v2` in `/data/UserData/schwung/config/features.json`
- Default: `false` initially; flip to `true` after on-device validation
- C-side setter `menu_style_set(v2)` in `shadow_ui.c`, mirroring the existing `display_mirror_set` / `set_pages_set` / `shadow_ui_trigger_set` patterns
- Setting UI: one new line in Global Settings (`Shift+Vol+Step2`), "Menu Style: Classic / V2"

### Read path

- `menu_layout.mjs` calls `host_read_file('/data/UserData/schwung/config/features.json')` once at module load
- Parses, caches the boolean
- Helpers branch on the cached value
- No per-frame I/O, no per-call parsing
- Toggling takes effect on next module entry (no live reload mid-screen)

### What flips with the flag

When `menu_style_v2 === true`:

| Constant | Classic | V2 |
|---|---|---|
| Header font | (caller's) | `tamzen-9.png` |
| List font | (caller's) | `tamzen-15.png` |
| `TITLE_Y` | 2 | -1 |
| `TITLE_RULE_Y` | 12 | 7 |
| `LIST_TOP_Y` | 15 | 6 |
| `LIST_LINE_HEIGHT` | 9 | 11 |
| `LIST_HIGHLIGHT_OFFSET` | 1 | 3 |
| Highlight extra padding | 0 | -2 (shrink) |

`drawMenuHeader` and `drawMenuList` call `set_font()` themselves under v2 and restore the previous font before returning, so callers that draw their own pre/post content keep their font.

### Char-width fix

The 6 hardcoded `DEFAULT_CHAR_WIDTH = 6` references inside `drawMenuList` (lines 167, 171, 177, 187, 191, 211-212) all assume monospace 6px glyphs. Tamzen-15px is 7px wide, breaking truncation and right-align math.

Fix under v2 only:

```js
const charW = text_width("M") || DEFAULT_CHAR_WIDTH;
// replace every DEFAULT_CHAR_WIDTH with charW inside the v2 branch
```

Tamzen is monospace at every size, so a single-glyph probe is exact (tamzen-9=5px, tamzen-15=7px, tamzen-20=10px).

Classic path stays byte-identical — zero regression risk to existing screens.

### What does NOT flip in v1

- `filepath_browser.mjs`, `text_entry.mjs`, `sampler_overlay.mjs` — bespoke layouts
- Parameter overlay, status overlay, message overlay (`menu_layout.mjs:267-434`) — modal popups with hardcoded coords
- Hand-rolled module screens (controller, m8, performance-fx, waveform-editor)

Visual mismatch is the trade-off. Migrating these is a follow-up.

## Rollout & validation

1. Build with flag default `false`. Deploy. Verify zero visual change.
2. SSH to device, flip flag in `features.json`, restart `shadow_ui` (auto-restarts on kill).
3. Walk every Bucket A screen: host menu, settings, 4 shadow slots, patches, master FX, store, tools, song mode, chain UI, controller bank.
4. Capture before/after via the `ui-test` module's capture flow.
5. If correct, flip default to `true` in code. If wrong, fix and redeploy. Classic branch unchanged until then.

**Rollback:** edit `features.json` `menu_style_v2` back to `false`, restart `shadow_ui`. No reinstall.

## Files touched

| File | Change |
|---|---|
| `src/shared/menu_layout.mjs` | Add v2 branch, font setting, char-width fix, flag cache |
| `src/shadow/shadow_ui.c` | New `menu_style_set()` setter, mirror SHM, persist features.json |
| `src/shadow/shadow_ui.js` | Read flag at startup, expose to settings menu |
| `src/shadow/shadow_ui_settings.mjs` | Add toggle line |

## Open follow-ups (not in v1)

- Migrate `filepath_browser.mjs` to use shared helpers (knocks out file-browser + waveform-editor's file dialog)
- Migrate sampler/text_entry overlays to v2 fonts when ready
- Add per-module `menu_style` capability override if needed
