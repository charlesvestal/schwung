package main

// controls.go -- the web editor for the active set's CONTROL DOCUMENT
// (set_state/<uuid>/controls.json): the Custom surface layout's pages and the
// generic CC map's bindings. The shadow UI owns the document on the device;
// see docs/superpowers/specs/2026-09-26-custom-surface-layout-design.md.
//
// The page does not re-implement the document. It imports the device's OWN
// control_map.mjs / control_target.mjs (served below from the installed
// shared/ directory), so a target the browser writes is validated by the same
// code that will load it. This file only reads, writes and describes.
//
// CONCURRENCY: the device edits the same file (learn, the on-device editors).
// Every write carries the ETag of the text it was based on and is refused with
// 409 if the file moved underneath it; the page then reloads. The shadow UI
// picks a web write up within a second (control_host.mjs reconcile).

import (
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

const (
	controlsFile      = "controls.json"
	controlsMaxBytes  = 256 * 1024
	controlsSlots     = 4
	controlsMasterFx  = 8
	controlsMaxFxRead = 8
)

// The shared ES modules the editor imports. A fixed list: this route serves
// nothing else from disk.
var controlsSharedJS = map[string]bool{
	"control_map.mjs":    true,
	"control_target.mjs": true,
	"control_picker.mjs": true,
}

// controlsParamGetter is the param channel as this file uses it, so tests can
// fake it. *ShmParams satisfies it.
type controlsParamGetter interface {
	GetParam(slot uint8, key string) (string, error)
}

type controlsSet struct {
	UUID string `json:"uuid"`
	Name string `json:"name"`
	dir  string
}

type controlsSlot struct {
	Synth  string   `json:"synth"`
	FX     []string `json:"fx"`
	MidiFX []string `json:"midiFx"`
}

type controlsChain struct {
	// Live: read from the running device. Otherwise from the set's saved
	// files, which can lag a change made in the last few seconds.
	Live     bool           `json:"live"`
	Slots    []controlsSlot `json:"slots"`
	MasterFX []string       `json:"masterFx"`
}

type controlsDoc struct {
	Set   controlsSet   `json:"set"`
	Text  string        `json:"text"`
	ETag  string        `json:"etag"`
	Chain controlsChain `json:"chain"`
}

func controlsETag(text []byte) string {
	sum := sha1.Sum(text)
	return hex.EncodeToString(sum[:])
}

// activeControlsSet names the set the device has loaded and its state dir.
func activeControlsSet(basePath string) (controlsSet, error) {
	raw, err := os.ReadFile(filepath.Join(basePath, "active_set.txt"))
	if err != nil {
		return controlsSet{}, err
	}
	lines := strings.Split(string(raw), "\n")
	uuid := strings.TrimSpace(lines[0])
	if uuid == "" || strings.ContainsAny(uuid, "/\\") || uuid == "." || uuid == ".." {
		return controlsSet{}, errors.New("active_set.txt names no set")
	}
	set := controlsSet{UUID: uuid, dir: filepath.Join(basePath, "set_state", uuid)}
	if len(lines) > 1 {
		set.Name = strings.TrimSpace(lines[1])
	}
	info, err := os.Stat(set.dir)
	if err != nil {
		return controlsSet{}, err
	}
	if !info.IsDir() {
		return controlsSet{}, errors.New("set_state/" + uuid + " is not a directory")
	}
	return set, nil
}

// readControlsText returns the document text ("" when there is none yet).
func readControlsText(dir string) ([]byte, error) {
	b, err := os.ReadFile(filepath.Join(dir, controlsFile))
	if errors.Is(err, os.ErrNotExist) {
		return []byte{}, nil
	}
	return b, err
}

// liveControlsChain asks the running device what each position holds. Any
// failed read makes the whole answer unusable (ok=false): a partial chain
// would show a loaded module as absent, which is worse than falling back.
func liveControlsChain(p controlsParamGetter) (controlsChain, bool) {
	ch := controlsChain{Live: true}
	get := func(slot uint8, key string) (string, bool) {
		v, err := p.GetParam(slot, key)
		return strings.TrimSpace(v), err == nil
	}
	count := func(slot uint8, key string) (int, bool) {
		v, ok := get(slot, key)
		if !ok {
			return 0, false
		}
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 {
			return 0, v == ""
		}
		if n > controlsMaxFxRead {
			n = controlsMaxFxRead
		}
		return n, true
	}
	for s := uint8(0); s < controlsSlots; s++ {
		var sl controlsSlot
		var ok bool
		if sl.Synth, ok = get(s, "synth_module"); !ok {
			return ch, false
		}
		n, ok := count(s, "fx_count")
		if !ok {
			return ch, false
		}
		for i := 1; i <= n; i++ {
			v, ok := get(s, "fx"+strconv.Itoa(i)+"_module")
			if !ok {
				return ch, false
			}
			sl.FX = append(sl.FX, v)
		}
		if n, ok = count(s, "midi_fx_count"); !ok {
			return ch, false
		}
		for i := 1; i <= n; i++ {
			v, ok := get(s, "midi_fx"+strconv.Itoa(i)+"_module")
			if !ok {
				return ch, false
			}
			sl.MidiFX = append(sl.MidiFX, v)
		}
		ch.Slots = append(ch.Slots, sl)
	}
	raw, err := p.GetParam(0, "master_fx:modules")
	if err != nil {
		return ch, false
	}
	var mfx []struct {
		ID string `json:"id"`
	}
	if json.Unmarshal([]byte(raw), &mfx) != nil {
		return ch, false
	}
	ch.MasterFX = make([]string, controlsMasterFx)
	for i := 0; i < len(mfx) && i < controlsMasterFx; i++ {
		ch.MasterFX[i] = masterFxModuleID(mfx[i].ID)
	}
	return ch, true
}

// savedControlsChain is the fallback: what the set's own files say.
func savedControlsChain(basePath string) controlsChain {
	ch := controlsChain{MasterFX: make([]string, controlsMasterFx)}
	st, err := readSetState(basePath)
	for s := 0; s < controlsSlots; s++ {
		var sl controlsSlot
		if err == nil {
			sl.Synth = st.Slots[s].Synth
			sl.FX = append(sl.FX, st.Slots[s].AudioFX...)
			sl.MidiFX = append(sl.MidiFX, st.Slots[s].MidiFX...)
		}
		ch.Slots = append(ch.Slots, sl)
	}
	if err == nil {
		for i := 0; i < controlsMasterFx && i < len(st.MasterFX); i++ {
			ch.MasterFX[i] = st.MasterFX[i]
		}
	}
	return ch
}

func (app *App) controlsParams() controlsParamGetter {
	if p := app.params(); p != nil {
		return p
	}
	return nil
}

func controlsJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

func controlsError(w http.ResponseWriter, status int, msg string) {
	controlsJSON(w, status, map[string]string{"error": msg})
}

// handleControls renders the page shell; everything else is fetched.
func (app *App) handleControls(w http.ResponseWriter, r *http.Request) {
	app.render(w, r, "controls.html", map[string]any{
		"Title":  "Controls",
		"Active": "controls",
	})
}

// loadControlsDoc assembles GET /api/controls.
func (app *App) loadControlsDoc(p controlsParamGetter) (controlsDoc, error) {
	set, err := activeControlsSet(app.basePath)
	if err != nil {
		return controlsDoc{}, err
	}
	text, err := readControlsText(set.dir)
	if err != nil {
		return controlsDoc{}, err
	}
	doc := controlsDoc{Set: set, Text: string(text), ETag: controlsETag(text)}
	live := false
	if p != nil {
		doc.Chain, live = liveControlsChain(p)
	}
	if !live {
		doc.Chain = savedControlsChain(app.basePath)
	}
	return doc, nil
}

func (app *App) handleControlsGet(w http.ResponseWriter, r *http.Request) {
	doc, err := app.loadControlsDoc(app.controlsParams())
	if err != nil {
		controlsError(w, http.StatusServiceUnavailable, "no active set: "+err.Error())
		return
	}
	controlsJSON(w, http.StatusOK, doc)
}

type controlsPut struct {
	UUID string `json:"uuid"`
	ETag string `json:"etag"`
	Text string `json:"text"`
}

// handleControlsPut writes the document IF it is still the one the page read.
func (app *App) handleControlsPut(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(io.LimitReader(r.Body, controlsMaxBytes+1))
	if err != nil || len(body) > controlsMaxBytes {
		controlsError(w, http.StatusRequestEntityTooLarge, "document too large")
		return
	}
	var req controlsPut
	if json.Unmarshal(body, &req) != nil {
		controlsError(w, http.StatusBadRequest, "bad request")
		return
	}
	// The text must be a JSON object. The device's parser is tolerant of
	// what is INSIDE; it refuses (and never overwrites) what is not an object.
	var probe map[string]any
	if json.Unmarshal([]byte(req.Text), &probe) != nil {
		controlsError(w, http.StatusBadRequest, "document is not a JSON object")
		return
	}
	set, err := activeControlsSet(app.basePath)
	if err != nil {
		controlsError(w, http.StatusServiceUnavailable, "no active set: "+err.Error())
		return
	}
	if req.UUID != set.UUID {
		controlsError(w, http.StatusConflict, "the device changed set")
		return
	}
	cur, err := readControlsText(set.dir)
	if err != nil {
		controlsError(w, http.StatusInternalServerError, "read failed")
		return
	}
	if controlsETag(cur) != req.ETag {
		controlsError(w, http.StatusConflict, "changed on the device")
		return
	}
	next := []byte(req.Text)
	if err := writeControlsAtomic(set.dir, next); err != nil {
		app.logger.Error("controls write", "err", err)
		controlsError(w, http.StatusInternalServerError, "write failed")
		return
	}
	controlsJSON(w, http.StatusOK, map[string]string{"etag": controlsETag(next)})
}

// writeControlsAtomic replaces the file by rename, so the device never reads
// half a document.
func writeControlsAtomic(dir string, text []byte) error {
	tmp, err := os.CreateTemp(dir, ".controls-*.tmp")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name)
	if _, err := tmp.Write(text); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Chmod(name, 0o644); err != nil {
		return err
	}
	return os.Rename(name, filepath.Join(dir, controlsFile))
}

