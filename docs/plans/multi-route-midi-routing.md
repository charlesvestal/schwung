# Multi-Route MIDI Routing for Shadow Slots

## Goal

Allow each shadow slot to have multiple MIDI routes, enabling scenarios like:
- Move Track 1 (Ch5) → JV-880 Ch5 (piano part)
- Move Track 4 (Ch10) → JV-880 Ch10 (rhythm part)
- Both routes go to the SAME JV-880 instance

## Current State

Each slot has:
- `channel` - single receive channel
- `forward_channel` - single forward channel (-1 = auto)

## New Design

Each slot has an array of routes:
```c
#define MAX_ROUTES_PER_SLOT 4

typedef struct shadow_route_t {
    int recv_channel;    // 0-15 (MIDI channel from Move)
    int fwd_channel;     // -1 = auto, 0-15 = specific channel to DSP
} shadow_route_t;

typedef struct shadow_chain_slot_t {
    void *instance;
    int active;
    int patch_index;
    float volume;
    int route_count;                          // Number of active routes
    shadow_route_t routes[MAX_ROUTES_PER_SLOT];
    char patch_name[64];
} shadow_chain_slot_t;
```

---

## Files to Modify

### 1. `src/move_anything_shim.c`

**Data structure changes:**
- Add `shadow_route_t` struct
- Replace `channel`/`forward_channel` with `route_count` and `routes[]`
- Update `shadow_ui_state_t` to expose routes

**MIDI routing changes:**
- Update `shadow_chain_slot_for_channel()` to search all routes across all slots
- Return both slot index and route index (or just the fwd_channel)
- Update `shadow_chain_remap_channel()` to use the matched route's fwd_channel

**Param API additions:**
- `slot:route_count` - get number of routes
- `slot:route_N_recv` - get/set receive channel for route N
- `slot:route_N_fwd` - get/set forward channel for route N
- `slot:add_route:RECV` - add new route with given recv channel
- `slot:remove_route:N` - remove route N

**Config parsing:**
- Support new `routes` array format in JSON
- Backwards compat: convert old `channel`/`forward_channel` to single route

### 2. `src/shadow/shadow_ui.js`

**UI state additions:**
- Track which route is selected in slot settings
- Track if adding a new route

**Slot settings view changes:**
- Show list of routes instead of single recv/fwd
- Each route shows: `Route 1: Ch5 → Auto`
- Selected route can be edited (recv ch, fwd ch)
- "Add Route" option at bottom
- Long-press or separate action to remove route

---

## Implementation Steps

### Step 1: Shim Data Structures
1. Define `shadow_route_t` struct
2. Update `shadow_chain_slot_t` with `route_count` and `routes[]`
3. Update `shadow_chain_defaults()` to initialize with 1 route per slot
4. Update `shadow_ui_state_t` to expose route data

### Step 2: MIDI Routing
1. Create `shadow_find_route_for_channel()` that returns {slot, route_idx}
2. Update `shadow_inprocess_process_midi()` to use new routing
3. Update channel remapping to use matched route's fwd_channel

### Step 3: Param API
1. Add route count getter/setter
2. Add per-route recv/fwd getters/setters
3. Add add_route/remove_route handlers
4. Update UI state sync

### Step 4: Config Parsing
1. Parse new `routes` array format
2. Backwards compat for old format

### Step 5: Shadow UI
1. Update slot settings view to show route list
2. Add route editing (recv/fwd per route)
3. Add "Add Route" functionality
4. Add route removal

---

## UI Flow

```
SLOTS view:
  > JV-880 Perf        Ch5,10
    SF2 Piano          Ch6
    ...

SLOT_SETTINGS view (for JV-880):
  > Patch:       JV-880 Perf
    Volume:      100%
    Route 1:     Ch5 → Auto
    Route 2:     Ch10 → Ch10
    [Add Route]

ROUTE_EDIT view (editing Route 2):
  > Recv Ch:     10
    Fwd Ch:      Ch10
    [Remove Route]
```

---

## Param API Reference

| Key | Type | Description |
|-----|------|-------------|
| `slot:route_count` | GET | Number of routes (1-4) |
| `slot:route_0_recv` | GET/SET | Route 0 receive channel (1-16) |
| `slot:route_0_fwd` | GET/SET | Route 0 forward channel (-1=auto, 1-16) |
| `slot:add_route` | SET | Add route, value = recv channel |
| `slot:remove_route` | SET | Remove route, value = route index |

---

## Verification

1. **Basic routing test:**
   - Set up slot with 2 routes: Ch5→Auto, Ch10→Ch10
   - Send MIDI on Ch5, verify DSP receives on Ch5
   - Send MIDI on Ch10, verify DSP receives on Ch10

2. **UI test:**
   - Enter slot settings, see route list
   - Add new route, verify it appears
   - Edit route recv/fwd, verify changes persist
   - Remove route, verify it's gone

3. **Config persistence:**
   - Save config with multiple routes
   - Restart, verify routes are restored

4. **Backwards compat:**
   - Load old config with single channel/forward_channel
   - Verify it converts to single route correctly
