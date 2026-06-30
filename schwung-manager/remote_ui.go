package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"strconv"
	"strings"
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
	// whether this client is subscribed to the active overtake tool's UI
	toolSub bool
	// rev-gated tool polling: skip the heavy "state" snapshot read unless the
	// tool's cheap "rui_poll" digest (rev:on:tick:bpm) shows a content change.
	// Touched only from this client's WS read goroutine. toolSynced=false until
	// the first full fetch.
	toolSynced   bool
	toolLastRev  int64
	toolLastTick int64
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

// wsToolInfo reports the active overtake tool to the Tool tab.
// id == "" means no tool with a remote UI is currently loaded.
type wsToolInfo struct {
	Type string `json:"type"`
	ID   string `json:"id"`
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

// overtakeParamPrefix is the param prefix for an active overtake tool's DSP
// The shim routes "overtake_dsp:<key>" GET/SET on the
// shadow_param ring straight to the loaded overtake DSP instance via
// shim_handle_param_special — the same path shadow_ui.js uses on-device.
const overtakeParamPrefix = "overtake_dsp:"

// setParam writes a param using the fast ring buffer if available,
// falling back to the old shared memory path.
func (ru *RemoteUI) setParam(slot uint8, key, value string) error {
	// Overtake-tool params MUST go through the reliable shadow_param ring
	// (request_type=1), which the shim routes to the overtake DSP. The fast
	// web_param_set ring's shadow_direct_set_param has no "overtake_dsp:"
	// branch, so it would mis-route the key to a chain slot. The slow ring is
	// fine here: overtake-tool edits are discrete ops, not knob-drag streams,
	// and it lifts the fast ring's 255-byte value cap (64KB on the slow ring).
	if strings.HasPrefix(key, overtakeParamPrefix) {
		if shm := ru.ensureShm(); shm != nil {
			return shm.SetParam(slot, key, value)
		}
		return fmt.Errorf("no shared memory available")
	}
	if ring := ru.ensureSetRing(); ring != nil {
		return ring.SetParam(slot, key, value)
	}
	if shm := ru.ensureShm(); shm != nil {
		return shm.SetParamFast(slot, key, value)
	}
	return fmt.Errorf("no shared memory available")
}

// activeOvertakeToolID returns the module id of the overtake tool currently
// loaded that opts into a remote UI by answering the
// "overtake_dsp:module_id" probe, or "" if no such tool is active. The shim
// returns an error for this GET when no overtake DSP is loaded (or the loaded
// one predates the probe), so this is backward-safe.
func (ru *RemoteUI) activeOvertakeToolID(slot uint8) string {
	shm := ru.ensureShm()
	if shm == nil {
		return ""
	}
	id, err := shm.GetParam(slot, overtakeParamPrefix+"module_id")
	if err != nil {
		return ""
	}
	return id
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

// Start launches the background poll loop, notify reader, and periodic refresh.
func (ru *RemoteUI) Start(ctx context.Context) {
	go ru.pollLoop(ctx)
	go ru.notifyLoop(ctx)
	go ru.refreshLoop(ctx)
}

// refreshLoop periodically re-reads all param values for subscribed slots.
// Catches any missed values from initial load or state drift. Runs every 5s
// with batched reads + yields to minimize impact on shadow_ui.js.
func (ru *RemoteUI) refreshLoop(ctx context.Context) {
	// Wait a long gap BETWEEN iterations rather than a fixed ticker interval.
	// sendInitialParamValues for a large module (Surge ~277 params) can take
	// 7–14 seconds; a 5s ticker would queue ticks and hammer the shm channel
	// continuously, starving handleSubscribe / sendHierarchy of access.
	const interval = 30 * time.Second

	for {
		select {
		case <-ctx.Done():
			return
		case <-time.After(interval):
		}

		shm := ru.ensureShm()
		if shm == nil {
			continue
		}

		activeSlots, _ := ru.activeSlotsAndMasterFx()
		for _, slot := range activeSlots {
			for _, comp := range componentPrefixes {
				modID, _, err := shm.TryGetParam(slot, comp+"_module")
				if err != nil || modID == "" {
					continue
				}
				// Read shm once and fan out to all subscribers of this slot.
				ru.broadcastInitialParamValues(ctx, slot, comp, ru.subscribedClients(slot))
			}
		}

		// Overtake-tool backstop: if any client is viewing the Tool tab and an
		// overtake tool is active, re-read its "overtake_dsp:state"
		// and fan out. (The web UI's own re-subscribe poll drives the fast live
		// sync; this 30s loop just catches drift.)
		if toolClients := ru.subscribedToolClients(); len(toolClients) > 0 {
			if toolID, ok, err := shm.TryGetParam(0, overtakeParamPrefix+"module_id"); ok && err == nil && toolID != "" {
				if params, hit := ru.fetchAllParams(0, "overtake_dsp"); hit {
					for _, c := range toolClients {
						ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: 0, Params: params})
					}
				}
			}
		}
	}
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
		case "subscribe_tool":
			ru.handleSubscribeTool(ctx, c)
		case "unsubscribe_tool":
			ru.handleUnsubscribeTool(c)
		case "refetch_tool":
			ru.handleRefetchTool(ctx, c)
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

	// Give the synth priority: send its custom_ui (if any) AND its hierarchy +
	// chain_params BEFORE the (potentially multi-second) sendSlotSettings retry
	// block. A data-driven custom UI (e.g. jv880 builds its controls from
	// chain_params) then has its metadata on the first request and renders
	// immediately instead of polling for it; embedded-layout UIs just start
	// loading sooner. The iframe seeds values from the parent cache on subscribe.
	if synthID, err := ru.shm.GetParam(slot, "synth_module"); err == nil && synthID != "" {
		if url := ru.findModuleWebUI(synthID); url != "" {
			ru.sendCustomUI(ctx, c, slot, "synth", url)
		}
		ru.sendHierarchy(ctx, c, slot, "synth")
		ru.sendChainParams(ctx, c, slot, "synth")
	}

	// Send slot-level settings + mapped knobs FIRST so the top of the UI
	// populates immediately — otherwise they'd be blocked behind potentially
	// many seconds of sendInitialParamValues for param-heavy modules.
	ru.sendSlotSettings(ctx, c, slot)

	// Send hierarchy and chain_params for the remaining components, plus initial
	// param values for every component (synth metadata already sent above).
	for _, comp := range componentPrefixes {
		moduleKey := comp + "_module"
		modID, err := ru.shm.GetParam(slot, moduleKey)
		if err != nil || modID == "" {
			continue
		}
		if comp != "synth" {
			ru.sendHierarchy(ctx, c, slot, comp)
			ru.sendChainParams(ctx, c, slot, comp)
		}
		// Fetch initial param values (one-time on subscribe).
		ru.sendInitialParamValues(ctx, c, slot, comp)
	}
}

