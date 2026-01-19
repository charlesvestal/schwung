# Shadow UI Test Plan

## Prerequisites
- Move device connected and accessible via SSH
- move-anything installed on device
- Signal Chain module with patches available

## Phase 1: Parameter API Foundation

### Test 1.1: Basic Shm Setup
1. SSH to device: `ssh ableton@move.local`
2. Check shared memory exists:
   ```bash
   ls -la /dev/shm/move-shadow*
   ```
3. Expected: See `move-shadow-param` alongside existing shm files

### Test 1.2: Parameter Get/Set (via JS console)
1. Toggle shadow UI on with Shift+Menu
2. Check shadow_ui.log for successful initialization
3. In shadow_ui.js, verify `getPatchCount(0)` returns a number
4. Verify `getPatchName(0, 0)` returns patch name string

## Phase 2: Enhanced Patch Browser

### Test 2.1: View Navigation
1. Start Move with move-anything
2. Press Shift+Menu to toggle shadow UI
3. **Expected**: See list of 4 slots with patch names

### Test 2.2: Enter Patch Browser
1. From slots view, jog to select a slot
2. Click jog wheel (CC 3 value 127)
3. **Expected**: View changes to patch list for that slot
4. If no patches: Should display "No patches found"

### Test 2.3: Browse Patches
1. In patches view, use jog wheel to scroll through list
2. **Expected**: Selection highlight moves up/down
3. **Expected**: List scrolls when selection goes beyond visible area

### Test 2.4: Enter Patch Detail
1. Select a patch in the list
2. Click jog wheel
3. **Expected**: View changes to patch detail showing:
   - Patch name
   - Synth module info
   - FX1 module info
   - FX2 module info
   - "Load" action

### Test 2.5: Load Patch
1. In patch detail view, select "Load" action
2. Click jog wheel
3. **Expected**: Patch loads to the slot
4. **Expected**: Audio output changes to new patch sound

### Test 2.6: Navigate Back
1. From any nested view, press Back button
2. **Expected**: Returns to previous view
3. **Expected**: From slots view, Back toggles shadow UI off

## Phase 3: Component Parameter Editing

### Test 3.1: Enter Component Params
1. In patch detail view, select "Edit Synth"
2. Click jog wheel
3. **Expected**: View shows synth parameters list:
   - Preset
   - Volume
   - Pan
   - Transpose
   - Tune

### Test 3.2: Edit Parameter Value
1. In component params view, select a parameter
2. Click jog wheel to enter edit mode
3. **Expected**: Value field is now editable (indicated visually)
4. Use jog wheel to adjust value
5. **Expected**: Value changes in display
6. **Expected**: Audio changes in real-time (for audible params)

### Test 3.3: Confirm/Cancel Edit
1. While editing, click jog wheel to confirm
2. **Expected**: Returns to param selection, value persists
3. Alternative: Press Back to cancel without saving
4. **Expected**: Value reverts to original

### Test 3.4: FX Parameter Editing
1. Navigate to patch detail, select "Edit FX1"
2. **Expected**: Shows FX parameters:
   - Wet/Dry/Mix
   - Bypass
   - FX-specific params (reverb: room_size, damping, etc.)
3. Verify adjustments affect audio output

## Phase 4: Shared UI Components

### Test 4.1: Main Chain UI Still Works
1. Exit shadow UI
2. Load Signal Chain module normally
3. Browse patches via main UI
4. **Expected**: All existing functionality works unchanged

### Test 4.2: Code Inspection
1. Verify shared modules exist:
   ```bash
   ls -la src/shared/chain_ui_views.mjs
   ls -la src/shared/chain_param_utils.mjs
   ```
2. Check imports work in both UIs

## Debugging

### If clicks don't register:
1. Check footer shows last CC: `CC3:127` indicates click received
2. If no CC shown, check MIDI filtering in shim

### If patches don't load:
1. SSH and check: `ls /data/UserData/move-anything/modules/chain/patches/`
2. Verify .json files exist with valid content

### If display doesn't update:
1. Check shadow_ui.log for errors
2. Verify shared memory is being written

### Log locations:
- `/data/UserData/move-anything/shadow_ui.log` - UI initialization
- `/data/UserData/move-anything/shadow_inprocess.log` - Chain loading
- `/data/UserData/move-anything/move-anything.log` - Main host log
