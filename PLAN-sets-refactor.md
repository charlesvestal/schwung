# Plan: Refactor sets.json to Individual Set Files

**Status: IMPLEMENTED**

## Current State

- Single file: `/data/UserData/move-anything-data/sequencer/sets.json`
- Contains array of 32 sets (large and growing)
- All sets loaded into memory at startup
- Entire file rewritten on every save

## Target State

- Individual files: `/data/UserData/move-anything-data/sequencer/sets/0.json` through `31.json`
- File exists = set has data; file missing = empty set
- Only load set data when needed
- Only write the specific set that changed

---

## Architecture Reference

```
src/modules/sequencer/
  ui.js                    # Main router - tick(), init(), onMidiMessage()
  lib/
    constants.js           # NUM_TRACKS=16, NUM_PATTERNS=16, NUM_SETS=32
    state.js               # All mutable state + view transitions
    helpers.js             # Utility functions
    data.js                # Track/pattern/step structures, migration, transpose
    persistence.js         # Save/load sets to disk (MAIN TARGET)
  views/
    set.js                 # Set selection (32 sets on pads)
    track.js               # Track view coordinator
    pattern.js             # Pattern selection grid
    master.js              # Master settings (transpose, chord follow)
    track/
      normal.js            # Main step editing mode
      loop.js              # Loop start/end editing
      spark.js             # Spark conditions
      channel.js           # MIDI channel adjustment
      speed.js             # Track speed multiplier
      swing.js             # Track swing amount
      shared.js            # Common re-exports
```

**Key Constants (constants.js):**
- `NUM_TRACKS = 16`
- `NUM_PATTERNS = 16`
- `NUM_STEPS = 16`
- `NUM_SETS = 32`

**View/Mode System (state.js):**
- `state.view`: `'set'` | `'track'` | `'pattern'` | `'master'`
- `state.trackMode`: `'normal'` | `'loop'` | `'spark'` | `'swing'` | `'bpm'` | `'speed'` | `'channel'`

**State for 16-track navigation:**
- `state.trackScrollPosition` - Scroll position (0-12)
- `state.chordFollow[16]` - Per-track transpose enable

---

## Current Set Data Format

Each set contains (from persistence.js `saveCurrentSet`):

```javascript
{
    tracks: [...],              // 16 tracks × 16 patterns × 16 steps
    bpm: 120,
    transposeSequence: [...],   // Global transpose sequence (max 16 steps)
    chordFollow: [...],         // 16 booleans - which tracks follow transpose
    sequencerType: 0            // Extensible
}
```

**Track structure (from data.js):**
```javascript
{
    patterns: [...],            // 16 patterns
    currentPattern: 0,
    muted: false,
    channel: 0,                 // MIDI channel (0-15)
    speedIndex: 4,              // Index into SPEED_OPTIONS
    swing: 50                   // 0-100
}
```

**Pattern structure:**
```javascript
{
    steps: [...],               // 16 steps
    loopStart: 0,
    loopEnd: 15
}
```

---

## Files to Modify

### 1. `src/modules/sequencer/lib/constants.js`

**Current (line 27-28):**
```javascript
export const DATA_DIR = '/data/UserData/move-anything-data/sequencer';
export const SETS_FILE = DATA_DIR + '/sets.json';
```

**Add:**
```javascript
export const SETS_DIR = DATA_DIR + '/sets';
```

### 2. `src/modules/sequencer/lib/persistence.js`

**Current functions:**
- `saveAllSetsToDisk()` - Writes entire `state.sets` array
- `loadAllSetsFromDisk()` - Reads entire sets.json
- `saveCurrentSet()` - Copies tracks/bpm/transpose/chordFollow/sequencerType to `state.sets[currentSet]`
- `loadSetToTracks(setIdx)` - Loads from `state.sets`, handles 8→16 track migration
- `setHasContent(setIdx)` - Scans for notes/CC values

**New functions:**

| Function | Purpose |
|----------|---------|
| `ensureSetsDir()` | Create `sets/` directory |
| `saveSetToDisk(setIdx)` | Write single set to `sets/{idx}.json` |
| `loadSetFromDisk(setIdx)` | Read single set from file |
| `saveCurrentSetToDisk()` | Helper: update in-memory + write to disk |
| `setFileExists(setIdx)` | Quick file existence check |
| `listPopulatedSets()` | Return indices with existing files |
| `deleteSetFile(setIdx)` | Remove set file |
| `migrateFromLegacy()` | One-time migration from sets.json |

### 3. `src/modules/sequencer/lib/state.js`

**No changes needed** - save-on-change doesn't require dirty tracking.

### 4. `src/modules/sequencer/ui.js`

**Current init() (line 86-88):**
```javascript
initializeSets();
loadAllSetsFromDisk();
```

**Changes:**
- Replace with `migrateFromLegacy()` + `ensureSetsDir()`
- Remove bulk load

**No changes to tick()** - save happens immediately on each edit.

### 5. `src/modules/sequencer/views/set.js`