// slotSettingKeys are the slot-level params to send to the web UI.
var slotSettingKeys = []string{
	"slot:volume", "slot:muted", "slot:soloed",
	"slot:receive_channel", "slot:forward_channel",
	"lfo1:enabled", "lfo1:shape", "lfo1:shape_name", "lfo1:rate_hz", "lfo1:rate_div",
	"lfo1:sync", "lfo1:depth", "lfo1:polarity", "lfo1:target", "lfo1:target_param",
	"lfo2:enabled", "lfo2:shape", "lfo2:shape_name", "lfo2:rate_hz", "lfo2:rate_div",
	"lfo2:sync", "lfo2:depth", "lfo2:polarity", "lfo2:target", "lfo2:target_param",
}

func (ru *RemoteUI) sendSlotSettings(ctx context.Context, c *ruClient, slot uint8) {
	params := make(map[string]string)

	// Slot settings — use getParamWithRetry so a transient busy shm channel
	// during subscribe doesn't leave dropdowns (recv/fwd channel etc.) stuck
	// defaulted to their first option instead of the real device value.
	for _, key := range slotSettingKeys {
		val := ru.getParamWithRetry(slot, key, 3)
		if val != "" {
			params[key] = val
		}
	}

	// Knob mappings (knob_1_name..knob_8_name, knob_1_value..knob_8_value)
	for i := 1; i <= 8; i++ {
		nameKey := fmt.Sprintf("knob_%d_name", i)
		valKey := fmt.Sprintf("knob_%d_value", i)
		if name := ru.getParamWithRetry(slot, nameKey, 2); name != "" {
			params[nameKey] = name
		}
		if val := ru.getParamWithRetry(slot, valKey, 2); val != "" {
			params[valKey] = val
		}
	}

	if len(params) > 0 {
		ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: slot, Params: params})
	}
}

