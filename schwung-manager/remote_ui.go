package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"

	"nhooyr.io/websocket"
	"nhooyr.io/websocket/wsjson"
)

// ---------------------------------------------------------------------------
// RemoteUI — WebSocket bridge between browser clients and ShmParams
// ---------------------------------------------------------------------------

// RemoteUI manages WebSocket connections for the remote parameter UI.
// Each client can subscribe to one or more shadow chain slots; the poll
// loop fetches parameter values via ShmParams and pushes diffs.
//
// ShmParams may be nil at startup (shared memory not yet created) and will
// be lazily connected when the first request arrives.
type RemoteUI struct {
	shm        *ShmParams
	setRing    *ShmWebParamSetRing    // fast fire-and-forget param writes (~3ms)
	notifyRing *ShmWebParamNotifyRing // push-based param change notifications from shim
	basePath   string                 // e.g. /data/UserData/schwung — for locating module web_ui.html
	logger     *slog.Logger

	mu      sync.Mutex
	clients map[*ruClient]struct{}
}

// ruClient represents a single WebSocket connection.
type ruClient struct {
	conn *websocket.Conn
	mu   sync.Mutex // serialises writes

	// slots this client is subscribed to (slot index -> true)
	subs map[uint8]bool
	// whether this client is subscribed to master FX
	masterFxSub bool
}

// per-slot cached state used by the poll loop.
type slotCache struct {
	params      map[string]string // key -> last known value
	hierarchies map[string]string // component -> last ui_hierarchy JSON
	modules     map[string]string // component -> last module ID
}

// --- Inbound message types (browser -> server) ---

type wsMessage struct {
	Type  string `json:"type"`
	Slot  *uint8 `json:"slot,omitempty"`
	Key   string `json:"key,omitempty"`
	Value string `json:"value,omitempty"`
}

// --- Outbound message types (server -> browser) ---

type wsHierarchy struct {
	Type      string          `json:"type"`
	Slot      uint8           `json:"slot"`
	Component string          `json:"component"`
	Data      json.RawMessage `json:"data"`
}

type wsChainParams struct {
	Type      string          `json:"type"`
	Slot      uint8           `json:"slot"`
	Component string          `json:"component"`
	Data      json.RawMessage `json:"data"`
}

type wsSlotInfo struct {
	Type    string `json:"type"`
	Slot    uint8  `json:"slot"`
	Synth   string `json:"synth"`
	FX1     string `json:"fx1"`
	FX2     string `json:"fx2"`
	MidiFX1 string `json:"midi_fx1"`
}

type wsMasterFxInfo struct {
	Type string `json:"type"`
	FX1  string `json:"fx1"`
	FX2  string `json:"fx2"`
	FX3  string `json:"fx3"`
	FX4  string `json:"fx4"`
}

type wsParamUpdate struct {
	Type   string            `json:"type"`
	Slot   uint8             `json:"slot"`
	Params map[string]string `json:"params"`
}

type wsCustomUI struct {
	Type      string `json:"type"`
	Slot      uint8  `json:"slot"`
	Component string `json:"component"`
	URL       string `json:"url"`
}

type wsError struct {
	Type    string `json:"type"`
	Message string `json:"message"`
}

// componentPrefixes lists all component types in a shadow slot.
var componentPrefixes = []string{"synth", "fx1", "fx2", "midi_fx1"}

// masterFxSlots lists the 4 master FX slot identifiers.
var masterFxSlots = []string{"fx1", "fx2", "fx3", "fx4"}

// NewRemoteUI creates a RemoteUI. shm/setRing may be nil (lazy connect on first use).
func NewRemoteUI(shm *ShmParams, setRing *ShmWebParamSetRing, basePath string, logger *slog.Logger) *RemoteUI {
	return &RemoteUI{
		shm:      shm,
		setRing:  setRing,
		basePath: basePath,
		logger:   logger,
		clients:  make(map[*ruClient]struct{}),
	}
}

// setParam writes a param using the fast ring buffer if available,
// falling back to the old shared memory path.
func (ru *RemoteUI) setParam(slot uint8, key, value string) error {
	if ring := ru.ensureSetRing(); ring != nil {
		return ring.SetParam(slot, key, value)
	}
	if shm := ru.ensureShm(); shm != nil {
		return shm.SetParamFast(slot, key, value)
	}
	return fmt.Errorf("no shared memory available")
}

