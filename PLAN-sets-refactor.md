# Plan: Refactor sets.json to Individual Set Files

## Current State

- Single file: `/data/UserData/move-anything-data/sequencer/sets.json`
- Contains array of 32 sets (~1MB and growing)
- All sets loaded into memory at startup
- Entire file rewritten on every save

## Target State

- Individual files: `/data/UserData/move-anything-data/sequencer/sets/0.json` through `31.json`
- File exists = set has data; file missing = empty set
- Only load set data when needed
- Only write the specific set that changed

---

## Files to Modify

### 1. `src/modules/sequencer/lib/constants.js`

**Changes:**
- Add `SETS_DIR = DATA_DIR + '/sets'`
- Remove or deprecate `SETS_FILE`

### 2. `src/modules/sequencer/lib/persistence.js`

**Replace functions:**

| Old Function | New Function | Behavior |
|--------------|--------------|----------|
| `saveAllSetsToDisk()` | `saveSetToDisk(setIdx)` | Saves single set to `sets/{idx}.json` |
| `loadAllSetsFromDisk()` | `loadSetFromDisk(setIdx)` | Loads single set from file |
| `saveCurrentSet()` | Keep as-is | Still updates in-memory state |

**New functions:**
- `ensureSetsDir()` - Creates `/data/UserData/move-anything-data/sequencer/sets/` if missing
- `deleteSet(setIdx)` - Removes set file (for clearing a set)
- `listPopulatedSets()` - Returns array of set indices that have files (for LED display)
- `migrateFromLegacy()` - One-time migration from old sets.json

**Updated logic:**
- `setHasContent()` - Can check file existence instead of scanning data
- `loadSetToTracks()` - Calls `loadSetFromDisk()` on demand

### 3. `src/modules/sequencer/lib/state.js`

**Changes:**
- `state.sets` no longer pre-populated with 32 entries
- Sets loaded lazily into `state.sets[idx]` when accessed
- Add `state.setsLoaded` bitmap or Set to track which are in memory

### 4. `src/modules/sequencer/ui.js`

**Changes in `init()`:**
- Remove `loadAllSetsFromDisk()` call at startup
- Add `migrateFromLegacy()` call (one-time, checks if old sets.json exists)
- Ensure sets directory exists

### 5. `src/modules/sequencer/views/set.js`

**Changes:**
- Update LED coloring logic to use `listPopulatedSets()` or check file existence
- Save/load calls updated to use new single-set functions

### 6. `scripts/backup.sh`

**No changes needed** - Already backs up entire `/data/UserData/move-anything-data/` directory, which will include the new `sequencer/sets/` folder.

### 7. `scripts/restore.sh`

**No changes needed** - Restores entire data directory. New structure will be restored correctly.

**Optional enhancement:** Add validation that restored data is compatible with current version.

---

## Migration Strategy

### Automatic Migration (in `persistence.js`)

```javascript
function migrateFromLegacy() {
    // Check if old sets.json exists
    const oldFile = DATA_DIR + '/sets.json';
    if (!fileExists(oldFile)) return;

    // Check if already migrated (sets/ dir has files)
    if (setsDirectoryHasFiles()) return;

    // Read old format
    const allSets = JSON.parse(readFile(oldFile));

    // Write each non-empty set to individual file
    for (let i = 0; i < allSets.length; i++) {
        if (allSets[i] && setDataHasContent(allSets[i])) {
            saveSetToDisk(i, allSets[i]);
        }
    }

    // Rename old file to sets.json.backup
    renameFile(oldFile, oldFile + '.backup');

    console.log('Migrated sets.json to individual files');
}
```

---

## New File Format

Each set file (`sets/0.json`, etc.) contains a single set object:

```json
{
  "tracks": [...],
  "bpm": 120
}
```

No wrapper array, no set index in file (index is the filename).

---

## Implementation Order

1. **Add new constants** - `SETS_DIR` in constants.js
2. **Add helper functions** - `ensureSetsDir()`, file existence checks
3. **Implement new save/load** - `saveSetToDisk(idx)`, `loadSetFromDisk(idx)`
4. **Update state management** - Lazy loading in state.js
5. **Update set view** - LED logic for populated sets
6. **Add migration** - `migrateFromLegacy()` in init
7. **Test on device** - Verify save/load/migration works
8. **Clean up** - Remove deprecated code

---

## Edge Cases

- **Empty set selected:** Create file on first note entry, or don't create until explicit save
- **Set cleared by user:** Delete the file
- **Corrupt file:** Log error, treat as empty set, don't crash
- **Disk full:** Handle write errors gracefully, warn user via display

---

## Current Save Point Problem

**Current behavior:** Sets only save to disk at two points:
1. `set.js:58-60` - When selecting a different set in set view
2. `pattern.js:56-60` - When going from pattern view back to set view (Shift+Step1)

**Problem:** Edits in step view, track view, notes, patterns, BPM changes - none of these trigger a save. Data loss on crash or exit.

### Proposed Save Strategy

**Option A: Periodic auto-save (recommended)**
- Save current set every N seconds if dirty (e.g., 30 seconds)
- Add `state.setDirty` flag, set true on any edit
- Timer in tick() checks flag and saves if needed
- With individual files, saving one set is fast

**Option B: Save on every edit**
- Too aggressive, may cause lag with frequent edits
- Not recommended even with smaller files

**Option C: Save on view exit**
- Save when leaving step view, track view, etc.
- Better but still gaps (what if you stay in step view for an hour?)

**Implementation for Option A:**
```javascript
// In state.js
state.setDirty = false;
state.lastSaveTime = 0;

// In tick() or a dedicated save check
const SAVE_INTERVAL_MS = 30000;  // 30 seconds
if (state.setDirty && (now - state.lastSaveTime) > SAVE_INTERVAL_MS) {
    saveCurrentSet();
    saveSetToDisk(state.currentSet);
    state.setDirty = false;
    state.lastSaveTime = now;
}

// Mark dirty on edits (in step editing, note entry, etc.)
state.setDirty = true;
```

---

## Benefits

- Faster startup (no 1MB JSON parse)
- Faster saves (only affected set written)
- Smaller file operations
- Easier to see which sets exist (file browser)
- Can backup/restore individual sets in future