// sendInitialParamValues sends all current param values for a component.
// Prefers the module's "all" key (single shm call returning a JSON object of
// every value) for speed; falls back to fetching each chain_params entry
// individually when "all" isn't supported.
func (ru *RemoteUI) sendInitialParamValues(ctx context.Context, c *ruClient, slot uint8, comp string) {
	// Fetch hierarchy-level params first so the preset browser (count/name)
	// populates BEFORE the slower value fetch — otherwise the user sees
	// "1/0 Preset 0" until all individual params arrive.
	ru.sendHierarchyParams(ctx, c, slot, comp)

	// Fast path: "all" returns every param in one round-trip.
	if ru.sendAllParamsAtOnce(ctx, c, slot, comp) {
		return
	}

	raw, err := ru.shm.GetParam(slot, comp+":chain_params")
	if err != nil || raw == "" {
		return
	}
	var params []chainParam
	if json.Unmarshal([]byte(raw), &params) != nil {
		return
	}

	ru.logger.Info("initial params: fetching", "slot", slot, "comp", comp, "count", len(params))

	// Fetch in batches of 8, yielding between batches so shadow_ui.js
	// gets time on the shared param channel. Send each batch immediately
	// so the browser populates progressively.
	const batchSize = 8
	batch := make(map[string]string, batchSize)
	fetched := 0

	for i, p := range params {
		if p.Key == "" {
			continue
		}
		fullKey := comp + ":" + p.Key
		val, err := ru.shm.GetParam(slot, fullKey)
		if err != nil {
			continue
		}
		batch[fullKey] = val
		fetched++

		// Send batch and yield
		if len(batch) >= batchSize || i == len(params)-1 {
			if len(batch) > 0 {
				ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: slot, Params: batch})
				batch = make(map[string]string, batchSize)
			}
			// Yield to let shadow_ui.js use the param channel
			time.Sleep(20 * time.Millisecond)
		}
	}

	ru.logger.Info("initial params: done", "slot", slot, "comp", comp, "fetched", fetched)
}

// fetchAllParams reads the plugin's "state" key (a JSON object of every param
// value used for save/restore) and flattens it into a param_update payload.
// Returns (nil, false) if the module doesn't support "state". This is the
// expensive shm read, factored out so a single read can fan out to multiple
// subscribers (see broadcastInitialParamValues).
func (ru *RemoteUI) fetchAllParams(slot uint8, comp string) (map[string]string, bool) {
	raw, err := ru.shm.GetParam(slot, comp+":state")
	if err != nil || raw == "" || raw[0] != '{' {
		return nil, false
	}
	var values map[string]any
	if json.Unmarshal([]byte(raw), &values) != nil {
		return nil, false
	}
	params := make(map[string]string, len(values))
	for k, v := range values {
		switch tv := v.(type) {
		case string:
			params[comp+":"+k] = tv
		case float64:
			params[comp+":"+k] = strconv.FormatFloat(tv, 'f', -1, 64)
		case bool:
			if tv {
				params[comp+":"+k] = "1"
			} else {
				params[comp+":"+k] = "0"
			}
		default:
			// Skip null/array/object fields
		}
	}
	if len(params) == 0 {
		return nil, false
	}
	return params, true
}

// sendAllParamsAtOnce sends a component's full param set to one client in a
// single param_update. Returns true on success. Modules with many params
// (e.g. Surge ~280) go from ~10s of fetches to one shm round-trip.
func (ru *RemoteUI) sendAllParamsAtOnce(ctx context.Context, c *ruClient, slot uint8, comp string) bool {
	params, ok := ru.fetchAllParams(slot, comp)
	if !ok {
		return false
	}
	ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: slot, Params: params})
	ru.logger.Info("initial params: sent via 'state'", "slot", slot, "comp", comp, "count", len(params))
	return true
}