// ensureSetRing attempts to open the web param set ring if not yet connected.
func (ru *RemoteUI) ensureSetRing() *ShmWebParamSetRing {
	if ru.setRing != nil {
		return ru.setRing
	}
	ring := OpenShmWebParamSetRing()
	if ring != nil {
		ru.setRing = ring
		ru.logger.Info("web param set ring: connected (lazy)")
	}
	return ru.setRing
}

// ensureShm attempts to open shared memory if not yet connected.
// Returns the ShmParams (possibly nil if still unavailable).
func (ru *RemoteUI) ensureShm() *ShmParams {
	if ru.shm != nil {
		return ru.shm
	}
	shm := OpenShmParams()
	if shm != nil {
		ru.shm = shm
		ru.logger.Info("shared memory params: connected (lazy)")
	}
	return ru.shm
}

// Start launches the background poll loop and notify reader. Call from main before ListenAndServe.
func (ru *RemoteUI) Start(ctx context.Context) {
	go ru.pollLoop(ctx)
	go ru.notifyLoop(ctx)
}

// ServeHTTP upgrades the request to a WebSocket and handles messages.
func (ru *RemoteUI) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		// Allow any origin — the server is on a local network device.
		InsecureSkipVerify: true,
	})
	if err != nil {
		ru.logger.Error("websocket accept failed", "err", err)
		return
	}

	client := &ruClient{
		conn: conn,
		subs: make(map[uint8]bool),
	}

	ru.mu.Lock()
	ru.clients[client] = struct{}{}
	ru.mu.Unlock()

	defer func() {
		ru.mu.Lock()
		delete(ru.clients, client)
		ru.mu.Unlock()
		conn.Close(websocket.StatusNormalClosure, "bye")
	}()

	ru.readLoop(r.Context(), client)
}

// requireShm tries to connect shared memory and sends an error if unavailable.
// Returns true if shm is ready.
func (ru *RemoteUI) requireShm(ctx context.Context, c *ruClient) bool {
	if ru.ensureShm() != nil {
		return true
	}
	ru.sendError(ctx, c, "shared memory not available (Move may still be starting)")
	return false
}

// readLoop processes inbound messages from a single client.
func (ru *RemoteUI) readLoop(ctx context.Context, c *ruClient) {
	for {
		var msg wsMessage
		if err := wsjson.Read(ctx, c.conn, &msg); err != nil {
			// Client disconnected or context cancelled — normal.
			ru.logger.Debug("ws read done", "err", err)
			return
		}

		switch msg.Type {
		case "subscribe":
			ru.handleSubscribe(ctx, c, msg)
		case "unsubscribe":
			ru.handleUnsubscribe(c, msg)
		case "set_param":
			ru.handleSetParam(ctx, c, msg)
		case "get_hierarchy":
			ru.handleGetHierarchy(ctx, c, msg)
		case "subscribe_master_fx":
			ru.handleSubscribeMasterFx(ctx, c)
		case "unsubscribe_master_fx":
			ru.handleUnsubscribeMasterFx(c)
		case "set_master_fx_param":
			ru.handleSetMasterFxParam(ctx, c, msg)
		default:
			ru.sendError(ctx, c, "unknown message type: "+msg.Type)
		}
	}
}

func (ru *RemoteUI) handleSubscribe(ctx context.Context, c *ruClient, msg wsMessage) {
	slot := ru.slotFromMsg(msg)

	c.mu.Lock()
	c.subs[slot] = true
	c.mu.Unlock()

	ru.logger.Info("ws subscribe", "slot", slot)

	if !ru.requireShm(ctx, c) {
		return
	}

	// Send slot info (which components are loaded).
	ru.sendSlotInfo(ctx, c, slot)

	// Send hierarchy and chain_params for all loaded components.
	for _, comp := range componentPrefixes {
		moduleKey := comp + "_module"
		modID, err := ru.shm.GetParam(slot, moduleKey)
		if err != nil || modID == "" {
			continue
		}
		// Check for custom web UI (synth component only).
		if comp == "synth" {
			if url := ru.findModuleWebUI(modID); url != "" {
				ru.sendCustomUI(ctx, c, slot, comp, url)
			}
		}
		ru.sendHierarchy(ctx, c, slot, comp)
		ru.sendChainParams(ctx, c, slot, comp)
	}
}

