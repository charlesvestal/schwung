# Remote Web UI Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a web-based remote UI that renders module parameter hierarchies as interactive controls (knobs, sliders, dropdowns) with bidirectional sync to hardware.

**Architecture:** Go server (schwung-manager) maps `/schwung-param` shared memory directly, exposes WebSocket for browser clients. Browser auto-generates UI from `ui_hierarchy` + `chain_params` JSON. No C changes needed — Go uses the same shared memory protocol as shadow_ui.js.

**Tech Stack:** Go stdlib + nhooyr.io/websocket, vanilla JS + knobs.js (or similar SVG knob library), HTML templates.

**Design doc:** `docs/plans/2026-04-08-remote-ui-design.md`

---

### Task 1: Go Shared Memory Param Bridge (`shmparams.go`)

Create a new Go file that maps `/schwung-param` shared memory and provides get/set operations matching the `shadow_param_t` protocol.

**Files:**
- Create: `schwung-manager/shmparams.go`

**Reference:**
- `src/host/shadow_constants.h:158-168` — `shadow_param_t` struct layout
- `schwung-manager/shmconfig.go` — existing shared memory pattern to follow

**Step 1: Create shmparams.go with struct mapping**

```go
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"sync"
	"syscall"
	"time"
)

// ShmParams provides get/set access to the shadow_param_t shared memory segment.
// Only one request can be in-flight at a time (shared with shadow_ui.js).
//
// Field offsets match shadow_param_t in src/host/shadow_constants.h.
type ShmParams struct {
	data []byte
	mu   sync.Mutex
	seq  uint32 // monotonic request ID
}

// Byte offsets into shadow_param_t (65664 bytes total).
const (
	paramOffRequestType  = 0     // uint8
	paramOffSlot         = 1     // uint8
	paramOffResponseReady = 2    // uint8
	paramOffError        = 3     // uint8
	paramOffRequestID    = 4     // uint32
	paramOffResponseID   = 8     // uint32
	paramOffResultLen    = 12    // int32
	paramOffKey          = 16    // char[64]
	paramOffValue        = 80    // char[65536]
	shmParamSize         = 65664
	paramKeyLen          = 64
	paramValueLen        = 65536
)

const shmParamPath = "/dev/shm/schwung-param"

func OpenShmParams() *ShmParams {
	f, err := os.OpenFile(shmParamPath, os.O_RDWR, 0)
	if err != nil {
		return nil
	}
	defer f.Close()

	data, err := syscall.Mmap(int(f.Fd()), 0, shmParamSize,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return nil
	}

	return &ShmParams{data: data}
}

// GetParam reads a parameter value from a chain slot.
// Returns the value string and any error. Thread-safe.
func (s *ShmParams) GetParam(slot uint8, key string) (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	// Wait for any in-flight request to finish
	if err := s.waitIdle(200 * time.Millisecond); err != nil {
		return "", err
	}

	s.seq++
	reqID := s.seq

	// Write request
	s.data[paramOffSlot] = slot
	s.data[paramOffResponseReady] = 0
	s.data[paramOffError] = 0
	binary.LittleEndian.PutUint32(s.data[paramOffRequestID:], reqID)
	binary.LittleEndian.PutUint32(s.data[paramOffResponseID:], 0)
	binary.LittleEndian.PutUint32(s.data[paramOffResultLen:], 0)

	// Write key (null-terminated)
	copy(s.data[paramOffKey:paramOffKey+paramKeyLen], make([]byte, paramKeyLen))
	copy(s.data[paramOffKey:], key)

	// Set request type last (triggers processing)
	s.data[paramOffRequestType] = 2 // get

	// Wait for response
	if err := s.waitResponse(reqID, 500*time.Millisecond); err != nil {
		s.data[paramOffRequestType] = 0 // cleanup
		return "", err
	}

	if s.data[paramOffError] != 0 {
		s.data[paramOffRequestType] = 0
		return "", fmt.Errorf("param error for key %q slot %d", key, slot)
	}

	resultLen := int32(binary.LittleEndian.Uint32(s.data[paramOffResultLen:]))
	if resultLen < 0 {
		s.data[paramOffRequestType] = 0
		return "", fmt.Errorf("negative result length for key %q", key)
	}
	if resultLen > paramValueLen {
		resultLen = paramValueLen
	}

	val := string(s.data[paramOffValue : paramOffValue+int(resultLen)])
	s.data[paramOffRequestType] = 0
	return val, nil
}

// SetParam writes a parameter value to a chain slot. Thread-safe.
func (s *ShmParams) SetParam(slot uint8, key, value string) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	if err := s.waitIdle(200 * time.Millisecond); err != nil {
		return err
	}

	s.seq++
	reqID := s.seq

	s.data[paramOffSlot] = slot
	s.data[paramOffResponseReady] = 0
	s.data[paramOffError] = 0
	binary.LittleEndian.PutUint32(s.data[paramOffRequestID:], reqID)

	// Write key
	copy(s.data[paramOffKey:paramOffKey+paramKeyLen], make([]byte, paramKeyLen))
	copy(s.data[paramOffKey:], key)

	// Write value
	copy(s.data[paramOffValue:paramOffValue+paramValueLen], make([]byte, min(len(value)+1, paramValueLen)))
	copy(s.data[paramOffValue:], value)

	s.data[paramOffRequestType] = 1 // set

	if err := s.waitResponse(reqID, 500*time.Millisecond); err != nil {
		s.data[paramOffRequestType] = 0
		return err
	}

	s.data[paramOffRequestType] = 0
	return nil
}

func (s *ShmParams) waitIdle(timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for s.data[paramOffRequestType] != 0 {
		if time.Now().After(deadline) {
			return fmt.Errorf("param channel busy (timeout)")
		}
		time.Sleep(1 * time.Millisecond)
	}
	return nil
}

func (s *ShmParams) waitResponse(reqID uint32, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		if s.data[paramOffResponseReady] != 0 {
			respID := binary.LittleEndian.Uint32(s.data[paramOffResponseID:])
			if respID == reqID {
				return nil
			}
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("param response timeout (req %d)", reqID)
		}
		time.Sleep(1 * time.Millisecond)
	}
}
```