**Changes:**
- Use `setFileExists(setIdx)` for LED display
- Update save calls to use single-set functions

### 6. `src/modules/sequencer/views/pattern.js`

**Changes:**
- Replace `saveAllSetsToDisk()` with `saveSetToDisk(state.currentSet)`

### 7. `scripts/backup.sh` and `scripts/restore.sh`

**No changes needed** - Already back up/restore entire data directory.

---

## Current Save Point Problem

**Sets only save to disk at two points:**
1. `views/set.js` - When selecting a different set
2. `views/pattern.js` - When going to set view (Shift+Step1)

**Never saves:** Edits in normal mode, loop mode, spark mode, channel/speed/swing modes, note entry, BPM changes, transpose changes.

**Risk:** Data loss on crash, module unload, or device power off.

### Save-on-Change Solution

Add helper function to `persistence.js`:
```javascript
export function saveCurrentSetToDisk() {
    if (state.currentSet < 0) return;
    saveCurrentSet();           // Update in-memory
    saveSetToDisk(state.currentSet);  // Write to disk
}
```

**Call `saveCurrentSetToDisk()` in these locations:**

| File | Trigger |
|------|---------|
| `views/track/normal.js` | Note add/remove, CC changes, step parameters, ratchet, probability, length, offset |
| `views/track/loop.js` | Loop start/end changes |
| `views/track/spark.js` | Spark parameter changes (paramSpark, compSpark, jump) |
| `views/track/channel.js` | MIDI channel changes |
| `views/track/speed.js` | Speed multiplier changes |
| `views/track/swing.js` | Swing amount changes |
| `views/pattern.js` | Pattern selection changes |
| `views/master.js` | Transpose sequence, chord follow changes |
| `ui.js` or wherever BPM is set | BPM changes |

With individual ~30-50KB files, save-on-change should be fast enough (no noticeable lag).

---

## Migration Strategy

```javascript
export function migrateFromLegacy() {
    const oldFile = DATA_DIR + '/sets.json';

    try {
        const content = std.loadFile(oldFile);
        if (!content) return false;

        // Check if already migrated
        ensureSetsDir();
        if (listPopulatedSets().length > 0) {
            console.log('Migration skipped - sets/ already has files');
            return false;
        }

        // Parse old format
        const allSets = JSON.parse(content);
        if (!Array.isArray(allSets)) return false;

        // Write each non-empty set
        let migrated = 0;
        for (let i = 0; i < allSets.length; i++) {
            if (allSets[i] && setDataHasContent(allSets[i])) {
                saveSetToDisk(i, allSets[i]);
                migrated++;
            }
        }

        // Rename old file
        os.rename(oldFile, oldFile + '.backup');
        console.log(`Migrated ${migrated} sets to individual files`);
        return true;
    } catch (e) {
        console.log('Migration failed: ' + e);
        return false;
    }
}
```

---

## New File Format

Each file `sets/{idx}.json`:
```json
{
  "tracks": [
    {
      "patterns": [...],
      "currentPattern": 0,
      "muted": false,
      "channel": 0,
      "speedIndex": 4,
      "swing": 50
    }
  ],
  "bpm": 120,
  "transposeSequence": [
    { "transpose": 0, "duration": 4 },
    { "transpose": 5, "duration": 4 }
  ],
  "chordFollow": [false, false, false, false, true, true, true, true,
                  false, false, false, false, true, true, true, true],
  "sequencerType": 0
}
```

---

## Implementation Order

1. Add `SETS_DIR` constant to constants.js
2. Implement `ensureSetsDir()`, `setFileExists()`, `listPopulatedSets()`
3. Implement `saveSetToDisk(idx)`, `loadSetFromDisk(idx)`, `saveCurrentSetToDisk()`
4. Update `loadSetToTracks()` to use new load function
5. Update `setHasContent()` to use file existence check
6. Update `set.js` and `pattern.js` save calls
7. Add `saveCurrentSetToDisk()` calls in all edit locations
8. Implement `migrateFromLegacy()`
9. Update `init()` to call migration and ensure directory
10. Test on device
11. Remove deprecated `saveAllSetsToDisk()`, `loadAllSetsFromDisk()`

---

## Edge Cases

| Case | Handling |
|------|----------|
| Empty set selected | Create file on first edit (save-on-change) |
| Set cleared | Delete file via `deleteSetFile(idx)` |
| Corrupt file | Log error, treat as empty, don't crash |
| Disk full | Log error, show warning on display |
| File read during write | Use tmp file + rename for atomic writes |
| 8→16 track migration | Handled by existing `migrateTrackData()` |
| 8→16 chordFollow migration | Handled by existing `loadSetToTracks()` |

---

## Benefits

- Faster startup (no large JSON parse)
- Faster saves (~30-50KB per set vs entire file)
- Zero data loss risk (save on every change)
- Easier debugging (view individual set files)
- Foundation for per-set backup/restore
- Lower memory usage (sparse set loading)
- Simpler code (no dirty tracking needed)