func (ru *RemoteUI) handleUnsubscribe(c *ruClient, msg wsMessage) {
	slot := ru.slotFromMsg(msg)
	c.mu.Lock()
	delete(c.subs, slot)
	c.mu.Unlock()
	ru.logger.Info("ws unsubscribe", "slot", slot)
}

func (ru *RemoteUI) handleSetParam(ctx context.Context, c *ruClient, msg wsMessage) {
	if !ru.requireShm(ctx, c) {
		return
	}
	slot := ru.slotFromMsg(msg)
	if msg.Key == "" {
		ru.sendError(ctx, c, "set_param requires key")
		return
	}
	if err := ru.setParam(slot, msg.Key, msg.Value); err != nil {
		ru.logger.Error("set_param failed", "slot", slot, "key", msg.Key, "err", err)
		ru.sendError(ctx, c, "set_param failed: "+err.Error())
	}
}

func (ru *RemoteUI) handleGetHierarchy(ctx context.Context, c *ruClient, msg wsMessage) {
	if !ru.requireShm(ctx, c) {
		return
	}
	slot := ru.slotFromMsg(msg)
	ru.sendSlotInfo(ctx, c, slot)
	for _, comp := range componentPrefixes {
		moduleKey := comp + "_module"
		modID, err := ru.shm.GetParam(slot, moduleKey)
		if err != nil || modID == "" {
			continue
		}
		if comp == "synth" {
			if url := ru.findModuleWebUI(modID); url != "" {
				ru.sendCustomUI(ctx, c, slot, comp, url)
			}
		}
		ru.sendHierarchy(ctx, c, slot, comp)
		ru.sendChainParams(ctx, c, slot, comp)
	}
}

func (ru *RemoteUI) handleSubscribeMasterFx(ctx context.Context, c *ruClient) {
	c.mu.Lock()
	c.masterFxSub = true
	c.mu.Unlock()

	ru.logger.Info("ws subscribe master_fx")

	if !ru.requireShm(ctx, c) {
		return
	}

	// Send master FX info (which modules are loaded).
	ru.sendMasterFxInfo(ctx, c)

	// Send hierarchy and chain_params for each loaded master FX slot.
	for _, fxSlot := range masterFxSlots {
		moduleKey := "master_fx:" + fxSlot + "_module"
		modID, err := ru.shm.GetParam(0, moduleKey)
		if err != nil || modID == "" {
			continue
		}
		compName := "master_fx:" + fxSlot
		ru.sendHierarchy(ctx, c, 0, compName)
		ru.sendChainParams(ctx, c, 0, compName)
	}
}

func (ru *RemoteUI) handleUnsubscribeMasterFx(c *ruClient) {
	c.mu.Lock()
	c.masterFxSub = false
	c.mu.Unlock()
	ru.logger.Info("ws unsubscribe master_fx")
}

func (ru *RemoteUI) handleSetMasterFxParam(ctx context.Context, c *ruClient, msg wsMessage) {
	if !ru.requireShm(ctx, c) {
		return
	}
	if msg.Key == "" {
		ru.sendError(ctx, c, "set_master_fx_param requires key")
		return
	}
	// All master FX params go through slot 0.
	if err := ru.setParam(0, msg.Key, msg.Value); err != nil {
		ru.logger.Error("set_master_fx_param failed", "key", msg.Key, "err", err)
		ru.sendError(ctx, c, "set_master_fx_param failed: "+err.Error())
	}
}

// slotFromMsg returns the slot number, defaulting to 0.
func (ru *RemoteUI) slotFromMsg(msg wsMessage) uint8 {
	if msg.Slot != nil {
		return *msg.Slot
	}
	return 0
}

// ---------------------------------------------------------------------------
// Outbound helpers
// ---------------------------------------------------------------------------

func (ru *RemoteUI) sendSlotInfo(ctx context.Context, c *ruClient, slot uint8) {
	info := wsSlotInfo{Type: "slot_info", Slot: slot}
	for _, comp := range componentPrefixes {
		modID, _ := ru.shm.GetParam(slot, comp+"_module")
		switch comp {
		case "synth":
			info.Synth = modID
		case "fx1":
			info.FX1 = modID
		case "fx2":
			info.FX2 = modID
		case "midi_fx1":
			info.MidiFX1 = modID
		}
	}
	ru.writeJSON(ctx, c, info)
}