// broadcastInitialParamValues sends a component's full param set to every
// subscriber of a slot, reading shared memory ONCE and fanning the result out —
// instead of re-reading per client. Avoids redundant heavy shm reads (e.g. the
// jv880 365-param "state" blob) when multiple clients view the same slot, such
// as a popped-out window alongside the main tab. Falls back to the per-client
// streaming path for modules that don't support the "state" fast path.
func (ru *RemoteUI) broadcastInitialParamValues(ctx context.Context, slot uint8, comp string, clients []*ruClient) {
	if len(clients) == 0 {
		return
	}
	hierParams := ru.fetchHierarchyParams(slot, comp)
	allParams, ok := ru.fetchAllParams(slot, comp)
	if !ok {
		// No "state" fast path — fall back to per-client streaming (unchanged).
		for _, c := range clients {
			ru.sendInitialParamValues(ctx, c, slot, comp)
		}
		return
	}
	for _, c := range clients {
		if len(hierParams) > 0 {
			ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: slot, Params: hierParams})
		}
		ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: slot, Params: allParams})
	}
	ru.logger.Info("initial params: sent via 'state' (coalesced)", "slot", slot, "comp", comp, "count", len(allParams), "clients", len(clients))
}

// fetchHierarchyParams reads the preset-browser params (count_param, list_param,
// name_param) referenced by the ui_hierarchy and returns their values. Factored
// out of sendHierarchyParams so the read can fan out to multiple subscribers.
func (ru *RemoteUI) fetchHierarchyParams(slot uint8, comp string) map[string]string {
	raw, err := ru.shm.GetParam(slot, comp+":ui_hierarchy")
	if err != nil || raw == "" {
		return nil
	}

	// Parse just enough to extract level params
	var hier struct {
		Levels map[string]struct {
			ListParam  string `json:"list_param"`
			CountParam string `json:"count_param"`
			NameParam  string `json:"name_param"`
		} `json:"levels"`
	}
	if json.Unmarshal([]byte(raw), &hier) != nil {
		return nil
	}

	params := make(map[string]string)
	seen := make(map[string]bool)
	for _, level := range hier.Levels {
		for _, key := range []string{level.ListParam, level.CountParam, level.NameParam} {
			if key == "" || seen[key] {
				continue
			}
			seen[key] = true
			fullKey := comp + ":" + key
			if val, err := ru.shm.GetParam(slot, fullKey); err == nil && val != "" {
				params[fullKey] = val
			}
		}
	}
	return params
}

// sendHierarchyParams extracts preset-browser params (count_param, list_param,
// name_param) from the ui_hierarchy and sends their values to one client.
func (ru *RemoteUI) sendHierarchyParams(ctx context.Context, c *ruClient, slot uint8, comp string) {
	params := ru.fetchHierarchyParams(slot, comp)
	if len(params) > 0 {
		ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: slot, Params: params})
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
		return
	}

	// After setting a preset-related param, re-read all component params
	// plus hierarchy params (preset name/count) and push to all subscribers.
	// A preset change reshuffles every param inside the module, so we need
	// to refetch the full set — not just the preset metadata.
	parts := strings.SplitN(msg.Key, ":", 2)
	if len(parts) == 2 {
		comp := parts[0]
		paramKey := parts[1]
		if paramKey == "preset" || paramKey == "preset_index" || strings.HasSuffix(paramKey, "_index") {
			go func() {
				time.Sleep(50 * time.Millisecond) // Let the plugin process the change
				// Read shm once and fan out to all subscribers of this slot.
				ru.broadcastInitialParamValues(ctx, slot, comp, ru.subscribedClients(slot))
			}()
		}
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
		moduleKey := "master_fx:" + fxSlot + ":module"
		modID, err := ru.shm.GetParam(0, moduleKey)
		if err != nil || modID == "" {
			continue
		}
		compName := "master_fx:" + fxSlot
		ru.sendHierarchy(ctx, c, 0, compName)
		ru.sendChainParams(ctx, c, 0, compName)
		ru.sendInitialParamValues(ctx, c, 0, compName)
	}
}

