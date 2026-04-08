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

// ---------------------------------------------------------------------------
// RemoteUI — WebSocket bridge between browser clients and ShmParams
// ---------------------------------------------------------------------------

// RemoteUI manages WebSocket connections for the remote parameter UI.
// Each client can subscribe to one or more shadow chain slots; the poll
// loop fetches parameter values via ShmParams and pushes diffs.
type RemoteUI struct {
	shm    *ShmParams
	logger *slog.Logger

	mu      sync.Mutex
	clients map[*ruClient]struct{}
}

// ruClient represents a single WebSocket connection.
type ruClient struct {
	conn *websocket.Conn
	mu   sync.Mutex // serialises writes

	// slots this client is subscribed to (slot index -> true)
	subs map[uint8]bool
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

type wsParamUpdate struct {
	Type   string            `json:"type"`
	Slot   uint8             `json:"slot"`
	Params map[string]string `json:"params"`
}

type wsError struct {
	Type    string `json:"type"`
	Message string `json:"message"`
}

// componentPrefixes lists all component types in a shadow slot.
var componentPrefixes = []string{"synth", "fx1", "fx2", "midi_fx1"}

// NewRemoteUI creates a RemoteUI. shmParams must not be nil.
func NewRemoteUI(shm *ShmParams, logger *slog.Logger) *RemoteUI {
	return &RemoteUI{
		shm:     shm,
		logger:  logger,
		clients: make(map[*ruClient]struct{}),
	}
}

// Start launches the background poll loop. Call from main before ListenAndServe.
func (ru *RemoteUI) Start(ctx context.Context) {
	go ru.pollLoop(ctx)
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

	// Send slot info (which components are loaded).
	ru.sendSlotInfo(ctx, c, slot)

	// Send hierarchy and chain_params for all loaded components.
	for _, comp := range componentPrefixes {
		moduleKey := comp + "_module"
		modID, err := ru.shm.GetParam(slot, moduleKey)
		if err != nil || modID == "" {
			continue
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
	slot := ru.slotFromMsg(msg)
	if msg.Key == "" {
		ru.sendError(ctx, c, "set_param requires key")
		return
	}
	if err := ru.shm.SetParam(slot, msg.Key, msg.Value); err != nil {
		ru.logger.Error("set_param failed", "slot", slot, "key", msg.Key, "err", err)
		ru.sendError(ctx, c, "set_param failed: "+err.Error())
	}
}

func (ru *RemoteUI) handleGetHierarchy(ctx context.Context, c *ruClient, msg wsMessage) {
	slot := ru.slotFromMsg(msg)
	ru.sendSlotInfo(ctx, c, slot)
	for _, comp := range componentPrefixes {
		moduleKey := comp + "_module"
		modID, err := ru.shm.GetParam(slot, moduleKey)
		if err != nil || modID == "" {
			continue
		}
		ru.sendHierarchy(ctx, c, slot, comp)
		ru.sendChainParams(ctx, c, slot, comp)
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

func (ru *RemoteUI) pollLoop(ctx context.Context) {
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()

	caches := make(map[uint8]*slotCache) // slot -> cache

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}

		// Determine which slots have at least one subscriber.
		activeSlots := ru.activeSlots()
		if len(activeSlots) == 0 {
			continue
		}

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
	}
}

// activeSlots returns a deduplicated list of slots with subscribers.
func (ru *RemoteUI) activeSlots() []uint8 {
	ru.mu.Lock()
	defer ru.mu.Unlock()

	seen := make(map[uint8]bool)
	for c := range ru.clients {
		c.mu.Lock()
		for s := range c.subs {
			seen[s] = true
		}
		c.mu.Unlock()
	}

	slots := make([]uint8, 0, len(seen))
	for s := range seen {
		slots = append(slots, s)
	}
	return slots
}

// chainParam is the minimal structure we parse from chain_params JSON.
type chainParam struct {
	Key string `json:"key"`
}

// pollSlot fetches current param values for a slot and broadcasts diffs.
// Also detects hierarchy/module changes for dynamic modules (e.g. JV-880).
func (ru *RemoteUI) pollSlot(ctx context.Context, slot uint8, cache *slotCache) {
	changed := make(map[string]string)

	for _, comp := range componentPrefixes {
		// Check if this component is loaded.
		modID, _ := ru.shm.GetParam(slot, comp+"_module")

		// Detect module change (loaded/unloaded/swapped).
		if prev, ok := cache.modules[comp]; !ok || prev != modID {
			cache.modules[comp] = modID
			// Module changed — broadcast updated slot_info and hierarchy.
			ru.broadcastSlotInfo(ctx, slot)
			if modID != "" {
				ru.broadcastHierarchy(ctx, slot, comp)
				ru.broadcastChainParams(ctx, slot, comp)
			}
		}

		if modID == "" {
			continue
		}

		// Detect hierarchy changes (dynamic modules like JV-880).
		hierJSON, _ := ru.shm.GetParam(slot, comp+":ui_hierarchy")
		if hierJSON != "" {
			if prev, ok := cache.hierarchies[comp]; !ok || prev != hierJSON {
				cache.hierarchies[comp] = hierJSON
				ru.broadcastHierarchy(ctx, slot, comp)
			}
		}

		// Fetch chain_params to learn which keys exist.
		raw, err := ru.shm.GetParam(slot, comp+":chain_params")
		if err != nil {
			continue
		}

		var params []chainParam
		if err := json.Unmarshal([]byte(raw), &params); err != nil {
			continue
		}

		for _, p := range params {
			if p.Key == "" {
				continue
			}
			fullKey := comp + ":" + p.Key
			val, err := ru.shm.GetParam(slot, fullKey)
			if err != nil {
				continue
			}
			if prev, ok := cache.params[fullKey]; !ok || prev != val {
				cache.params[fullKey] = val
				changed[fullKey] = val
			}
		}
	}

	if len(changed) == 0 {
		return
	}

	// Broadcast to subscribed clients.
	update := wsParamUpdate{Type: "param_update", Slot: slot, Params: changed}

	ru.mu.Lock()
	clients := make([]*ruClient, 0, len(ru.clients))
	for c := range ru.clients {
		clients = append(clients, c)
	}
	ru.mu.Unlock()

	for _, c := range clients {
		c.mu.Lock()
		subscribed := c.subs[slot]
		c.mu.Unlock()
		if subscribed {
			ru.writeJSON(ctx, c, update)
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

// broadcastChainParams sends chain_params for a component to all subscribers of a slot.
func (ru *RemoteUI) broadcastChainParams(ctx context.Context, slot uint8, component string) {
	for _, c := range ru.subscribedClients(slot) {
		ru.sendChainParams(ctx, c, slot, component)
	}
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