func (ru *RemoteUI) sendMasterFxInfo(ctx context.Context, c *ruClient) {
	info := wsMasterFxInfo{Type: "master_fx_info"}
	for _, fxSlot := range masterFxSlots {
		moduleKey := "master_fx:" + fxSlot + "_module"
		modID, _ := ru.shm.GetParam(0, moduleKey)
		switch fxSlot {
		case "fx1":
			info.FX1 = modID
		case "fx2":
			info.FX2 = modID
		case "fx3":
			info.FX3 = modID
		case "fx4":
			info.FX4 = modID
		}
	}
	ru.writeJSON(ctx, c, info)
}

func (ru *RemoteUI) sendHierarchy(ctx context.Context, c *ruClient, slot uint8, component string) {
	raw, err := ru.shm.GetParam(slot, component+":ui_hierarchy")
	if err != nil {
		ru.logger.Debug("get ui_hierarchy failed", "slot", slot, "component", component, "err", err)
		raw = "{}"
	}
	var js json.RawMessage
	if json.Unmarshal([]byte(raw), &js) != nil {
		js = json.RawMessage(`{}`)
	} else {
		js = json.RawMessage(raw)
	}
	ru.writeJSON(ctx, c, wsHierarchy{Type: "hierarchy", Slot: slot, Component: component, Data: js})
}

func (ru *RemoteUI) sendChainParams(ctx context.Context, c *ruClient, slot uint8, component string) {
	raw, err := ru.shm.GetParam(slot, component+":chain_params")
	if err != nil {
		ru.logger.Debug("get chain_params failed", "slot", slot, "component", component, "err", err)
		raw = "[]"
	}
	var js json.RawMessage
	if json.Unmarshal([]byte(raw), &js) != nil {
		js = json.RawMessage(`[]`)
	} else {
		js = json.RawMessage(raw)
	}
	ru.writeJSON(ctx, c, wsChainParams{Type: "chain_params", Slot: slot, Component: component, Data: js})
}

// moduleCategoryDirs lists the subdirectories under modules/ to search.
var moduleCategoryDirs = []string{"", "sound_generators", "audio_fx", "midi_fx", "tools", "overtake"}

// findModuleWebUI checks if a module has a web_ui.html file and returns its
// URL path (e.g. "/api/remote-ui/module-assets/braids/web_ui.html"), or "".
func (ru *RemoteUI) findModuleWebUI(moduleID string) string {
	if ru.basePath == "" || moduleID == "" {
		return ""
	}
	for _, cat := range moduleCategoryDirs {
		candidate := filepath.Join(ru.basePath, "modules", cat, moduleID, "web_ui.html")
		if _, err := os.Stat(candidate); err == nil {
			return "/api/remote-ui/module-assets/" + moduleID + "/web_ui.html"
		}
	}
	return ""
}

// sendCustomUI notifies a client that a module has a custom web UI.
func (ru *RemoteUI) sendCustomUI(ctx context.Context, c *ruClient, slot uint8, component, url string) {
	ru.writeJSON(ctx, c, wsCustomUI{Type: "custom_ui", Slot: slot, Component: component, URL: url})
}

func (ru *RemoteUI) sendError(ctx context.Context, c *ruClient, message string) {
	ru.writeJSON(ctx, c, wsError{Type: "error", Message: message})
}

func (ru *RemoteUI) writeJSON(ctx context.Context, c *ruClient, v any) {
	c.mu.Lock()
	defer c.mu.Unlock()
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	if err := wsjson.Write(ctx, c.conn, v); err != nil {
		ru.logger.Debug("ws write failed", "err", err)
	}
}

// ---------------------------------------------------------------------------
// Poll loop — fetches params for subscribed slots and pushes diffs
// ---------------------------------------------------------------------------

// notifyLoop reads the param change notify ring from the shim and pushes
// updates to subscribed WebSocket clients. Runs at ~5ms interval for
// near-instant hardware knob → browser updates.
func (ru *RemoteUI) notifyLoop(ctx context.Context) {
	ticker := time.NewTicker(5 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}

		ring := ru.ensureNotifyRing()
		if ring == nil {
			continue
		}

		changes := ring.Drain()
		if len(changes) == 0 {
			continue
		}

		// Group changes by slot for efficient broadcasting.
		slotChanges := make(map[uint8]map[string]string)
		for _, c := range changes {
			m, ok := slotChanges[c.Slot]
			if !ok {
				m = make(map[string]string)
				slotChanges[c.Slot] = m
			}
			m[c.Key] = c.Value
		}

		// Broadcast to subscribed clients.
		ru.mu.Lock()
		clients := make([]*ruClient, 0, len(ru.clients))
		for c := range ru.clients {
			clients = append(clients, c)
		}
		ru.mu.Unlock()

		for slot, params := range slotChanges {
			update := wsParamUpdate{Type: "param_update", Slot: slot, Params: params}
			for _, c := range clients {
				c.mu.Lock()
				subscribed := c.subs[slot]
				c.mu.Unlock()
				if subscribed {
					ru.writeJSON(ctx, c, update)
				}
			}
		}
	}
}