func (ru *RemoteUI) handleUnsubscribeMasterFx(c *ruClient) {
	c.mu.Lock()
	c.masterFxSub = false
	c.mu.Unlock()
	ru.logger.Info("ws unsubscribe master_fx")
}

// handleSubscribeTool serves the active overtake tool's remote UI. An overtake
// tool occupies no chain slot — its DSP is dlopen'd in the shim
// as overtake_dsp_gen_inst and addressed via the "overtake_dsp:" prefix. We
// discover it by probing overtake_dsp:module_id, serve its web_ui.html under
// the "tool" component, and seed values from overtake_dsp:state. A tool_info
// with id=="" tells the Tool tab that nothing is loaded.
func (ru *RemoteUI) handleSubscribeTool(ctx context.Context, c *ruClient) {
	c.mu.Lock()
	c.toolSub = true
	c.toolSynced = false // force the next poll to do a full fetch
	c.mu.Unlock()

	ru.logger.Info("ws subscribe tool")

	if !ru.requireShm(ctx, c) {
		return
	}

	toolID := ru.activeOvertakeToolID(0)
	ru.writeJSON(ctx, c, wsToolInfo{Type: "tool_info", ID: toolID})
	if toolID == "" {
		return
	}
	if url := ru.findModuleWebUI(toolID); url != "" {
		ru.sendCustomUI(ctx, c, 0, "tool", url)
	}
	// Seed the tool's params from overtake_dsp:state (flat snapshot fields).
	if params, ok := ru.fetchAllParams(0, "overtake_dsp"); ok {
		ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: 0, Params: params})
	}
	ru.logger.Info("ws subscribe: served overtake tool remote UI", "tool", toolID)
}

func (ru *RemoteUI) handleUnsubscribeTool(c *ruClient) {
	c.mu.Lock()
	c.toolSub = false
	c.toolSynced = false
	c.mu.Unlock()
	ru.logger.Info("ws unsubscribe tool")
}

// handleRefetchTool picks up device-side changes the web UI polls for (playhead,
// external edits) that don't notify. Rev-gated: it first reads the tool's CHEAP
// "rui_poll" digest (rev:on:tick:bpm, no note serialization) and only does the
// heavy full "state" read (fetchAllParams) when rev changes (a content edit).
// While playing with no edit it pushes only the moving playhead; when nothing
// changed it does no work. Params ONLY — never custom_ui/tool_info (which would
// reload the iframe).
func (ru *RemoteUI) handleRefetchTool(ctx context.Context, c *ruClient) {
	// Safety belt: only poll the device for a client actually viewing the Tool tab.
	if !c.toolSub {
		return
	}
	if !ru.requireShm(ctx, c) {
		return
	}
	digest, ok, err := ru.shm.TryGetParam(0, overtakeParamPrefix+"rui_poll")
	if !ok || err != nil || digest == "" {
		// Tool predates the cheap digest key — fall back to a full fetch.
		if params, hit := ru.fetchAllParams(0, "overtake_dsp"); hit {
			ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: 0, Params: params})
		}
		return
	}
	rev, on, tick, bpm := parseRuiPoll(digest)
	if !c.toolSynced || rev != c.toolLastRev {
		// Content changed (or first sync) → full snapshot.
		if params, hit := ru.fetchAllParams(0, "overtake_dsp"); hit {
			ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: 0, Params: params})
			c.toolSynced = true
			c.toolLastRev = rev
			c.toolLastTick = tick
		}
		return
	}
	// No content change → push only the playhead while playing (cheap).
	if on && tick != c.toolLastTick {
		c.toolLastTick = tick
		ru.writeJSON(ctx, c, wsParamUpdate{Type: "param_update", Slot: 0,
			Params: map[string]string{
				overtakeParamPrefix + "rui_play": fmt.Sprintf("%d:%d:%d", boolToInt(on), tick, bpm),
			}})
	}
}

