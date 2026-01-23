# Shadow UI Store Picker Design

## Summary

Add module store access from Shadow UI's module picker. When selecting instruments or effects for chain slots, users can choose "[Get more...]" to browse and install modules without leaving Shadow UI.

## Requirements

- Access store from module picker (synth, fx1, fx2, midiFx slots)
- Filter to relevant category only (sound_generator, audio_fx, midi_fx)
- Stay in Shadow UI throughout (audio keeps playing)
- Restricted navigation - back returns to module picker, not full store browsing

## Architecture

### Shared Utilities

**New file: `src/shared/store_utils.mjs`**

Extract from `store/ui.js`:
- `compareVersions(a, b)` - semver comparison
- `isNewerVersion(a, b)` - check if a > b
- `fetchReleaseJson(github_repo)` - get version/download_url from repo
- `getInstallSubdir(componentType)` - map type to directory
- `installModule(mod)` - download and extract, returns {success, error}
- `getModuleStatus(mod, installedModules)` - check installed/update status
- `fetchCatalog()` - fetch and cache catalog
- `loadCatalogFromCache()` - load cached catalog
- `getModulesForCategory(catalog, categoryId)` - filter by type

Constants:
- `CATALOG_URL`, `MODULES_DIR`, `TMP_DIR`, `CATALOG_CACHE_PATH`
- `CATEGORIES` array

### Shadow UI Changes

**New views:**
- `STORE_PICKER_LIST` - available modules for category
- `STORE_PICKER_DETAIL` - module info and install action
- `STORE_PICKER_LOADING` - catalog fetch or install in progress
- `STORE_PICKER_RESULT` - success/error after install

**Entry point:**
In `scanModulesForType()`, append to result:
```javascript
result.push({ id: "__get_more__", name: "[Get more...]" });
```

**State to track:**
- `storePickerCategory` - which category we're browsing
- `storePickerModules` - filtered catalog modules
- `storePickerSelectedIndex` - selection in list
- `storePickerCurrentModule` - module being viewed in detail
- `storeCatalog` - cached catalog (session lifetime)

### Navigation Flow

```
COMPONENT_SELECT
    ↓ select "[Get more...]"
STORE_PICKER_LOADING (fetch catalog if needed)
    ↓
STORE_PICKER_LIST (modules for this category)
    ↓ select module
STORE_PICKER_DETAIL (info + Install/Update/Remove)
    ↓ click Install
STORE_PICKER_LOADING (installing)
    ↓
STORE_PICKER_RESULT (success/error)
    ↓ click or timeout
STORE_PICKER_LIST
    ↓ Back
COMPONENT_SELECT (now includes newly installed module)
```

### Category Mapping

| Slot Type | Catalog Category |
|-----------|------------------|
| synth | sound_generator |
| fx1, fx2 | audio_fx |
| midiFx | midi_fx |

### Store Module Refactor

`src/modules/store/ui.js` imports shared utilities:
```javascript
import {
    compareVersions, isNewerVersion,
    fetchReleaseJson, getInstallSubdir,
    installModule, getModuleStatus,
    fetchCatalog, getModulesForCategory,
    CATALOG_URL, MODULES_DIR, CATEGORIES
} from '/data/UserData/move-anything/shared/store_utils.mjs';
```

Remove local definitions of these functions/constants.

## Error Handling

- Catalog fetch fails + no cache: show error, back to component select
- Catalog fetch fails + cache exists: use cache with warning
- Install fails: show error message, return to list
- Empty category: show "No modules available"

## Future Considerations

- Progress callbacks for download/install (if host supports it)
- Showing download size before install
- Module dependencies