// ensureNotifyRing attempts to open the notify ring if not yet connected.
func (ru *RemoteUI) ensureNotifyRing() *ShmWebParamNotifyRing {
	if ru.notifyRing != nil {
		return ru.notifyRing
	}
	ring := OpenShmWebParamNotifyRing()
	if ring != nil {
		ru.notifyRing = ring
		ru.logger.Info("web param notify ring: connected (lazy)")
	}
	return ru.notifyRing
}

func (ru *RemoteUI) pollLoop(ctx context.Context) {
	// Slow poll — only checks module/hierarchy changes, NOT individual params.
	// Param values come via the notify ring (notifyLoop at 5ms).
	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()

	caches := make(map[uint8]*slotCache) // slot -> cache
	var masterFxCache *slotCache

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}

		// Try to connect shared memory if not yet available.
		if ru.ensureShm() == nil {
			continue
		}

		// Determine which slots have at least one subscriber.
		activeSlots, hasMasterFxSubs := ru.activeSlotsAndMasterFx()

		for _, slot := range activeSlots {
			cache, ok := caches[slot]
			if !ok {
				cache = &slotCache{
					params:      make(map[string]string),
					hierarchies: make(map[string]string),
					modules:     make(map[string]string),
				}
				caches[slot] = cache
			}
			ru.pollSlot(ctx, slot, cache)
		}

		if hasMasterFxSubs {
			if masterFxCache == nil {
				masterFxCache = &slotCache{
					params:      make(map[string]string),
					hierarchies: make(map[string]string),
					modules:     make(map[string]string),
				}
			}
			ru.pollMasterFx(ctx, masterFxCache)
		}
	}
}

// activeSlotsAndMasterFx returns a deduplicated list of slots with subscribers
// and whether any client is subscribed to master FX.
func (ru *RemoteUI) activeSlotsAndMasterFx() ([]uint8, bool) {
	ru.mu.Lock()
	defer ru.mu.Unlock()

	seen := make(map[uint8]bool)
	hasMasterFx := false
	for c := range ru.clients {
		c.mu.Lock()
		for s := range c.subs {
			seen[s] = true
		}
		if c.masterFxSub {
			hasMasterFx = true
		}
		c.mu.Unlock()
	}

	slots := make([]uint8, 0, len(seen))
	for s := range seen {
		slots = append(slots, s)
	}
	return slots, hasMasterFx
}

// chainParam is the minimal structure we parse from chain_params JSON.
type chainParam struct {
	Key string `json:"key"`
}

// pollSlot checks for module/hierarchy changes only (infrequent).
// Param value updates come via the notify ring — NO per-param polling here,
// which was starving shadow_ui.js of the shared param channel.
func (ru *RemoteUI) pollSlot(ctx context.Context, slot uint8, cache *slotCache) {
	for _, comp := range componentPrefixes {
		// Check if this component is loaded (1 shm read per component).
		modID, ok, err := ru.shm.TryGetParam(slot, comp+"_module")
		if !ok || err != nil {
			continue // mutex busy or shm error — skip this tick, don't change state
		}

		// Detect module change (loaded/unloaded/swapped).
		if prev, ok := cache.modules[comp]; !ok || prev != modID {
			cache.modules[comp] = modID
			ru.broadcastSlotInfo(ctx, slot)
			if modID != "" {
				if comp == "synth" {
					if url := ru.findModuleWebUI(modID); url != "" {
						ru.broadcastCustomUI(ctx, slot, comp, url)
					}
				}
				ru.broadcastHierarchy(ctx, slot, comp)
				ru.broadcastChainParams(ctx, slot, comp)
			}
		}

		if modID == "" {
			continue
		}

		// Detect hierarchy changes (dynamic modules like JV-880).
		hierJSON, ok, _ := ru.shm.TryGetParam(slot, comp+":ui_hierarchy")
		if ok && hierJSON != "" {
			if prev, exists := cache.hierarchies[comp]; !exists || prev != hierJSON {
				cache.hierarchies[comp] = hierJSON
				ru.broadcastHierarchy(ctx, slot, comp)
			}
		}
	}
}