**Step 2: Wire into App struct in main.go**

Add `shmParams *ShmParams` field to `App` struct. Open in `main()` alongside `OpenShmConfig()`.

**Step 3: Commit**

```
feat: add shared memory param bridge for remote UI
```

---

### Task 2: WebSocket Endpoint in Go

Add a WebSocket endpoint to schwung-manager that handles subscribe/unsubscribe, param get/set, and pushes state updates.

**Files:**
- Create: `schwung-manager/remote_ui.go`
- Modify: `schwung-manager/main.go` (add route, wire deps)
- Modify: `schwung-manager/go.mod` (add websocket dep)

**Step 1: Add nhooyr.io/websocket dependency**

```bash
cd schwung-manager && go get nhooyr.io/websocket@latest
```

**Step 2: Create remote_ui.go**

```go
package main

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"nhooyr.io/websocket"
	"nhooyr.io/websocket/wsjson"
)

// RemoteUI manages WebSocket connections for the remote parameter UI.
type RemoteUI struct {
	params *ShmParams
	logger *slog.Logger

	mu      sync.Mutex
	clients map[*remoteClient]struct{}
}

type remoteClient struct {
	conn       *websocket.Conn
	subscribed [4]bool // which slots this client watches
	cancel     context.CancelFunc
}

// Inbound message from browser.
type wsMessage struct {
	Type  string `json:"type"`
	Slot  int    `json:"slot,omitempty"`
	Key   string `json:"key,omitempty"`
	Value string `json:"value,omitempty"`
}

// Outbound messages to browser.
type wsHierarchy struct {
	Type string          `json:"type"`
	Slot int             `json:"slot"`
	Data json.RawMessage `json:"data"`
}

type wsChainParams struct {
	Type string          `json:"type"`
	Slot int             `json:"slot"`
	Data json.RawMessage `json:"data"`
}

type wsParamUpdate struct {
	Type   string            `json:"type"`
	Slot   int               `json:"slot"`
	Params map[string]string `json:"params"`
}

type wsSlotInfo struct {
	Type  string `json:"type"`
	Slot  int    `json:"slot"`
	Synth string `json:"synth"`
	Name  string `json:"name"`
}

func NewRemoteUI(params *ShmParams, logger *slog.Logger) *RemoteUI {
	r := &RemoteUI{
		params:  params,
		logger:  logger,
		clients: make(map[*remoteClient]struct{}),
	}
	go r.pollLoop()
	return r
}

func (r *RemoteUI) ServeHTTP(w http.ResponseWriter, req *http.Request) {
	conn, err := websocket.Accept(w, req, &websocket.AcceptOptions{
		InsecureSkipVerify: true, // allow any origin for local network
	})
	if err != nil {
		r.logger.Error("websocket accept failed", "err", err)
		return
	}

	ctx, cancel := context.WithCancel(req.Context())
	client := &remoteClient{conn: conn, cancel: cancel}

	r.mu.Lock()
	r.clients[client] = struct{}{}
	r.mu.Unlock()

	defer func() {
		r.mu.Lock()
		delete(r.clients, client)
		r.mu.Unlock()
		cancel()
		conn.Close(websocket.StatusNormalClosure, "")
	}()

	for {
		var msg wsMessage
		err := wsjson.Read(ctx, conn, &msg)
		if err != nil {
			return // client disconnected
		}
		r.handleMessage(ctx, client, msg)
	}
}

func (r *RemoteUI) handleMessage(ctx context.Context, c *remoteClient, msg wsMessage) {
	if r.params == nil {
		return
	}

	switch msg.Type {
	case "subscribe":
		if msg.Slot >= 0 && msg.Slot < 4 {
			c.subscribed[msg.Slot] = true
			r.sendSlotState(ctx, c, msg.Slot)
		}

	case "unsubscribe":
		if msg.Slot >= 0 && msg.Slot < 4 {
			c.subscribed[msg.Slot] = false
		}

	case "set_param":
		if msg.Slot >= 0 && msg.Slot < 4 {
			r.params.SetParam(uint8(msg.Slot), msg.Key, msg.Value)
		}

	case "get_hierarchy":
		if msg.Slot >= 0 && msg.Slot < 4 {
			r.sendHierarchy(ctx, c, msg.Slot)
		}
	}
}

func (r *RemoteUI) sendSlotState(ctx context.Context, c *remoteClient, slot int) {
	r.sendHierarchy(ctx, c, slot)
	r.sendChainParams(ctx, c, slot)
	// Initial param values will come from the next poll cycle
}

func (r *RemoteUI) sendHierarchy(ctx context.Context, c *remoteClient, slot int) {
	val, err := r.params.GetParam(uint8(slot), "synth:ui_hierarchy")
	if err != nil || val == "" {
		return
	}
	wsjson.Write(ctx, c.conn, wsHierarchy{
		Type: "hierarchy",
		Slot: slot,
		Data: json.RawMessage(val),
	})
}

func (r *RemoteUI) sendChainParams(ctx context.Context, c *remoteClient, slot int) {
	val, err := r.params.GetParam(uint8(slot), "synth:chain_params")
	if err != nil || val == "" {
		return
	}
	wsjson.Write(ctx, c.conn, wsChainParams{
		Type: "chain_params",
		Slot: slot,
		Data: json.RawMessage(val),
	})
}

// pollLoop reads param state every 100ms and pushes diffs to subscribed clients.
func (r *RemoteUI) pollLoop() {
	// Track last known values per slot for diffing
	type slotState struct {
		hierarchy   string
		chainParams string
		params      map[string]string
		paramKeys   []string
	}
	states := [4]*slotState{}
	for i := range states {
		states[i] = &slotState{params: make(map[string]string)}
	}

	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()

	for range ticker.C {
		r.mu.Lock()
		if len(r.clients) == 0 {
			r.mu.Unlock()
			continue
		}

		// Find which slots have at least one subscriber
		var activeSlots [4]bool
		for c := range r.clients {
			for i := 0; i < 4; i++ {
				if c.subscribed[i] {
					activeSlots[i] = true
				}
			}
		}
		r.mu.Unlock()

		if r.params == nil {
			continue
		}

		for slot := 0; slot < 4; slot++ {
			if !activeSlots[slot] {
				continue
			}

			st := states[slot]

			// Re-fetch chain_params to know which keys to poll
			cpJSON, _ := r.params.GetParam(uint8(slot), "synth:chain_params")
			if cpJSON != st.chainParams && cpJSON != "" {
				st.chainParams = cpJSON
				// Parse param keys from chain_params
				var cp []struct {
					Key string `json:"key"`
				}
				if json.Unmarshal([]byte(cpJSON), &cp) == nil {
					st.paramKeys = make([]string, len(cp))
					for i, p := range cp {
						st.paramKeys[i] = "synth:" + p.Key
					}
				}
			}

			// Poll each known param
			changed := make(map[string]string)
			for _, key := range st.paramKeys {
				val, err := r.params.GetParam(uint8(slot), key)
				if err != nil {
					continue
				}
				if old, ok := st.params[key]; !ok || old != val {
					st.params[key] = val
					changed[key] = val
				}
			}

			if len(changed) > 0 {
				r.broadcast(slot, wsParamUpdate{
					Type:   "param_update",
					Slot:   slot,
					Params: changed,
				})
			}
		}
	}
}

func (r *RemoteUI) broadcast(slot int, msg interface{}) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for c := range r.clients {
		if c.subscribed[slot] {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			wsjson.Write(ctx, c.conn, msg)
			cancel()
		}
	}
}
```