// handleControlsParams answers a module's chain_params, for the picker.
// ?slot=0..3&comp=synth|fxN|midi_fxN, or ?fx=1..8 for Master FX.
func (app *App) handleControlsParams(w http.ResponseWriter, r *http.Request) {
	p := app.controlsParams()
	if p == nil {
		controlsError(w, http.StatusServiceUnavailable, "the device is not answering")
		return
	}
	slot, key, ok := controlsParamsKey(r.URL.Query())
	if !ok {
		controlsError(w, http.StatusBadRequest, "bad position")
		return
	}
	raw := ""
	for i := 0; i < 3; i++ {
		v, err := p.GetParam(slot, key)
		if err == nil && strings.TrimSpace(v) != "" {
			raw = v
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	var js json.RawMessage
	if raw == "" || json.Unmarshal([]byte(raw), &js) != nil {
		controlsError(w, http.StatusBadGateway, "the module did not describe its parameters")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.Write(js)
}

// controlsParamsKey validates the query into a param-channel address.
func controlsParamsKey(q map[string][]string) (uint8, string, bool) {
	first := func(k string) string {
		if v := q[k]; len(v) > 0 {
			return v[0]
		}
		return ""
	}
	if f := first("fx"); f != "" {
		n, err := strconv.Atoi(f)
		if err != nil || n < 1 || n > controlsMasterFx {
			return 0, "", false
		}
		return 0, "master_fx:fx" + strconv.Itoa(n) + ":chain_params", true
	}
	s, err := strconv.Atoi(first("slot"))
	if err != nil || s < 0 || s >= controlsSlots {
		return 0, "", false
	}
	comp := first("comp")
	if !controlsComponentOK(comp) {
		return 0, "", false
	}
	return uint8(s), comp + ":chain_params", true
}

func controlsComponentOK(c string) bool {
	if c == "synth" {
		return true
	}
	for _, pre := range []string{"midi_fx", "fx"} {
		if strings.HasPrefix(c, pre) {
			n, err := strconv.Atoi(c[len(pre):])
			return err == nil && n >= 1 && n <= controlsMaxFxRead
		}
	}
	return false
}

// handleControlsJS serves the two shared modules the page imports, from the
// installed shared/ directory -- the same files the device runs.
func (app *App) handleControlsJS(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !controlsSharedJS[name] {
		http.NotFound(w, r)
		return
	}
	b, err := os.ReadFile(filepath.Join(app.basePath, "shared", name))
	if err != nil {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/javascript; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	w.Write(b)
}
