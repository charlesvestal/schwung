# scan_packs: Auto-discover module packs

## Problem

RNBO synth/FX packs (`.rnbopack` files) need to appear as individual module entries in the menu. Currently, one module.json = one menu entry. Users want to drop a pack file into a directory and have it show up automatically.

## Design

### module.json addition

```json
{
  "id": "rnbo-synth",
  "name": "RNBO Synth",
  "dsp": "dsp.so",
  "scan_packs": "packs/",
  "component_type": "sound_generator"
}
```

When `scan_packs` is present, the base module entry is **hidden**. Instead, the scanner creates a virtual module entry for each `.rnbopack` file found in the specified subdirectory.

### Virtual module entries

For each pack, a `module_info_t` is created:
- `id` = `"{parent_id}-{pack_filename}"` (sans extension)
- `name` = extracted from pack's set JSON (cached in `.name` sidecar)
- `dsp_path` = same as parent module's dsp.so
- `ui_script` = same as parent module's ui.js
- `module_dir` = same as parent module directory
- `defaults_json` = `{"pack": "/full/path/to/pack.rnbopack"}`
- `component_type` = inherited from parent
- All capabilities inherited from parent

### Name extraction and caching

On first scan, extract the graph name from the `.rnbopack` tarball's set JSON. Cache to a `.name` sidecar file (e.g. `FM-Synth.rnbopack.name`). Subsequent scans read the sidecar. If sidecar is missing or older than the pack, re-extract.

### Scan timing

Packs are scanned on every menu open (rescanning is just readdir + check sidecar files — sub-millisecond). No caching of the scan results themselves.

### Changes required

1. **module_manager.c** (~40 lines):
   - `parse_module_json()`: parse `scan_packs` field
   - New `scan_packs_dir()`: iterate `.rnbopack` files, create virtual entries
   - `scan_directory()`: call `scan_packs_dir()` when field is present, skip base entry

2. **module_manager.h** (~2 lines):
   - Add `scan_packs` field to `module_info_t`
   - Increase `MAX_MODULES` from 32 to 64

3. **No menu changes** — virtual entries are normal `module_info_t`, menu handles them identically.

4. **No DSP API changes** — pack path arrives via existing `defaults_json`.