**Step 3: Wire into main.go**

Add to App struct and route setup:
```go
// In main(), after OpenShmParams():
var remoteUI *RemoteUI
if shmParams != nil {
    remoteUI = NewRemoteUI(shmParams, logger)
}

// In route setup:
if remoteUI != nil {
    mux.Handle("GET /ws/remote-ui", remoteUI)
}
```

Note: The WebSocket endpoint must be registered BEFORE CSRF middleware is applied (WebSocket upgrades don't carry CSRF tokens). Move it above the middleware chain, or add a CSRF exemption for `/ws/`.

**Step 4: Commit**

```
feat: add WebSocket endpoint for remote UI param sync
```

---

### Task 3: Remote UI HTML Page + Tab Navigation

Create the web page with tab switching between slots and basic layout structure.

**Files:**
- Create: `schwung-manager/templates/remote_ui.html`
- Create: `schwung-manager/static/remote-ui.js`
- Create: `schwung-manager/static/remote-ui.css`
- Modify: `schwung-manager/main.go` (add GET /remote-ui route)
- Modify: `schwung-manager/templates/base.html` (add nav link)

**Step 1: Create remote_ui.html template**

Template extends base.html with tab bar and slot container. Loads remote-ui.js and remote-ui.css.

```html
{{template "base" .}}
{{define "title"}}Remote UI{{end}}
{{define "content"}}
<div id="remote-ui">
  <div class="tab-bar">
    <button class="tab active" data-slot="0">Slot 1</button>
    <button class="tab" data-slot="1">Slot 2</button>
    <button class="tab" data-slot="2">Slot 3</button>
    <button class="tab" data-slot="3">Slot 4</button>
  </div>
  <div class="slot-container">
    <div class="slot-header">
      <h2 id="slot-title">Slot 1</h2>
      <nav id="breadcrumb"></nav>
    </div>
    <div id="knob-row" class="knob-row"></div>
    <div id="param-list" class="param-list"></div>
  </div>
</div>
<link rel="stylesheet" href="/static/remote-ui.css">
<script src="/static/remote-ui.js"></script>
{{end}}
```

**Step 2: Create remote-ui.css**

Basic styling for tab bar, knob row (horizontal flex), param list (vertical). Mobile-responsive with media queries for phone layout.

**Step 3: Create remote-ui.js with WebSocket connection and tab switching**

```javascript
// Connect WebSocket, handle tab clicks to subscribe/unsubscribe,
// receive hierarchy/chain_params/param_update messages.
// Store current state per slot. On tab switch, render from cached state.
```

Core structure:
- `connect()` — open WebSocket, set up message handlers
- `switchSlot(n)` — unsubscribe old, subscribe new, render cached state
- `onHierarchy(slot, data)` — cache and render if active
- `onChainParams(slot, data)` — cache param metadata
- `onParamUpdate(slot, params)` — update cached values, update controls

**Step 4: Add route in main.go**

```go
mux.HandleFunc("GET /remote-ui", app.handleRemoteUI)
```

Handler renders the template.

**Step 5: Add nav link in base.html**

Add "Remote UI" to the navigation bar.

**Step 6: Commit**

```
feat: add remote UI page with tab navigation and WebSocket connection
```

---

### Task 4: Auto-Generate UI from Hierarchy

Parse `ui_hierarchy` and `chain_params` to render knobs, sliders, dropdowns, and navigation links.

**Files:**
- Modify: `schwung-manager/static/remote-ui.js`
- Modify: `schwung-manager/static/remote-ui.css`

**Step 1: Parse hierarchy levels**

The `ui_hierarchy` JSON has a `levels` object where each level has:
- `knobs`: array of param key strings (map to visual knobs)
- `params`: array of strings, `{key, label}` objects, or `{level, label}` navigation objects
- `label`: level display name

Render the `root` level initially. Navigation objects push onto a breadcrumb stack.

**Step 2: Render knobs**

For each entry in the level's `knobs` array:
- Look up metadata in `chain_params` (type, min, max, step, options)
- Create an SVG rotary knob element (or use a lightweight knob library)
- Bind drag events to send `set_param` via WebSocket (throttled to ~30Hz)
- Listen for `param_update` to move knob position

Simple SVG knob implementation (no external dependency):
```javascript
function createKnob(key, meta, value) {
    // SVG circle with rotating indicator line
    // mousedown/mousemove for drag interaction
    // Maps pixel delta to param range
}
```

**Step 3: Render param list**

For each entry in the level's `params` array:
- **String or {key, label}**: Look up metadata in `chain_params`
  - `float` → horizontal slider with label and value display
  - `int` → slider with integer steps
  - `enum` → dropdown/select
- **{level, label}**: Render as clickable navigation link (→ arrow)

**Step 4: Breadcrumb navigation**

Maintain a stack of level names. Clicking breadcrumb entries pops back. Clicking navigation links pushes new level. Re-render on navigation.

**Step 5: Wire bidirectional updates**

- Control change → `ws.send({type: "set_param", slot, key, value})`
- Incoming `param_update` → find control by key, update visual state without triggering send

**Step 6: Commit**

```
feat: auto-generate knobs, sliders, and menus from module hierarchy
```

---

### Task 5: FX and MIDI FX Hierarchy Support

The synth is only one component. Each slot can also have audio FX (fx1, fx2) and MIDI FX (midi_fx1). These have their own `ui_hierarchy` and `chain_params` under different prefixes.

**Files:**
- Modify: `schwung-manager/remote_ui.go` (poll all component hierarchies)
- Modify: `schwung-manager/static/remote-ui.js` (render component sections)

**Step 1: Extend WebSocket to fetch all component hierarchies per slot**

On subscribe, also fetch:
- `fx1:ui_hierarchy`, `fx1:chain_params`
- `fx2:ui_hierarchy`, `fx2:chain_params`
- `midi_fx1:ui_hierarchy`, `midi_fx1:chain_params`
- `synth_module`, `fx1_module`, `fx2_module`, `midi_fx1_module` (module IDs)

**Step 2: Render component sections in the UI**

Each slot tab shows sections:
- **Synth** (with its hierarchy)
- **MIDI FX** (if loaded)
- **Audio FX 1** (if loaded)
- **Audio FX 2** (if loaded)

Each section is collapsible with its own knob row and param list.

**Step 3: Update poll loop for all component params**

Poll param keys across all component prefixes, not just `synth:`.

**Step 4: Commit**

```
feat: show FX and MIDI FX hierarchies in remote UI
```

---

### Task 6: Master FX Tab

Add Master FX as a 5th tab, showing the 4-slot master effects chain.

**Files:**
- Modify: `schwung-manager/remote_ui.go` (master FX param access)
- Modify: `schwung-manager/static/remote-ui.js` (master FX tab rendering)

**Step 1: Add Master FX subscribe support**

Master FX uses different param key prefixes: `master_fx:fx1:ui_hierarchy`, etc. The Go code needs to handle slot=0 with a special "master_fx" mode, or we add a `component` field to the WebSocket messages.

**Step 2: Render Master FX as 4 collapsible FX sections**

Similar to per-slot FX but all 4 master FX slots visible.

**Step 3: Commit**

```
feat: add Master FX tab to remote UI
```

---

### Task 7: Custom Module UI Support

Allow modules to ship `web_ui.html` for a custom UI instead of auto-generated.

**Files:**
- Modify: `schwung-manager/remote_ui.go` (detect and serve custom UI files)
- Modify: `schwung-manager/static/remote-ui.js` (iframe loading + schwungRemote API)
- Modify: `schwung-manager/main.go` (add route for module static files)

**Step 1: Add endpoint to serve module web UI files**

```
GET /api/remote-ui/module/{id}/web_ui.html
```

Reads from the module's install directory on disk.

**Step 2: Detect custom UI on subscribe**

When a slot is subscribed, check if the loaded synth module has a `web_ui.html`. Send a `custom_ui` message with the URL.

**Step 3: Load custom UI in iframe with schwungRemote API**

```javascript
// Parent window provides API via postMessage
window.addEventListener('message', (e) => {
    if (e.data.type === 'getParam') { ... }
    if (e.data.type === 'setParam') { ... }
});
```

The iframe gets `schwungRemote` object injected that proxies through postMessage to the parent WebSocket.

**Step 4: Commit**

```
feat: support custom web_ui.html per module in remote UI
```

---

### Task 8: Polish and Mobile

**Files:**
- Modify: `schwung-manager/static/remote-ui.css` (responsive layout)
- Modify: `schwung-manager/static/remote-ui.js` (reconnection, loading states)

**Step 1: WebSocket reconnection**

Auto-reconnect with exponential backoff on disconnect. Show connection status indicator.

**Step 2: Loading states**

Show spinner/skeleton while waiting for hierarchy. Show "No module loaded" for empty slots.

**Step 3: Mobile layout**

- Tabs become scrollable on small screens
- Knobs stack 4-per-row on phone (vs 8 on desktop)
- Touch-friendly knob interaction (vertical drag)

**Step 4: Commit**

```
feat: add reconnection, loading states, and mobile layout to remote UI
```