// parseRuiPoll parses the overtake tool's cheap "rev:on:tick:bpm" poll digest.
func parseRuiPoll(s string) (rev int64, on bool, tick int64, bpm int64) {
	p := strings.Split(s, ":")
	if len(p) > 0 {
		rev, _ = strconv.ParseInt(p[0], 10, 64)
	}
	if len(p) > 1 {
		v, _ := strconv.Atoi(p[1])
		on = v != 0
	}
	if len(p) > 2 {
		tick, _ = strconv.ParseInt(p[2], 10, 64)
	}
	if len(p) > 3 {
		bpm, _ = strconv.ParseInt(p[3], 10, 64)
	}
	return
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
		moduleKey := "master_fx:" + fxSlot + ":module"
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
	raw := ru.getParamWithRetry(slot, component+":ui_hierarchy", 3)
	if raw == "" {
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
	raw := ru.getParamWithRetry(slot, component+":chain_params", 3)
	if raw == "" {
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

// getParamWithRetry retries GetParam up to maxRetries times with a short delay.
// Handles large responses (chain_params, ui_hierarchy) that may timeout on first attempt.
func (ru *RemoteUI) getParamWithRetry(slot uint8, key string, maxRetries int) string {
	for i := 0; i < maxRetries; i++ {
		raw, err := ru.shm.GetParam(slot, key)
		if err == nil && raw != "" {
			return raw
		}
		if i < maxRetries-1 {
			ru.logger.Debug("getParam retry", "slot", slot, "key", key, "attempt", i+1, "err", err)
			time.Sleep(100 * time.Millisecond)
		}
	}
	ru.logger.Debug("getParam failed after retries", "slot", slot, "key", key)
	return ""
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

	// When a component's preset (list_param) changes, the notify ring only
	// carries the numeric index — not the new preset's param VALUES or its name
	// string. So we re-push that component's full state (+ preset-browser
	// params). Throttled per slot (leading edge fires immediately; trailing
	// change still lands after the throttle) so quick preset scrolling doesn't
	// flood the shared param channel and stall sync.
	const presetResendThrottle = 120 * time.Millisecond
	lastResend := make(map[uint8]time.Time)
	pendingResend := make(map[uint8]map[string]bool) // slot -> comp -> needs full re-send
	lastPreset := make(map[uint8]map[string]string)  // slot -> comp -> last preset value seen
	// Master FX lives at slot 0 but is delivered to a separate subscription
	// (masterFxSub), so it gets its own pending set + throttle keyed by comp
	// ("master_fx:fxN") rather than sharing the chain-slot maps above.
	var lastResendMasterFx time.Time
	pendingMasterFxResend := make(map[string]bool) // comp -> needs full re-send

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
		if len(changes) == 0 && len(pendingResend) == 0 && len(pendingMasterFxResend) == 0 {
			continue
		}

		// Snapshot subscribers once per active tick.
		ru.mu.Lock()
		clients := make([]*ruClient, 0, len(ru.clients))
		for c := range ru.clients {
			clients = append(clients, c)
		}
		ru.mu.Unlock()

		if len(changes) > 0 {
			// Group changes by slot, splitting master FX keys (slot 0,
			// "master_fx:" prefix) into their own bucket so master-FX-only
			// subscribers receive them without needing a slot 0 subscription.
			slotChanges := make(map[uint8]map[string]string)
			masterFxChanges := make(map[string]string)
			for _, c := range changes {
				if c.Slot == 0 && strings.HasPrefix(c.Key, "master_fx:") {
					masterFxChanges[c.Key] = c.Value
					// A device-initiated master-FX preset load reshuffles every
					// param internally; the notify ring carries only the index,
					// so mark the comp ("master_fx:fxN") for a full re-send.
					if i := strings.LastIndex(c.Key, ":"); i >= 0 && c.Key[i+1:] == "preset" {
						pendingMasterFxResend[c.Key[:i]] = true
					}
					continue
				}
				m, ok := slotChanges[c.Slot]
				if !ok {
					m = make(map[string]string)
					slotChanges[c.Slot] = m
				}
				m[c.Key] = c.Value
				if i := strings.LastIndex(c.Key, ":"); i >= 0 && c.Key[i+1:] == "preset" {
					// Only re-push full state when the preset VALUE actually
					// changed. Some modules (e.g. jv880) re-assert preset on the
					// notify ring without a real change; without this guard each
					// such notification triggers a full N-param re-fetch, which
					// floods the channel and makes sync feel inconsistent.
					comp := c.Key[:i]
					if lastPreset[c.Slot] == nil {
						lastPreset[c.Slot] = make(map[string]string)
					}
					if prev, seen := lastPreset[c.Slot][comp]; !seen || prev != c.Value {
						lastPreset[c.Slot][comp] = c.Value
						if pendingResend[c.Slot] == nil {
							pendingResend[c.Slot] = make(map[string]bool)
						}
						pendingResend[c.Slot][comp] = true
					}
				}
			}

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

			if len(masterFxChanges) > 0 {
				update := wsParamUpdate{Type: "param_update", Slot: 0, Params: masterFxChanges}
				for _, c := range clients {
					c.mu.Lock()
					subscribed := c.masterFxSub
					c.mu.Unlock()
					if subscribed {
						ru.writeJSON(ctx, c, update)
					}
				}
			}
		}

		// Flush throttled full-state re-sends for components whose preset
		// changed. sendInitialParamValues pushes name/count + every value, so
		// the browser's knobs/sliders catch up to the new preset.
		//
		// The send is offloaded to a goroutine: for a param-heavy module
		// without the :state fast path, sendInitialParamValues sleeps ~20ms
		// per 8 params (hundreds of ms total). Running it inline would block
		// this 5ms drain loop, overflowing the 64-entry shim ring and dropping
		// live knob updates. The throttle keys off dispatch time (lastResend
		// set before launch), so rapid preset scrolling still coalesces.
		if len(pendingResend) > 0 {
			now := time.Now()
			for slot, comps := range pendingResend {
				if now.Sub(lastResend[slot]) < presetResendThrottle {
					continue
				}
				lastResend[slot] = now
				delete(pendingResend, slot)
				// Coalesce the re-fetch: snapshot subscribers + comps, then read
				// shm once per comp and fan out to all of them (cdbc123d), while
				// keeping upstream's goroutine offload so the 5ms drain loop never
				// blocks on a param-heavy module's sendInitialParamValues.
				subs := ru.subscribedClients(slot)
				compList := make([]string, 0, len(comps))
				for comp := range comps {
					compList = append(compList, comp)
				}
				go func(slot uint8, comps []string, subs []*ruClient) {
					for _, comp := range comps {
						ru.broadcastInitialParamValues(ctx, slot, comp, subs)
					}
				}(slot, compList, subs)
			}
		}

		// Same treatment for master-FX preset changes, delivered to the
		// separate master-FX subscription and throttled independently.
		if len(pendingMasterFxResend) > 0 {
			now := time.Now()
			if now.Sub(lastResendMasterFx) >= presetResendThrottle {
				lastResendMasterFx = now
				compList := make([]string, 0, len(pendingMasterFxResend))
				for comp := range pendingMasterFxResend {
					compList = append(compList, comp)
				}
				pendingMasterFxResend = make(map[string]bool)
				go func(comps []string) {
					for _, comp := range comps {
						for _, c := range ru.masterFxSubscribedClients() {
							ru.sendInitialParamValues(ctx, c, 0, comp)
						}
					}
				}(compList)
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
		moduleKey := "master_fx:" + fxSlot + ":module"

		modID, ok, err := ru.shm.TryGetParam(0, moduleKey)
		if !ok || err != nil {
			// Mutex busy or a transient shm read error (e.g. shadow_param
			// timeout under load). TryGetParam returns ("", true, err) in the
			// error case — without this guard an empty modID is mistaken for a
			// module unload and we broadcast "no effects loaded", then restore
			// it the next tick (visible flicker). Mirrors pollSlot. Skip tick.
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

func (ru *RemoteUI) subscribedToolClients() []*ruClient {
	ru.mu.Lock()
	defer ru.mu.Unlock()
	var out []*ruClient
	for c := range ru.clients {
		c.mu.Lock()
		sub := c.toolSub
		c.mu.Unlock()
		if sub {
			out = append(out, c)
		}
	}
	return out
}