// broadcastSlotInfo sends slot_info to all subscribers of a slot.
func (ru *RemoteUI) broadcastSlotInfo(ctx context.Context, slot uint8) {
	for _, c := range ru.subscribedClients(slot) {
		ru.sendSlotInfo(ctx, c, slot)
	}
}

// broadcastHierarchy sends hierarchy for a component to all subscribers of a slot.
func (ru *RemoteUI) broadcastHierarchy(ctx context.Context, slot uint8, component string) {
	for _, c := range ru.subscribedClients(slot) {
		ru.sendHierarchy(ctx, c, slot, component)
	}
}

// broadcastCustomUI sends custom_ui to all subscribers of a slot.
func (ru *RemoteUI) broadcastCustomUI(ctx context.Context, slot uint8, component, url string) {
	for _, c := range ru.subscribedClients(slot) {
		ru.sendCustomUI(ctx, c, slot, component, url)
	}
}

// broadcastChainParams sends chain_params for a component to all subscribers of a slot.
func (ru *RemoteUI) broadcastChainParams(ctx context.Context, slot uint8, component string) {
	for _, c := range ru.subscribedClients(slot) {
		ru.sendChainParams(ctx, c, slot, component)
	}
}

// pollMasterFx checks for module/hierarchy changes only (no per-param polling).
func (ru *RemoteUI) pollMasterFx(ctx context.Context, cache *slotCache) {
	for _, fxSlot := range masterFxSlots {
		compName := "master_fx:" + fxSlot
		moduleKey := "master_fx:" + fxSlot + "_module"

		modID, ok, _ := ru.shm.TryGetParam(0, moduleKey)
		if !ok {
			continue
		}

		// Detect module change.
		if prev, exists := cache.modules[fxSlot]; !exists || prev != modID {
			cache.modules[fxSlot] = modID
			ru.broadcastMasterFxInfo(ctx)
			if modID != "" {
				ru.broadcastMasterFxHierarchy(ctx, compName)
				ru.broadcastMasterFxChainParams(ctx, compName)
			}
		}

		if modID == "" {
			continue
		}

		// Detect hierarchy changes.
		hierJSON, ok, _ := ru.shm.TryGetParam(0, compName+":ui_hierarchy")
		if ok && hierJSON != "" {
			if prev, exists := cache.hierarchies[fxSlot]; !exists || prev != hierJSON {
				cache.hierarchies[fxSlot] = hierJSON
				ru.broadcastMasterFxHierarchy(ctx, compName)
			}
		}
	}
}

func (ru *RemoteUI) broadcastMasterFxInfo(ctx context.Context) {
	for _, c := range ru.masterFxSubscribedClients() {
		ru.sendMasterFxInfo(ctx, c)
	}
}

func (ru *RemoteUI) broadcastMasterFxHierarchy(ctx context.Context, compName string) {
	for _, c := range ru.masterFxSubscribedClients() {
		ru.sendHierarchy(ctx, c, 0, compName)
	}
}

func (ru *RemoteUI) broadcastMasterFxChainParams(ctx context.Context, compName string) {
	for _, c := range ru.masterFxSubscribedClients() {
		ru.sendChainParams(ctx, c, 0, compName)
	}
}

// masterFxSubscribedClients returns all clients subscribed to master FX.
func (ru *RemoteUI) masterFxSubscribedClients() []*ruClient {
	ru.mu.Lock()
	defer ru.mu.Unlock()
	var out []*ruClient
	for c := range ru.clients {
		c.mu.Lock()
		sub := c.masterFxSub
		c.mu.Unlock()
		if sub {
			out = append(out, c)
		}
	}
	return out
}

// subscribedClients returns all clients subscribed to a given slot.
func (ru *RemoteUI) subscribedClients(slot uint8) []*ruClient {
	ru.mu.Lock()
	defer ru.mu.Unlock()
	var out []*ruClient
	for c := range ru.clients {
		c.mu.Lock()
		sub := c.subs[slot]
		c.mu.Unlock()
		if sub {
			out = append(out, c)
		}
	}
	return out
}
