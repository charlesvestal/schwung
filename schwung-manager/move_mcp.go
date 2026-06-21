package main

import (
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	moveMCPModuleID        = "move-mcp"
	moveSetsDir            = "/data/UserData/UserLibrary/Sets"
	moveSamplesDir         = "/data/UserData/UserLibrary/Samples"
	moveSettings           = "/data/UserData/settings/Settings.json"
	moveMCPSampleIndexPath = "/data/UserData/schwung/move-mcp/sample-index.json"
)

type moveMCPConfig struct {
	Enabled      bool   `json:"enabled"`
	BindMode     string `json:"bind_mode"`
	RequireToken bool   `json:"require_token"`
	AllowRead    bool   `json:"allow_read"`
	AllowWrite   bool   `json:"allow_write"`
	AllowDelete  bool   `json:"allow_delete"`
	AllowActions bool   `json:"allow_actions"`
	MaxUploadMB  int    `json:"max_upload_mb"`
	PackRoot     string `json:"pack_root"`
	LogRequests  bool   `json:"log_requests"`
	TokenSet     bool   `json:"token_set"`
	token        string
}

type moveCurrentSet struct {
	UUID      string
	Name      string
	SongIndex int
	Path      string
	SongPath  string
	ModTime   time.Time
	Size      int64
}

type moveSong struct {
	Schema               string           `json:"$schema,omitempty"`
	StepEditorResolution string           `json:"stepEditorResolution,omitempty"`
	Tempo                float64          `json:"tempo,omitempty"`
	GlobalGrooveAmount   float64          `json:"globalGrooveAmount,omitempty"`
	TimeSignature        map[string]any   `json:"timeSignature,omitempty"`
	RootNote             int              `json:"rootNote,omitempty"`
	Scale                string           `json:"scale,omitempty"`
	MelodicLayout        string           `json:"melodicLayout,omitempty"`
	Tracks               []moveTrack      `json:"tracks,omitempty"`
	ReturnTracks         []map[string]any `json:"returnTracks,omitempty"`
	MasterTrack          map[string]any   `json:"masterTrack,omitempty"`
	Scenes               []map[string]any `json:"scenes,omitempty"`
	Grooves              []map[string]any `json:"grooves,omitempty"`
	Metadata             map[string]any   `json:"metadata,omitempty"`
}

type moveTrack struct {
	Kind               string          `json:"kind,omitempty"`
	Name               string          `json:"name,omitempty"`
	Color              int             `json:"color,omitempty"`
	IsSelected         bool            `json:"isSelected,omitempty"`
	ClipSlots          []moveClipSlot  `json:"clipSlots,omitempty"`
	IsNoteRepeatOn     bool            `json:"isNoteRepeatOn,omitempty"`
	NoteRepeatRate     string          `json:"noteRepeatRate,omitempty"`
	NoteRepeatArpeggio map[string]any  `json:"noteRepeatArpeggio,omitempty"`
	UIOctaveIndex      int             `json:"uiOctaveIndex,omitempty"`
	MIDIInputMode      string          `json:"midiInputMode,omitempty"`
	MIDIOutputEndpoint any             `json:"midiOutputEndpoint,omitempty"`
	Devices            []moveDevice    `json:"devices,omitempty"`
	Mixer              map[string]any  `json:"mixer,omitempty"`
	Raw                json.RawMessage `json:"-"`
}

type moveClipSlot struct {
	HasStop bool      `json:"hasStop,omitempty"`
	Clip    *moveClip `json:"clip,omitempty"`
}

type moveClip struct {
	IsPlaying                bool             `json:"isPlaying,omitempty"`
	Name                     string           `json:"name,omitempty"`
	Color                    int              `json:"color,omitempty"`
	IsEnabled                bool             `json:"isEnabled,omitempty"`
	TimeSignature            map[string]any   `json:"timeSignature,omitempty"`
	Region                   moveClipRegion   `json:"region,omitempty"`
	GrooveID                 any              `json:"grooveId,omitempty"`
	StepEditorScrollPosition any              `json:"stepEditorScrollPosition,omitempty"`
	Notes                    []moveNote       `json:"notes,omitempty"`
	Envelopes                []map[string]any `json:"envelopes,omitempty"`
}

type moveClipRegion struct {
	Start float64         `json:"start"`
	End   float64         `json:"end"`
	Loop  moveClipLoop    `json:"loop"`
	Raw   json.RawMessage `json:"-"`
}

type moveClipLoop struct {
	Start     float64 `json:"start"`
	End       float64 `json:"end"`
	IsEnabled bool    `json:"isEnabled"`
}

type moveNote struct {
	NoteNumber  int                        `json:"noteNumber"`
	StartTime   float64                    `json:"startTime"`
	Duration    float64                    `json:"duration"`
	Velocity    float64                    `json:"velocity"`
	OffVelocity float64                    `json:"offVelocity"`
	Automations map[string][]moveAutoPoint `json:"automations,omitempty"`
}

type moveAutoPoint struct {
	Time  float64 `json:"time"`
	Value float64 `json:"value"`
}

type moveDevice struct {
	PresetURI        string            `json:"presetUri,omitempty"`
	Kind             string            `json:"kind,omitempty"`
	Name             string            `json:"name,omitempty"`
	LockID           any               `json:"lockId,omitempty"`
	LockSeal         any               `json:"lockSeal,omitempty"`
	Parameters       map[string]any    `json:"parameters,omitempty"`
	DeviceData       map[string]any    `json:"deviceData,omitempty"`
	Chains           []moveDeviceChain `json:"chains,omitempty"`
	ReturnChains     []moveDeviceChain `json:"returnChains,omitempty"`
	DrumZoneSettings map[string]any    `json:"drumZoneSettings,omitempty"`
}

type moveDeviceChain struct {
	Name             string            `json:"name,omitempty"`
	Devices          []moveDevice      `json:"devices,omitempty"`
	Chains           []moveDeviceChain `json:"chains,omitempty"`
	ReturnChains     []moveDeviceChain `json:"returnChains,omitempty"`
	DrumZoneSettings map[string]any    `json:"drumZoneSettings,omitempty"`
}

type moveMCPSetResponse struct {
	OK     bool               `json:"ok"`
	Set    moveMCPSetMeta     `json:"set"`
	Tracks []moveMCPTrackView `json:"tracks"`
	Scenes []map[string]any   `json:"scenes,omitempty"`
}

type moveMCPSetMeta struct {
	UUID                 string         `json:"uuid"`
	Name                 string         `json:"name"`
	SongIndex            int            `json:"song_index"`
	Path                 string         `json:"path"`
	SongPath             string         `json:"song_path"`
	Schema               string         `json:"schema,omitempty"`
	StepEditorResolution string         `json:"step_editor_resolution,omitempty"`
	Tempo                float64        `json:"tempo,omitempty"`
	GlobalGrooveAmount   float64        `json:"global_groove_amount,omitempty"`
	TimeSignature        map[string]any `json:"time_signature,omitempty"`
	RootNote             int            `json:"root_note,omitempty"`
	Scale                string         `json:"scale,omitempty"`
	MelodicLayout        string         `json:"melodic_layout,omitempty"`
	Modified             string         `json:"modified,omitempty"`
	Size                 int64          `json:"size,omitempty"`
}

type moveMCPTrackView struct {
	Index         int                    `json:"index"`
	Kind          string                 `json:"kind,omitempty"`
	Name          string                 `json:"name,omitempty"`
	Color         int                    `json:"color,omitempty"`
	IsSelected    bool                   `json:"is_selected,omitempty"`
	MIDIInputMode string                 `json:"midi_input_mode,omitempty"`
	UIOctaveIndex int                    `json:"ui_octave_index,omitempty"`
	Mixer         map[string]any         `json:"mixer,omitempty"`
	Devices       []moveMCPDeviceSummary `json:"devices,omitempty"`
	Clips         []moveMCPClipView      `json:"clips,omitempty"`
	ClipSlotCount int                    `json:"clip_slot_count"`
}

type moveMCPDeviceSummary struct {
	Kind             string                      `json:"kind,omitempty"`
	Name             string                      `json:"name,omitempty"`
	PresetURI        string                      `json:"preset_uri,omitempty"`
	LockID           any                         `json:"lock_id,omitempty"`
	LockSeal         any                         `json:"lock_seal,omitempty"`
	Parameters       map[string]any              `json:"parameters,omitempty"`
	ParameterCount   int                         `json:"parameter_count"`
	DeviceDataKeys   []string                    `json:"device_data_keys,omitempty"`
	Chains           []moveMCPDeviceChainSummary `json:"chains,omitempty"`
	ReturnChains     []moveMCPDeviceChainSummary `json:"return_chains,omitempty"`
	DrumZoneSettings map[string]any              `json:"drum_zone_settings,omitempty"`
}

type moveMCPDeviceChainSummary struct {
	Name             string                      `json:"name,omitempty"`
	Devices          []moveMCPDeviceSummary      `json:"devices,omitempty"`
	Chains           []moveMCPDeviceChainSummary `json:"chains,omitempty"`
	ReturnChains     []moveMCPDeviceChainSummary `json:"return_chains,omitempty"`
	DrumZoneSettings map[string]any              `json:"drum_zone_settings,omitempty"`
}

type moveMCPClipView struct {
	Scene                    int              `json:"scene"`
	HasStop                  bool             `json:"has_stop"`
	IsPlaying                bool             `json:"is_playing"`
	Name                     string           `json:"name,omitempty"`
	Color                    int              `json:"color,omitempty"`
	IsEnabled                bool             `json:"is_enabled"`
	TimeSignature            map[string]any   `json:"time_signature,omitempty"`
	Start                    float64          `json:"start"`
	End                      float64          `json:"end"`
	LoopStart                float64          `json:"loop_start"`
	LoopEnd                  float64          `json:"loop_end"`
	LoopEnabled              bool             `json:"loop_enabled"`
	GrooveID                 any              `json:"groove_id,omitempty"`
	StepEditorScrollPosition any              `json:"step_editor_scroll_position,omitempty"`
	Notes                    []moveNote       `json:"notes"`
	Envelopes                []map[string]any `json:"envelopes,omitempty"`
}

type moveMCPWriteClipRequest struct {
	Track     int              `json:"track"`
	Scene     int              `json:"scene"`
	Start     *float64         `json:"start,omitempty"`
	End       *float64         `json:"end,omitempty"`
	LoopStart *float64         `json:"loop_start,omitempty"`
	LoopEnd   *float64         `json:"loop_end,omitempty"`
	Name      string           `json:"name,omitempty"`
	Color     *int             `json:"color,omitempty"`
	IsPlaying *bool            `json:"is_playing,omitempty"`
	IsEnabled *bool            `json:"is_enabled,omitempty"`
	Notes     []moveNote       `json:"notes"`
	Envelopes []map[string]any `json:"envelopes,omitempty"`
	Replace   *bool            `json:"replace,omitempty"`
}

type moveMCPSongSettingsRequest struct {
	Tempo                *float64        `json:"tempo,omitempty"`
	GlobalGrooveAmount   *float64        `json:"global_groove_amount,omitempty"`
	RootNote             *int            `json:"root_note,omitempty"`
	Scale                *string         `json:"scale,omitempty"`
	MelodicLayout        *string         `json:"melodic_layout,omitempty"`
	StepEditorResolution *string         `json:"step_editor_resolution,omitempty"`
	TimeSignature        *map[string]any `json:"time_signature,omitempty"`
}

type moveMCPWriteTrackDevicesRequest struct {
	Track       int              `json:"track,omitempty"`
	SourceTrack *int             `json:"source_track,omitempty"`
	Devices     []map[string]any `json:"devices,omitempty"`
	TrackName   *string          `json:"track_name,omitempty"`
	TrackColor  *int             `json:"track_color,omitempty"`
}

type moveMCPWriteDeviceParametersRequest struct {
	DevicePath []int          `json:"device_path"`
	Parameters map[string]any `json:"parameters"`
}

type moveMCPSampleIndex struct {
	Schema      string              `json:"schema"`
	GeneratedAt string              `json:"generated_at"`
	SourceRoot  string              `json:"source_root,omitempty"`
	MoveRoot    string              `json:"move_root,omitempty"`
	Count       int                 `json:"count"`
	Samples     []moveMCPSampleItem `json:"samples"`
	Summary     map[string]any      `json:"summary,omitempty"`
	Analyzer    map[string]string   `json:"analyzer,omitempty"`
	Warnings    []string            `json:"warnings,omitempty"`
}

type moveMCPSampleItem struct {
	ID             string         `json:"id"`
	Path           string         `json:"path"`
	RelativePath   string         `json:"relative_path,omitempty"`
	Name           string         `json:"name"`
	Ext            string         `json:"ext,omitempty"`
	Size           int64          `json:"size,omitempty"`
	MTime          string         `json:"mtime,omitempty"`
	DurationSec    *float64       `json:"duration_sec,omitempty"`
	SampleRate     *int           `json:"sample_rate,omitempty"`
	Channels       *int           `json:"channels,omitempty"`
	BitDepth       *int           `json:"bit_depth,omitempty"`
	BPM            *float64       `json:"bpm,omitempty"`
	Key            string         `json:"key,omitempty"`
	Mode           string         `json:"mode,omitempty"`
	Tags           []string       `json:"tags,omitempty"`
	Mood           []string       `json:"mood,omitempty"`
	LoopBars       *float64       `json:"loop_bars,omitempty"`
	OneShot        *bool          `json:"one_shot,omitempty"`
	Analysis       map[string]any `json:"analysis,omitempty"`
	MetadataSource string         `json:"metadata_source,omitempty"`
}

type moveMCPSetListItem struct {
	UUID      string `json:"uuid"`
	Name      string `json:"name"`
	SongIndex int    `json:"song_index"`
	Path      string `json:"path"`
	SongPath  string `json:"song_path"`
	Modified  string `json:"modified,omitempty"`
	Size      int64  `json:"size,omitempty"`
	Current   bool   `json:"current"`
}

type moveMCPSwitchSetRequest struct {
	UUID      string `json:"uuid,omitempty"`
	Name      string `json:"name,omitempty"`
	SongIndex *int   `json:"song_index,omitempty"`
	SaveDirty *bool  `json:"save_dirty,omitempty"`
	Restart   *bool  `json:"restart,omitempty"`
}

type moveMCPDuplicateSetRequest struct {
	Name            string `json:"name"`
	SourceUUID      string `json:"source_uuid,omitempty"`
	SourceName      string `json:"source_name,omitempty"`
	SourceSongIndex *int   `json:"source_song_index,omitempty"`
	SongColor       *int   `json:"song_color,omitempty"`
	SelectNew       *bool  `json:"select_new,omitempty"`
	Restart         *bool  `json:"restart,omitempty"`
}

func (app *App) readMoveMCPConfig() moveMCPConfig {
	cfg := moveMCPConfig{
		Enabled:      false,
		BindMode:     "lan",
		RequireToken: true,
		AllowRead:    true,
		AllowWrite:   false,
		AllowDelete:  false,
		AllowActions: false,
		MaxUploadMB:  64,
		PackRoot:     filepath.Join(app.basePath, "move-mcp", "packs"),
		LogRequests:  true,
	}

	moduleDir := filepath.Join(app.basePath, "modules", "tools", moveMCPModuleID)
	if raw, err := os.ReadFile(filepath.Join(moduleDir, "config.json")); err == nil {
		_ = json.Unmarshal(raw, &cfg)
	}
	if tok, err := os.ReadFile(filepath.Join(moduleDir, "secrets", "access_token.txt")); err == nil {
		cfg.token = strings.TrimSpace(string(tok))
		cfg.TokenSet = cfg.token != ""
	}
	return cfg
}

func (app *App) requireMoveMCP(w http.ResponseWriter, r *http.Request, write bool) (moveMCPConfig, bool) {
	cfg := app.readMoveMCPConfig()
	if !cfg.Enabled {
		writeJSONError(w, http.StatusForbidden, "move-mcp disabled")
		return cfg, false
	}
	if write {
		if !cfg.AllowWrite {
			writeJSONError(w, http.StatusForbidden, "move-mcp write disabled")
			return cfg, false
		}
	} else if !cfg.AllowRead {
		writeJSONError(w, http.StatusForbidden, "move-mcp read disabled")
		return cfg, false
	}
	if cfg.RequireToken {
		token := bearerToken(r)
		if token == "" {
			token = r.Header.Get("X-Move-MCP-Token")
		}
		if token == "" {
			token = r.URL.Query().Get("token")
		}
		if cfg.token == "" || token != cfg.token {
			writeJSONError(w, http.StatusUnauthorized, "invalid or missing move-mcp token")
			return cfg, false
		}
	}
	return cfg, true
}

func (app *App) requireMoveMCPAction(w http.ResponseWriter, r *http.Request) (moveMCPConfig, bool) {
	cfg, ok := app.requireMoveMCP(w, r, false)
	if !ok {
		return cfg, false
	}
	if !cfg.AllowActions {
		writeJSONError(w, http.StatusForbidden, "move-mcp actions disabled")
		return cfg, false
	}
	return cfg, true
}

func bearerToken(r *http.Request) string {
	const prefix = "Bearer "
	auth := r.Header.Get("Authorization")
	if !strings.HasPrefix(auth, prefix) {
		return ""
	}
	return strings.TrimSpace(strings.TrimPrefix(auth, prefix))
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	_ = enc.Encode(v)
}

func writeJSONError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]any{"ok": false, "error": msg})
}

func (app *App) handleMoveMCPStatus(w http.ResponseWriter, r *http.Request) {
	cfg := app.readMoveMCPConfig()
	cfg.token = ""
	cur, _ := findCurrentMoveSet()
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":           true,
		"bridge":       cfg,
		"current_set":  cur,
		"capabilities": moveMCPCapabilities(),
	})
}

func (app *App) handleMoveMCPCapabilities(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":           true,
		"capabilities": moveMCPCapabilities(),
	})
}

func moveMCPCapabilities() map[string]any {
	return map[string]any{
		"read_current_set":        true,
		"list_sets":               true,
		"select_set":              true,
		"read_tracks":             true,
		"read_devices":            true,
		"read_clip_notes":         true,
		"read_clip_envelopes":     true,
		"read_note_automations":   true,
		"read_song_settings":      true,
		"write_song_settings":     true,
		"read_sample_index":       true,
		"write_sample_index":      true,
		"write_clip_notes":        true,
		"write_clip_envelopes":    true,
		"read_track_devices_raw":  true,
		"write_track_devices":     true,
		"write_device_parameters": true,
		"duplicate_set":           true,
		"create_blank_set":        false,
	}
}

func (app *App) handleMoveMCPSets(w http.ResponseWriter, r *http.Request) {
	if _, ok := app.requireMoveMCP(w, r, false); !ok {
		return
	}
	sets, current, err := listMoveSets()
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	out := make([]moveMCPSetListItem, 0, len(sets))
	for _, set := range sets {
		out = append(out, moveMCPSetListItem{
			UUID:      set.UUID,
			Name:      set.Name,
			SongIndex: set.SongIndex,
			Path:      set.Path,
			SongPath:  set.SongPath,
			Modified:  set.ModTime.Format(time.RFC3339),
			Size:      set.Size,
			Current:   set.SongIndex == current,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":                 true,
		"current_song_index": current,
		"sets":               out,
	})
}

func (app *App) handleMoveMCPSelectSet(w http.ResponseWriter, r *http.Request) {
	if _, ok := app.requireMoveMCPAction(w, r); !ok {
		return
	}
	var req moveMCPSwitchSetRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	target, err := findMoveSet(req.UUID, req.Name, req.SongIndex)
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.SaveDirty == nil || *req.SaveDirty {
		if err := saveMoveSongIfDirty(); err != nil {
			writeJSONError(w, http.StatusInternalServerError, "save current song failed: "+err.Error())
			return
		}
	}
	backupPath, err := updateMoveCurrentSongIndex(target.SongIndex)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "settings update failed: "+err.Error())
		return
	}
	restarted := req.Restart == nil || *req.Restart
	if restarted {
		if err := restartMoveUI(); err != nil {
			writeJSONError(w, http.StatusInternalServerError, "restart failed: "+err.Error())
			return
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":          true,
		"set":         moveSetSummary(target),
		"backup_path": backupPath,
		"restarted":   restarted,
	})
}

func (app *App) handleMoveMCPDuplicateSet(w http.ResponseWriter, r *http.Request) {
	cfg, ok := app.requireMoveMCP(w, r, true)
	if !ok {
		return
	}
	var req moveMCPDuplicateSetRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if req.SelectNew != nil && *req.SelectNew && !cfg.AllowActions {
		writeJSONError(w, http.StatusForbidden, "move-mcp actions disabled")
		return
	}
	if req.Restart != nil && *req.Restart && !cfg.AllowActions {
		writeJSONError(w, http.StatusForbidden, "move-mcp actions disabled")
		return
	}
	created, err := duplicateMoveSet(req)
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	selected := req.SelectNew != nil && *req.SelectNew
	var settingsBackup string
	if selected {
		if err := saveMoveSongIfDirty(); err != nil {
			writeJSONError(w, http.StatusInternalServerError, "save current song failed: "+err.Error())
			return
		}
		settingsBackup, err = updateMoveCurrentSongIndex(created.SongIndex)
		if err != nil {
			writeJSONError(w, http.StatusInternalServerError, "settings update failed: "+err.Error())
			return
		}
	}
	restarted := req.Restart != nil && *req.Restart
	if restarted {
		if err := restartMoveUI(); err != nil {
			writeJSONError(w, http.StatusInternalServerError, "restart failed: "+err.Error())
			return
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":                   true,
		"set":                  moveSetSummary(created),
		"selected":             selected,
		"restarted":            restarted,
		"settings_backup_path": settingsBackup,
	})
}

func (app *App) handleMoveMCPCurrentSet(w http.ResponseWriter, r *http.Request) {
	if _, ok := app.requireMoveMCP(w, r, false); !ok {
		return
	}
	cur, song, err := loadCurrentMoveSong()
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, buildMoveMCPSetResponse(cur, song))
}

func (app *App) handleMoveMCPGetSongSettings(w http.ResponseWriter, r *http.Request) {
	if _, ok := app.requireMoveMCP(w, r, false); !ok {
		return
	}
	cur, song, err := loadCurrentMoveSong()
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":       true,
		"set":      moveSetSummary(cur),
		"settings": moveMCPSongSettingsFromSong(song),
	})
}

func (app *App) handleMoveMCPWriteSongSettings(w http.ResponseWriter, r *http.Request) {
	if _, ok := app.requireMoveMCP(w, r, true); !ok {
		return
	}
	var req moveMCPSongSettingsRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if err := validateSongSettingsRequest(req); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	cur, err := findCurrentMoveSet()
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	backupPath, err := backupSongFile(cur)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "backup failed: "+err.Error())
		return
	}
	settings, changed, err := writeMoveSongSettingsToSongFile(cur.SongPath, req)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "write failed: "+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":          true,
		"backup_path": backupPath,
		"set":         moveSetSummary(cur),
		"changed":     changed,
		"settings":    settings,
		"message":     "Song.abl settings updated. Reopen or reload the set on Move if changes are not visible immediately.",
	})
}

func (app *App) handleMoveMCPWriteClip(w http.ResponseWriter, r *http.Request) {
	if _, ok := app.requireMoveMCP(w, r, true); !ok {
		return
	}
	var req moveMCPWriteClipRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if err := validateWriteClipRequest(req); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}

	cur, song, err := loadCurrentMoveSong()
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	trackIdx := req.Track - 1
	sceneIdx := req.Scene - 1
	if trackIdx >= len(song.Tracks) {
		writeJSONError(w, http.StatusBadRequest, "track does not exist in current set")
		return
	}
	replace := req.Replace == nil || *req.Replace
	if sceneIdx < len(song.Tracks[trackIdx].ClipSlots) &&
		song.Tracks[trackIdx].ClipSlots[sceneIdx].Clip != nil && !replace {
		writeJSONError(w, http.StatusConflict, "clip already exists; set replace=true to overwrite")
		return
	}

	backupPath, err := backupSongFile(cur)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "backup failed: "+err.Error())
		return
	}
	if err := writeMoveClipToSongFile(cur.SongPath, req, song, song.Tracks[trackIdx]); err != nil {
		writeJSONError(w, http.StatusInternalServerError, "write failed: "+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":          true,
		"backup_path": backupPath,
		"set":         map[string]any{"uuid": cur.UUID, "name": cur.Name, "song_index": cur.SongIndex},
		"track":       req.Track,
		"scene":       req.Scene,
		"note_count":  len(req.Notes),
		"message":     "Song.abl updated. Reopen or reload the set on Move if changes are not visible immediately.",
	})
}

func (app *App) handleMoveMCPGetTrackDevices(w http.ResponseWriter, r *http.Request) {
	if _, ok := app.requireMoveMCP(w, r, false); !ok {
		return
	}
	track, ok := moveTrackFromRequest(w, r)
	if !ok {
		return
	}
	cur, root, trackObj, err := loadMoveTrackObject(track)
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	devices, _ := trackObj["devices"].([]any)
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":           true,
		"set":          moveSetSummary(cur),
		"track":        track,
		"track_name":   trackObj["name"],
		"track_color":  trackObj["color"],
		"devices":      devices,
		"device_count": len(devices),
		"schema":       root["$schema"],
	})
}

func (app *App) handleMoveMCPWriteTrackDevices(w http.ResponseWriter, r *http.Request) {
	if _, ok := app.requireMoveMCP(w, r, true); !ok {
		return
	}
	track, ok := moveTrackFromRequest(w, r)
	if !ok {
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 4<<20)
	var req moveMCPWriteTrackDevicesRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	req.Track = track
	cur, err := findCurrentMoveSet()
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	devices, sourceTrack, err := resolveTrackDevicesForWrite(cur.SongPath, req)
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err := validateRawDeviceTree(devices); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	backupPath, err := backupSongFile(cur)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "backup failed: "+err.Error())
		return
	}
	if err := writeMoveTrackDevicesToSongFile(cur.SongPath, req, devices); err != nil {
		writeJSONError(w, http.StatusInternalServerError, "write failed: "+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":           true,
		"backup_path":  backupPath,
		"set":          map[string]any{"uuid": cur.UUID, "name": cur.Name, "song_index": cur.SongIndex},
		"track":        req.Track,
		"source_track": sourceTrack,
		"device_count": len(devices),
		"message":      "Song.abl devices updated. Reopen or reload the set on Move if changes are not visible immediately.",
	})
}

func (app *App) handleMoveMCPWriteDeviceParameters(w http.ResponseWriter, r *http.Request) {
	if _, ok := app.requireMoveMCP(w, r, true); !ok {
		return
	}
	track, ok := moveTrackFromRequest(w, r)
	if !ok {
		return
	}
	var req moveMCPWriteDeviceParametersRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if err := validateDeviceParameterWriteRequest(req); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	cur, err := findCurrentMoveSet()
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	backupPath, err := backupSongFile(cur)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "backup failed: "+err.Error())
		return
	}
	device, err := writeMoveDeviceParametersToSongFile(cur.SongPath, track, req)
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":          true,
		"backup_path": backupPath,
		"set":         map[string]any{"uuid": cur.UUID, "name": cur.Name, "song_index": cur.SongIndex},
		"track":       track,
		"device_path": req.DevicePath,
		"device":      deviceSummaryFromRaw(device),
		"message":     "Song.abl device parameters updated. Reopen or reload the set on Move if changes are not visible immediately.",
	})
}

func (app *App) handleMoveMCPSamples(w http.ResponseWriter, r *http.Request) {
	if _, ok := app.requireMoveMCP(w, r, false); !ok {
		return
	}
	index, err := readMoveMCPSampleIndex()
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			writeJSON(w, http.StatusOK, map[string]any{
				"ok":      true,
				"samples": []moveMCPSampleItem{},
				"count":   0,
				"message": "sample index not found; run scan_samples.py and upload it",
			})
			return
		}
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	samples := filterMoveMCPSamples(index.Samples, r)
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":           true,
		"schema":       index.Schema,
		"generated_at": index.GeneratedAt,
		"source_root":  index.SourceRoot,
		"move_root":    index.MoveRoot,
		"count":        len(samples),
		"total_count":  len(index.Samples),
		"samples":      samples,
		"summary":      index.Summary,
		"warnings":     index.Warnings,
	})
}

func (app *App) handleMoveMCPWriteSampleIndex(w http.ResponseWriter, r *http.Request) {
	cfg, ok := app.requireMoveMCP(w, r, true)
	if !ok {
		return
	}
	maxBytes := int64(cfg.MaxUploadMB) << 20
	if maxBytes <= 0 {
		maxBytes = 64 << 20
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxBytes)
	var index moveMCPSampleIndex
	if err := json.NewDecoder(r.Body).Decode(&index); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if err := validateMoveMCPSampleIndex(index); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	if index.Count == 0 {
		index.Count = len(index.Samples)
	}
	if err := writeMoveMCPSampleIndex(index); err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":           true,
		"path":         moveMCPSampleIndexPath,
		"count":        len(index.Samples),
		"generated_at": index.GeneratedAt,
	})
}

func findCurrentMoveSet() (moveCurrentSet, error) {
	var settings struct {
		CurrentSongIndex int `json:"currentSongIndex"`
	}
	raw, err := os.ReadFile(moveSettings)
	if err != nil {
		return moveCurrentSet{}, err
	}
	if err := json.Unmarshal(raw, &settings); err != nil {
		return moveCurrentSet{}, err
	}
	return findMoveSetByIndex(settings.CurrentSongIndex)
}

func readMoveCurrentSongIndex() (int, error) {
	var settings struct {
		CurrentSongIndex int `json:"currentSongIndex"`
	}
	raw, err := os.ReadFile(moveSettings)
	if err != nil {
		return 0, err
	}
	if err := json.Unmarshal(raw, &settings); err != nil {
		return 0, err
	}
	return settings.CurrentSongIndex, nil
}

func findMoveSetByIndex(index int) (moveCurrentSet, error) {
	entries, err := os.ReadDir(moveSetsDir)
	if err != nil {
		return moveCurrentSet{}, err
	}
	for _, e := range entries {
		if !e.IsDir() || strings.HasPrefix(e.Name(), ".") {
			continue
		}
		uuidPath := filepath.Join(moveSetsDir, e.Name())
		xattr, err := readUserXattrString(uuidPath, "user.song-index")
		if err != nil || strings.TrimSpace(xattr) != strconv.Itoa(index) {
			continue
		}
		sub, err := firstSetName(uuidPath)
		if err != nil {
			return moveCurrentSet{}, err
		}
		songPath := filepath.Join(uuidPath, sub, "Song.abl")
		info, err := os.Stat(songPath)
		if err != nil {
			return moveCurrentSet{}, err
		}
		return moveCurrentSet{
			UUID:      e.Name(),
			Name:      sub,
			SongIndex: index,
			Path:      filepath.Join(uuidPath, sub),
			SongPath:  songPath,
			ModTime:   info.ModTime(),
			Size:      info.Size(),
		}, nil
	}
	return moveCurrentSet{}, fmt.Errorf("current set index %d not found", index)
}

func listMoveSets() ([]moveCurrentSet, int, error) {
	current, err := readMoveCurrentSongIndex()
	if err != nil {
		return nil, 0, err
	}
	entries, err := os.ReadDir(moveSetsDir)
	if err != nil {
		return nil, current, err
	}
	sets := make([]moveCurrentSet, 0, len(entries))
	for _, e := range entries {
		if !e.IsDir() || strings.HasPrefix(e.Name(), ".") {
			continue
		}
		uuidPath := filepath.Join(moveSetsDir, e.Name())
		indexRaw, err := readUserXattrString(uuidPath, "user.song-index")
		if err != nil {
			continue
		}
		index, err := strconv.Atoi(strings.TrimSpace(indexRaw))
		if err != nil {
			continue
		}
		sub, err := firstSetName(uuidPath)
		if err != nil {
			continue
		}
		songPath := filepath.Join(uuidPath, sub, "Song.abl")
		info, err := os.Stat(songPath)
		if err != nil {
			continue
		}
		sets = append(sets, moveCurrentSet{
			UUID:      e.Name(),
			Name:      sub,
			SongIndex: index,
			Path:      filepath.Join(uuidPath, sub),
			SongPath:  songPath,
			ModTime:   info.ModTime(),
			Size:      info.Size(),
		})
	}
	sort.Slice(sets, func(i, j int) bool {
		if sets[i].SongIndex == sets[j].SongIndex {
			return strings.ToLower(sets[i].Name) < strings.ToLower(sets[j].Name)
		}
		return sets[i].SongIndex < sets[j].SongIndex
	})
	return sets, current, nil
}

func findMoveSet(uuid, name string, songIndex *int) (moveCurrentSet, error) {
	if songIndex != nil {
		return findMoveSetByIndex(*songIndex)
	}
	sets, _, err := listMoveSets()
	if err != nil {
		return moveCurrentSet{}, err
	}
	for _, set := range sets {
		if uuid != "" && set.UUID == uuid {
			return set, nil
		}
		if name != "" && strings.EqualFold(set.Name, name) {
			return set, nil
		}
	}
	return moveCurrentSet{}, errors.New("set not found; provide uuid, name, or song_index")
}

func moveSetSummary(set moveCurrentSet) moveMCPSetListItem {
	return moveMCPSetListItem{
		UUID:      set.UUID,
		Name:      set.Name,
		SongIndex: set.SongIndex,
		Path:      set.Path,
		SongPath:  set.SongPath,
		Modified:  set.ModTime.Format(time.RFC3339),
		Size:      set.Size,
	}
}

func firstSetName(uuidPath string) (string, error) {
	entries, err := os.ReadDir(uuidPath)
	if err != nil {
		return "", err
	}
	for _, e := range entries {
		if e.IsDir() && !strings.HasPrefix(e.Name(), ".") {
			return e.Name(), nil
		}
	}
	return "", errors.New("set name directory not found")
}

func loadCurrentMoveSong() (moveCurrentSet, moveSong, error) {
	cur, err := findCurrentMoveSet()
	if err != nil {
		return moveCurrentSet{}, moveSong{}, err
	}
	raw, err := os.ReadFile(cur.SongPath)
	if err != nil {
		return moveCurrentSet{}, moveSong{}, err
	}
	var song moveSong
	if err := json.Unmarshal(raw, &song); err != nil {
		return moveCurrentSet{}, moveSong{}, err
	}
	return cur, song, nil
}

func readUserXattrString(path, name string) (string, error) {
	out, err := exec.Command("getfattr", "--only-values", "-n", name, path).Output()
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(strings.TrimRight(string(out), "\x00")), nil
}

func setUserXattrString(path, name, value string) error {
	return exec.Command("setfattr", "-n", name, "-v", value, path).Run()
}

func updateMoveCurrentSongIndex(index int) (string, error) {
	raw, err := os.ReadFile(moveSettings)
	if err != nil {
		return "", err
	}
	backupPath, err := backupMoveMCPFile("settings", "Settings.json", raw)
	if err != nil {
		return "", err
	}
	var root map[string]any
	if err := json.Unmarshal(raw, &root); err != nil {
		return "", err
	}
	root["currentSongIndex"] = index
	data, err := json.MarshalIndent(root, "", "  ")
	if err != nil {
		return "", err
	}
	data = append(data, '\n')
	if err := writeFileAtomic(moveSettings, data, 0o644); err != nil {
		return "", err
	}
	chownToAbleton(moveSettings)
	return backupPath, nil
}

func saveMoveSongIfDirty() error {
	return exec.Command(
		"dbus-send",
		"--system",
		"--print-reply",
		"--dest=com.ableton.move",
		"/com/ableton/move/browser",
		"com.ableton.move.Browser.saveSongIfDirty",
		"string:",
	).Run()
}

func restartMoveUI() error {
	return exec.Command("/data/UserData/schwung/restart-move.sh").Start()
}

func duplicateMoveSet(req moveMCPDuplicateSetRequest) (moveCurrentSet, error) {
	name, err := sanitizeMoveSetName(req.Name)
	if err != nil {
		return moveCurrentSet{}, err
	}
	source, err := findMoveSet(req.SourceUUID, req.SourceName, req.SourceSongIndex)
	if err != nil {
		if req.SourceUUID != "" || req.SourceName != "" || req.SourceSongIndex != nil {
			return moveCurrentSet{}, err
		}
		source, err = findCurrentMoveSet()
		if err != nil {
			return moveCurrentSet{}, err
		}
	}
	sets, _, err := listMoveSets()
	if err != nil {
		return moveCurrentSet{}, err
	}
	nextIndex := 0
	for _, set := range sets {
		if strings.EqualFold(set.Name, name) {
			return moveCurrentSet{}, fmt.Errorf("set name already exists: %s", name)
		}
		if set.SongIndex >= nextIndex {
			nextIndex = set.SongIndex + 1
		}
	}
	uuid, err := newUUIDv4()
	if err != nil {
		return moveCurrentSet{}, err
	}
	uuidPath := filepath.Join(moveSetsDir, uuid)
	dst := filepath.Join(uuidPath, name)
	if err := os.MkdirAll(uuidPath, 0o755); err != nil {
		return moveCurrentSet{}, err
	}
	if err := copyDir(source.Path, dst); err != nil {
		return moveCurrentSet{}, err
	}
	chownTreeToAbleton(uuidPath)
	color := "21"
	if req.SongColor != nil {
		color = strconv.Itoa(*req.SongColor)
	} else if sourceColor, err := readUserXattrString(filepath.Dir(source.Path), "user.song-color"); err == nil && sourceColor != "" {
		color = sourceColor
	}
	attrs := map[string]string{
		"user.song-index":              strconv.Itoa(nextIndex),
		"user.song-color":              color,
		"user.last-modified-time":      time.Now().UTC().Format(time.RFC3339),
		"user.local-cloud-state":       "notSynced",
		"user.was-externally-modified": "true",
	}
	for key, value := range attrs {
		if err := setUserXattrString(uuidPath, key, value); err != nil {
			return moveCurrentSet{}, fmt.Errorf("set %s failed: %w", key, err)
		}
	}
	info, err := os.Stat(filepath.Join(dst, "Song.abl"))
	if err != nil {
		return moveCurrentSet{}, err
	}
	return moveCurrentSet{
		UUID:      uuid,
		Name:      name,
		SongIndex: nextIndex,
		Path:      dst,
		SongPath:  filepath.Join(dst, "Song.abl"),
		ModTime:   info.ModTime(),
		Size:      info.Size(),
	}, nil
}

func sanitizeMoveSetName(name string) (string, error) {
	name = strings.TrimSpace(name)
	if name == "" {
		return "", errors.New("name is required")
	}
	if len(name) > 80 {
		return "", errors.New("name is too long")
	}
	if strings.ContainsAny(name, `/\:`) || strings.ContainsRune(name, 0) {
		return "", errors.New("name contains invalid path characters")
	}
	if name == "." || name == ".." {
		return "", errors.New("name is invalid")
	}
	return name, nil
}

func newUUIDv4() (string, error) {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return fmt.Sprintf("%08x-%04x-%04x-%04x-%012x",
		b[0:4], b[4:6], b[6:8], b[8:10], b[10:16]), nil
}

func copyDir(src, dst string) error {
	info, err := os.Stat(src)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return fmt.Errorf("%s is not a directory", src)
	}
	if err := os.MkdirAll(dst, info.Mode().Perm()); err != nil {
		return err
	}
	entries, err := os.ReadDir(src)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		srcPath := filepath.Join(src, entry.Name())
		dstPath := filepath.Join(dst, entry.Name())
		entryInfo, err := entry.Info()
		if err != nil {
			return err
		}
		if entryInfo.IsDir() {
			if err := copyDir(srcPath, dstPath); err != nil {
				return err
			}
			continue
		}
		if entryInfo.Mode()&os.ModeSymlink != 0 {
			continue
		}
		if err := copyFile(srcPath, dstPath, entryInfo.Mode().Perm()); err != nil {
			return err
		}
	}
	return nil
}

func copyFile(src, dst string, mode os.FileMode) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, mode)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

func chownTreeToAbleton(path string) {
	_ = filepath.WalkDir(path, func(p string, _ os.DirEntry, _ error) error {
		chownToAbleton(p)
		return nil
	})
}

func buildMoveMCPSetResponse(cur moveCurrentSet, song moveSong) moveMCPSetResponse {
	resp := moveMCPSetResponse{
		OK: true,
		Set: moveMCPSetMeta{
			UUID:                 cur.UUID,
			Name:                 cur.Name,
			SongIndex:            cur.SongIndex,
			Path:                 cur.Path,
			SongPath:             cur.SongPath,
			Schema:               song.Schema,
			StepEditorResolution: song.StepEditorResolution,
			Tempo:                song.Tempo,
			GlobalGrooveAmount:   song.GlobalGrooveAmount,
			TimeSignature:        song.TimeSignature,
			RootNote:             song.RootNote,
			Scale:                song.Scale,
			MelodicLayout:        song.MelodicLayout,
			Modified:             cur.ModTime.Format(time.RFC3339),
			Size:                 cur.Size,
		},
		Scenes: song.Scenes,
	}
	for i, tr := range song.Tracks {
		tv := moveMCPTrackView{
			Index:         i + 1,
			Kind:          tr.Kind,
			Name:          tr.Name,
			Color:         tr.Color,
			IsSelected:    tr.IsSelected,
			MIDIInputMode: tr.MIDIInputMode,
			UIOctaveIndex: tr.UIOctaveIndex,
			Mixer:         tr.Mixer,
			Devices:       summarizeDevices(tr.Devices),
			ClipSlotCount: len(tr.ClipSlots),
		}
		for j, slot := range tr.ClipSlots {
			if slot.Clip == nil {
				continue
			}
			c := slot.Clip
			tv.Clips = append(tv.Clips, moveMCPClipView{
				Scene:                    j + 1,
				HasStop:                  slot.HasStop,
				IsPlaying:                c.IsPlaying,
				Name:                     c.Name,
				Color:                    c.Color,
				IsEnabled:                c.IsEnabled,
				TimeSignature:            c.TimeSignature,
				Start:                    c.Region.Start,
				End:                      c.Region.End,
				LoopStart:                c.Region.Loop.Start,
				LoopEnd:                  c.Region.Loop.End,
				LoopEnabled:              c.Region.Loop.IsEnabled,
				GrooveID:                 c.GrooveID,
				StepEditorScrollPosition: c.StepEditorScrollPosition,
				Notes:                    c.Notes,
				Envelopes:                c.Envelopes,
			})
		}
		resp.Tracks = append(resp.Tracks, tv)
	}
	return resp
}

func moveMCPSongSettingsFromSong(song moveSong) map[string]any {
	return map[string]any{
		"tempo":                  song.Tempo,
		"global_groove_amount":   song.GlobalGrooveAmount,
		"root_note":              song.RootNote,
		"scale":                  song.Scale,
		"melodic_layout":         song.MelodicLayout,
		"step_editor_resolution": song.StepEditorResolution,
		"time_signature":         song.TimeSignature,
	}
}

func summarizeDevices(devs []moveDevice) []moveMCPDeviceSummary {
	out := make([]moveMCPDeviceSummary, 0, len(devs))
	for _, d := range devs {
		out = append(out, moveMCPDeviceSummary{
			Kind:             d.Kind,
			Name:             d.Name,
			PresetURI:        d.PresetURI,
			LockID:           d.LockID,
			LockSeal:         d.LockSeal,
			Parameters:       d.Parameters,
			ParameterCount:   len(d.Parameters),
			DeviceDataKeys:   sortedMapKeys(d.DeviceData),
			Chains:           summarizeDeviceChains(d.Chains),
			ReturnChains:     summarizeDeviceChains(d.ReturnChains),
			DrumZoneSettings: d.DrumZoneSettings,
		})
	}
	return out
}

func deviceSummaryFromRaw(dev map[string]any) map[string]any {
	params, _ := dev["parameters"].(map[string]any)
	return map[string]any{
		"kind":            dev["kind"],
		"name":            dev["name"],
		"preset_uri":      dev["presetUri"],
		"parameter_count": len(params),
		"parameters":      params,
	}
}

func summarizeDeviceChains(chains []moveDeviceChain) []moveMCPDeviceChainSummary {
	out := make([]moveMCPDeviceChainSummary, 0, len(chains))
	for _, ch := range chains {
		out = append(out, moveMCPDeviceChainSummary{
			Name:             ch.Name,
			Devices:          summarizeDevices(ch.Devices),
			Chains:           summarizeDeviceChains(ch.Chains),
			ReturnChains:     summarizeDeviceChains(ch.ReturnChains),
			DrumZoneSettings: ch.DrumZoneSettings,
		})
	}
	return out
}

func sortedMapKeys(m map[string]any) []string {
	if len(m) == 0 {
		return nil
	}
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

func validateWriteClipRequest(req moveMCPWriteClipRequest) error {
	if req.Track < 1 || req.Track > 4 {
		return fmt.Errorf("track must be 1-4")
	}
	if req.Scene < 1 || req.Scene > 8 {
		return fmt.Errorf("scene must be 1-8")
	}
	if len(req.Notes) > 512 {
		return fmt.Errorf("too many notes: %d", len(req.Notes))
	}
	for i, n := range req.Notes {
		if n.NoteNumber < 0 || n.NoteNumber > 127 {
			return fmt.Errorf("notes[%d].noteNumber must be 0-127", i)
		}
		if n.StartTime < 0 || n.Duration <= 0 {
			return fmt.Errorf("notes[%d] has invalid startTime/duration", i)
		}
		if n.Velocity < 0 || n.Velocity > 127 || n.OffVelocity < 0 || n.OffVelocity > 127 {
			return fmt.Errorf("notes[%d] velocity/offVelocity must be 0-127", i)
		}
	}
	if err := validateClipEnvelopes(req.Envelopes); err != nil {
		return err
	}
	return nil
}

func validateClipEnvelopes(envelopes []map[string]any) error {
	if len(envelopes) > 256 {
		return fmt.Errorf("too many envelopes: %d", len(envelopes))
	}
	for i, env := range envelopes {
		prefix := fmt.Sprintf("envelopes[%d]", i)
		if _, ok := numberFromAny(env["parameterId"]); !ok {
			return fmt.Errorf("%s.parameterId is required and must be numeric", prefix)
		}
		if region, ok := env["region"].(map[string]any); ok {
			for _, key := range []string{"start", "end"} {
				if _, ok := numberFromAny(region[key]); !ok {
					return fmt.Errorf("%s.region.%s must be numeric", prefix, key)
				}
			}
		}
		breakpoints, ok := env["breakpoints"].([]any)
		if !ok {
			return fmt.Errorf("%s.breakpoints is required", prefix)
		}
		if len(breakpoints) > 4096 {
			return fmt.Errorf("%s has too many breakpoints: %d", prefix, len(breakpoints))
		}
		for j, raw := range breakpoints {
			point, ok := raw.(map[string]any)
			if !ok {
				return fmt.Errorf("%s.breakpoints[%d] must be an object", prefix, j)
			}
			t, ok := numberFromAny(point["time"])
			if !ok || t < 0 {
				return fmt.Errorf("%s.breakpoints[%d].time must be a non-negative number", prefix, j)
			}
			if _, ok := numberFromAny(point["value"]); !ok {
				return fmt.Errorf("%s.breakpoints[%d].value must be numeric", prefix, j)
			}
		}
	}
	return nil
}

func numberFromAny(v any) (float64, bool) {
	switch n := v.(type) {
	case float64:
		return n, true
	case float32:
		return float64(n), true
	case int:
		return float64(n), true
	case int64:
		return float64(n), true
	case json.Number:
		f, err := n.Float64()
		return f, err == nil
	default:
		return 0, false
	}
}

func validateSongSettingsRequest(req moveMCPSongSettingsRequest) error {
	if req.Tempo != nil && (*req.Tempo < 20 || *req.Tempo > 999) {
		return fmt.Errorf("tempo must be between 20 and 999 BPM")
	}
	if req.GlobalGrooveAmount != nil && (*req.GlobalGrooveAmount < 0 || *req.GlobalGrooveAmount > 1) {
		return fmt.Errorf("global_groove_amount must be between 0 and 1")
	}
	if req.RootNote != nil && (*req.RootNote < 0 || *req.RootNote > 11) {
		return fmt.Errorf("root_note must be 0-11")
	}
	if req.Scale != nil && len(strings.TrimSpace(*req.Scale)) > 64 {
		return fmt.Errorf("scale is too long")
	}
	if req.MelodicLayout != nil && len(strings.TrimSpace(*req.MelodicLayout)) > 64 {
		return fmt.Errorf("melodic_layout is too long")
	}
	if req.StepEditorResolution != nil && len(strings.TrimSpace(*req.StepEditorResolution)) > 32 {
		return fmt.Errorf("step_editor_resolution is too long")
	}
	if req.TimeSignature != nil {
		upper, ok := numberFromAny((*req.TimeSignature)["upper"])
		if !ok || upper < 1 || upper > 32 {
			return fmt.Errorf("time_signature.upper must be 1-32")
		}
		lower, ok := numberFromAny((*req.TimeSignature)["lower"])
		if !ok || lower < 1 || lower > 32 {
			return fmt.Errorf("time_signature.lower must be 1-32")
		}
	}
	return nil
}

func buildMoveClipFromRequest(req moveMCPWriteClipRequest, song moveSong, tr moveTrack) *moveClip {
	start, end := 0.0, 4.0
	if req.Start != nil {
		start = *req.Start
	}
	if req.End != nil {
		end = *req.End
	}
	loopStart, loopEnd := start, end
	if req.LoopStart != nil {
		loopStart = *req.LoopStart
	}
	if req.LoopEnd != nil {
		loopEnd = *req.LoopEnd
	}
	color := tr.Color
	if req.Color != nil {
		color = *req.Color
	}
	isPlaying := false
	if req.IsPlaying != nil {
		isPlaying = *req.IsPlaying
	}
	isEnabled := true
	if req.IsEnabled != nil {
		isEnabled = *req.IsEnabled
	}
	return &moveClip{
		IsPlaying:     isPlaying,
		Name:          req.Name,
		Color:         color,
		IsEnabled:     isEnabled,
		TimeSignature: song.TimeSignature,
		Region: moveClipRegion{
			Start: start,
			End:   end,
			Loop:  moveClipLoop{Start: loopStart, End: loopEnd, IsEnabled: true},
		},
		GrooveID:                 1,
		StepEditorScrollPosition: 0,
		Notes:                    req.Notes,
		Envelopes:                req.Envelopes,
	}
}

func buildMoveClipMapFromRequest(req moveMCPWriteClipRequest, song moveSong, tr moveTrack) map[string]any {
	clip := buildMoveClipFromRequest(req, song, tr)
	notes := make([]any, 0, len(clip.Notes))
	for _, n := range clip.Notes {
		note := map[string]any{
			"noteNumber":  n.NoteNumber,
			"startTime":   n.StartTime,
			"duration":    n.Duration,
			"velocity":    n.Velocity,
			"offVelocity": n.OffVelocity,
		}
		if len(n.Automations) > 0 {
			note["automations"] = n.Automations
		}
		notes = append(notes, note)
	}
	envelopes := make([]any, 0, len(clip.Envelopes))
	for _, env := range clip.Envelopes {
		envelopes = append(envelopes, env)
	}
	return map[string]any{
		"isPlaying":     clip.IsPlaying,
		"name":          clip.Name,
		"color":         clip.Color,
		"isEnabled":     clip.IsEnabled,
		"timeSignature": clip.TimeSignature,
		"region": map[string]any{
			"start": clip.Region.Start,
			"end":   clip.Region.End,
			"loop": map[string]any{
				"start":     clip.Region.Loop.Start,
				"end":       clip.Region.Loop.End,
				"isEnabled": clip.Region.Loop.IsEnabled,
			},
		},
		"grooveId":                 clip.GrooveID,
		"stepEditorScrollPosition": clip.StepEditorScrollPosition,
		"notes":                    notes,
		"envelopes":                envelopes,
	}
}

func backupSongFile(cur moveCurrentSet) (string, error) {
	ts := time.Now().UTC().Format("20060102T150405Z")
	dir := filepath.Join("/data/UserData/schwung/move-mcp/backups", cur.UUID, ts)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	dst := filepath.Join(dir, "Song.abl")
	raw, err := os.ReadFile(cur.SongPath)
	if err != nil {
		return "", err
	}
	if err := os.WriteFile(dst, raw, 0o644); err != nil {
		return "", err
	}
	return dst, nil
}

func backupMoveMCPFile(scope, name string, raw []byte) (string, error) {
	ts := time.Now().UTC().Format("20060102T150405Z")
	dir := filepath.Join("/data/UserData/schwung/move-mcp/backups", scope, ts)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	dst := filepath.Join(dir, name)
	if err := os.WriteFile(dst, raw, 0o644); err != nil {
		return "", err
	}
	chownToAbleton(dst)
	return dst, nil
}

func writeMoveClipToSongFile(path string, req moveMCPWriteClipRequest, song moveSong, tr moveTrack) error {
	raw, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	var root map[string]any
	if err := json.Unmarshal(raw, &root); err != nil {
		return err
	}
	tracks, ok := root["tracks"].([]any)
	if !ok || req.Track < 1 || req.Track > len(tracks) {
		return fmt.Errorf("tracks array missing or track out of range")
	}
	track, ok := tracks[req.Track-1].(map[string]any)
	if !ok {
		return fmt.Errorf("track %d is not an object", req.Track)
	}
	slots, _ := track["clipSlots"].([]any)
	for len(slots) < req.Scene {
		slots = append(slots, map[string]any{"hasStop": true})
	}
	slot, ok := slots[req.Scene-1].(map[string]any)
	if !ok {
		slot = map[string]any{}
		slots[req.Scene-1] = slot
	}
	slot["hasStop"] = true
	slot["clip"] = buildMoveClipMapFromRequest(req, song, tr)
	track["clipSlots"] = slots
	root["tracks"] = tracks
	return writeMoveSongAtomic(path, root)
}

func writeMoveSongSettingsToSongFile(path string, req moveMCPSongSettingsRequest) (map[string]any, []string, error) {
	root, err := readMoveSongRoot(path)
	if err != nil {
		return nil, nil, err
	}
	changed := []string{}
	if req.Tempo != nil {
		root["tempo"] = *req.Tempo
		changed = append(changed, "tempo")
	}
	if req.GlobalGrooveAmount != nil {
		root["globalGrooveAmount"] = *req.GlobalGrooveAmount
		changed = append(changed, "global_groove_amount")
	}
	if req.RootNote != nil {
		root["rootNote"] = *req.RootNote
		changed = append(changed, "root_note")
	}
	if req.Scale != nil {
		root["scale"] = strings.TrimSpace(*req.Scale)
		changed = append(changed, "scale")
	}
	if req.MelodicLayout != nil {
		root["melodicLayout"] = strings.TrimSpace(*req.MelodicLayout)
		changed = append(changed, "melodic_layout")
	}
	if req.StepEditorResolution != nil {
		root["stepEditorResolution"] = strings.TrimSpace(*req.StepEditorResolution)
		changed = append(changed, "step_editor_resolution")
	}
	if req.TimeSignature != nil {
		root["timeSignature"] = *req.TimeSignature
		changed = append(changed, "time_signature")
	}
	if len(changed) > 0 {
		if err := writeMoveSongAtomic(path, root); err != nil {
			return nil, nil, err
		}
	}
	var song moveSong
	raw, err := json.Marshal(root)
	if err != nil {
		return nil, nil, err
	}
	if err := json.Unmarshal(raw, &song); err != nil {
		return nil, nil, err
	}
	return moveMCPSongSettingsFromSong(song), changed, nil
}

func moveTrackFromRequest(w http.ResponseWriter, r *http.Request) (int, bool) {
	track, err := strconv.Atoi(r.PathValue("track"))
	if err != nil || track < 1 || track > 4 {
		writeJSONError(w, http.StatusBadRequest, "track must be 1-4")
		return 0, false
	}
	return track, true
}

func loadMoveTrackObject(track int) (moveCurrentSet, map[string]any, map[string]any, error) {
	cur, err := findCurrentMoveSet()
	if err != nil {
		return moveCurrentSet{}, nil, nil, err
	}
	root, err := readMoveSongRoot(cur.SongPath)
	if err != nil {
		return moveCurrentSet{}, nil, nil, err
	}
	trackObj, err := rawTrackObject(root, track)
	if err != nil {
		return moveCurrentSet{}, nil, nil, err
	}
	return cur, root, trackObj, nil
}

func readMoveSongRoot(path string) (map[string]any, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var root map[string]any
	if err := json.Unmarshal(raw, &root); err != nil {
		return nil, err
	}
	return root, nil
}

func rawTrackObject(root map[string]any, track int) (map[string]any, error) {
	tracks, ok := root["tracks"].([]any)
	if !ok {
		return nil, fmt.Errorf("tracks array missing")
	}
	if track < 1 || track > len(tracks) {
		return nil, fmt.Errorf("track %d does not exist", track)
	}
	trackObj, ok := tracks[track-1].(map[string]any)
	if !ok {
		return nil, fmt.Errorf("track %d is not an object", track)
	}
	return trackObj, nil
}

func resolveTrackDevicesForWrite(path string, req moveMCPWriteTrackDevicesRequest) ([]any, int, error) {
	if req.Track < 1 || req.Track > 4 {
		return nil, 0, fmt.Errorf("track must be 1-4")
	}
	root, err := readMoveSongRoot(path)
	if err != nil {
		return nil, 0, err
	}
	if _, err := rawTrackObject(root, req.Track); err != nil {
		return nil, 0, err
	}
	if req.SourceTrack != nil {
		source, err := rawTrackObject(root, *req.SourceTrack)
		if err != nil {
			return nil, 0, fmt.Errorf("source_track invalid: %w", err)
		}
		devices, ok := source["devices"].([]any)
		if !ok {
			return nil, 0, fmt.Errorf("source_track %d has no devices array", *req.SourceTrack)
		}
		return cloneJSONSlice(devices), *req.SourceTrack, nil
	}
	if req.Devices == nil {
		return nil, 0, fmt.Errorf("devices or source_track is required")
	}
	devices := make([]any, 0, len(req.Devices))
	for _, d := range req.Devices {
		devices = append(devices, d)
	}
	return devices, 0, nil
}

func cloneJSONSlice(in []any) []any {
	raw, err := json.Marshal(in)
	if err != nil {
		return in
	}
	var out []any
	if err := json.Unmarshal(raw, &out); err != nil {
		return in
	}
	return out
}

func validateRawDeviceTree(devices []any) error {
	if len(devices) == 0 {
		return fmt.Errorf("devices must contain at least one device")
	}
	if len(devices) > 32 {
		return fmt.Errorf("too many top-level devices: %d", len(devices))
	}
	return validateRawDevices(devices, "devices", 0)
}

func validateRawDevices(devices []any, path string, depth int) error {
	if depth > 8 {
		return fmt.Errorf("%s is nested too deeply", path)
	}
	for i, raw := range devices {
		devPath := fmt.Sprintf("%s[%d]", path, i)
		dev, ok := raw.(map[string]any)
		if !ok {
			return fmt.Errorf("%s must be an object", devPath)
		}
		if err := validateRawDevice(dev, devPath, depth); err != nil {
			return err
		}
	}
	return nil
}

func validateRawDevice(dev map[string]any, path string, depth int) error {
	kind, hasKind := dev["kind"].(string)
	name, hasName := dev["name"].(string)
	presetURI, hasPreset := dev["presetUri"].(string)
	if !hasKind || strings.TrimSpace(kind) == "" {
		return fmt.Errorf("%s.kind is required", path)
	}
	if hasName && len(name) > 160 {
		return fmt.Errorf("%s.name is too long", path)
	}
	if hasPreset {
		if !strings.HasPrefix(presetURI, "ableton:/") && !strings.HasPrefix(presetURI, "file:") {
			return fmt.Errorf("%s.presetUri must be an ableton:/ or file: URI", path)
		}
		if _, ok := dev["lockId"]; !ok {
			return fmt.Errorf("%s.lockId is required when presetUri is present", path)
		}
		if _, ok := dev["lockSeal"]; !ok {
			return fmt.Errorf("%s.lockSeal is required when presetUri is present", path)
		}
	}
	if params, ok := dev["parameters"].(map[string]any); ok && len(params) > 512 {
		return fmt.Errorf("%s.parameters has too many entries", path)
	}
	for _, key := range []string{"chains", "returnChains"} {
		chains, ok := dev[key].([]any)
		if !ok {
			continue
		}
		if len(chains) > 128 {
			return fmt.Errorf("%s.%s has too many chains", path, key)
		}
		if err := validateRawChains(chains, path+"."+key, depth+1); err != nil {
			return err
		}
	}
	return nil
}

func validateRawChains(chains []any, path string, depth int) error {
	if depth > 8 {
		return fmt.Errorf("%s is nested too deeply", path)
	}
	for i, raw := range chains {
		chainPath := fmt.Sprintf("%s[%d]", path, i)
		chain, ok := raw.(map[string]any)
		if !ok {
			return fmt.Errorf("%s must be an object", chainPath)
		}
		if name, ok := chain["name"].(string); ok && len(name) > 160 {
			return fmt.Errorf("%s.name is too long", chainPath)
		}
		if devs, ok := chain["devices"].([]any); ok {
			if len(devs) > 128 {
				return fmt.Errorf("%s.devices has too many devices", chainPath)
			}
			if err := validateRawDevices(devs, chainPath+".devices", depth+1); err != nil {
				return err
			}
		}
		for _, key := range []string{"chains", "returnChains"} {
			nested, ok := chain[key].([]any)
			if !ok {
				continue
			}
			if len(nested) > 128 {
				return fmt.Errorf("%s.%s has too many chains", chainPath, key)
			}
			if err := validateRawChains(nested, chainPath+"."+key, depth+1); err != nil {
				return err
			}
		}
	}
	return nil
}

func writeMoveTrackDevicesToSongFile(path string, req moveMCPWriteTrackDevicesRequest, devices []any) error {
	root, err := readMoveSongRoot(path)
	if err != nil {
		return err
	}
	track, err := rawTrackObject(root, req.Track)
	if err != nil {
		return err
	}
	track["devices"] = devices
	if req.TrackName != nil {
		name := strings.TrimSpace(*req.TrackName)
		if len(name) > 160 {
			return fmt.Errorf("track_name is too long")
		}
		track["name"] = name
	}
	if req.TrackColor != nil {
		if *req.TrackColor < 0 || *req.TrackColor > 127 {
			return fmt.Errorf("track_color must be 0-127")
		}
		track["color"] = *req.TrackColor
	}
	return writeMoveSongAtomic(path, root)
}

func validateDeviceParameterWriteRequest(req moveMCPWriteDeviceParametersRequest) error {
	if len(req.DevicePath) == 0 {
		return fmt.Errorf("device_path is required")
	}
	if len(req.DevicePath) > 16 {
		return fmt.Errorf("device_path is too deep")
	}
	for i, idx := range req.DevicePath {
		if idx < 0 || idx > 255 {
			return fmt.Errorf("device_path[%d] is out of range", i)
		}
	}
	if len(req.Parameters) == 0 {
		return fmt.Errorf("parameters is required")
	}
	if len(req.Parameters) > 64 {
		return fmt.Errorf("too many parameters: %d", len(req.Parameters))
	}
	for key, val := range req.Parameters {
		if strings.TrimSpace(key) == "" || len(key) > 160 {
			return fmt.Errorf("invalid parameter key: %q", key)
		}
		if !validMoveParameterValue(val) {
			return fmt.Errorf("parameter %s has unsupported value type", key)
		}
	}
	return nil
}

func validMoveParameterValue(v any) bool {
	switch val := v.(type) {
	case nil, bool, string, float64:
		return true
	case map[string]any:
		if len(val) > 16 {
			return false
		}
		_, hasValue := val["value"]
		if !hasValue {
			return false
		}
		for k, nested := range val {
			if len(k) > 80 {
				return false
			}
			switch k {
			case "value", "presetValue", "id", "macroMapping":
			default:
				return false
			}
			if k != "macroMapping" && !validMoveParameterValue(nested) {
				return false
			}
			if k == "macroMapping" {
				mapping, ok := nested.(map[string]any)
				if !ok || len(mapping) > 8 {
					return false
				}
			}
		}
		return true
	default:
		return false
	}
}

func writeMoveDeviceParametersToSongFile(path string, track int, req moveMCPWriteDeviceParametersRequest) (map[string]any, error) {
	root, err := readMoveSongRoot(path)
	if err != nil {
		return nil, err
	}
	trackObj, err := rawTrackObject(root, track)
	if err != nil {
		return nil, err
	}
	device, err := rawDeviceAtPath(trackObj, req.DevicePath)
	if err != nil {
		return nil, err
	}
	params, ok := device["parameters"].(map[string]any)
	if !ok {
		params = map[string]any{}
		device["parameters"] = params
	}
	for key, val := range req.Parameters {
		params[key] = val
	}
	if err := writeMoveSongAtomic(path, root); err != nil {
		return nil, err
	}
	return device, nil
}

func rawDeviceAtPath(track map[string]any, path []int) (map[string]any, error) {
	devices, ok := track["devices"].([]any)
	if !ok {
		return nil, fmt.Errorf("track has no devices array")
	}
	var current map[string]any
	for depth, idx := range path {
		if idx < 0 || idx >= len(devices) {
			return nil, fmt.Errorf("device_path[%d]=%d out of range", depth, idx)
		}
		var ok bool
		current, ok = devices[idx].(map[string]any)
		if !ok {
			return nil, fmt.Errorf("device at depth %d is not an object", depth)
		}
		if depth == len(path)-1 {
			return current, nil
		}
		chains, ok := current["chains"].([]any)
		if !ok || len(chains) == 0 {
			return nil, fmt.Errorf("device at depth %d has no chains", depth)
		}
		chainIdx := 0
		chain, ok := chains[chainIdx].(map[string]any)
		if !ok {
			return nil, fmt.Errorf("chain at depth %d is not an object", depth)
		}
		devices, ok = chain["devices"].([]any)
		if !ok {
			return nil, fmt.Errorf("chain at depth %d has no devices array", depth)
		}
	}
	return nil, fmt.Errorf("device_path is empty")
}

func readMoveMCPSampleIndex() (moveMCPSampleIndex, error) {
	raw, err := os.ReadFile(moveMCPSampleIndexPath)
	if err != nil {
		return moveMCPSampleIndex{}, err
	}
	var index moveMCPSampleIndex
	if err := json.Unmarshal(raw, &index); err != nil {
		return moveMCPSampleIndex{}, err
	}
	return index, nil
}

func writeMoveMCPSampleIndex(index moveMCPSampleIndex) error {
	data, err := json.MarshalIndent(index, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	if err := os.MkdirAll(filepath.Dir(moveMCPSampleIndexPath), 0o755); err != nil {
		return err
	}
	if err := writeFileAtomic(moveMCPSampleIndexPath, data, 0o644); err != nil {
		return err
	}
	chownToAbleton(moveMCPSampleIndexPath)
	return nil
}

func validateMoveMCPSampleIndex(index moveMCPSampleIndex) error {
	if index.Schema != "move-mcp-sample-index-v1" {
		return fmt.Errorf("schema must be move-mcp-sample-index-v1")
	}
	if len(index.Samples) > 50000 {
		return fmt.Errorf("too many samples: %d", len(index.Samples))
	}
	if index.Count != 0 && index.Count != len(index.Samples) {
		return fmt.Errorf("count does not match samples length")
	}
	seen := make(map[string]bool, len(index.Samples))
	for i, sample := range index.Samples {
		if err := validateMoveMCPSampleItem(i, sample); err != nil {
			return err
		}
		if seen[sample.ID] {
			return fmt.Errorf("samples[%d].id is duplicated", i)
		}
		seen[sample.ID] = true
	}
	return nil
}

func validateMoveMCPSampleItem(i int, sample moveMCPSampleItem) error {
	prefix := fmt.Sprintf("samples[%d]", i)
	if sample.ID == "" {
		return fmt.Errorf("%s.id is required", prefix)
	}
	if len(sample.ID) > 128 {
		return fmt.Errorf("%s.id is too long", prefix)
	}
	if sample.Path == "" {
		return fmt.Errorf("%s.path is required", prefix)
	}
	if strings.ContainsRune(sample.Path, 0) || strings.Contains(sample.Path, "..") {
		return fmt.Errorf("%s.path is invalid", prefix)
	}
	if filepath.IsAbs(sample.Path) && !strings.HasPrefix(filepath.Clean(sample.Path), moveSamplesDir) {
		return fmt.Errorf("%s.path must be under %s", prefix, moveSamplesDir)
	}
	if len(sample.Name) > 240 {
		return fmt.Errorf("%s.name is too long", prefix)
	}
	if sample.DurationSec != nil && (*sample.DurationSec < 0 || *sample.DurationSec > 24*60*60) {
		return fmt.Errorf("%s.duration_sec is out of range", prefix)
	}
	if sample.SampleRate != nil && (*sample.SampleRate < 1000 || *sample.SampleRate > 768000) {
		return fmt.Errorf("%s.sample_rate is out of range", prefix)
	}
	if sample.Channels != nil && (*sample.Channels < 1 || *sample.Channels > 64) {
		return fmt.Errorf("%s.channels is out of range", prefix)
	}
	if sample.BitDepth != nil && (*sample.BitDepth < 1 || *sample.BitDepth > 64) {
		return fmt.Errorf("%s.bit_depth is out of range", prefix)
	}
	if sample.BPM != nil && (*sample.BPM < 20 || *sample.BPM > 400) {
		return fmt.Errorf("%s.bpm is out of range", prefix)
	}
	if len(sample.Tags) > 32 || len(sample.Mood) > 32 {
		return fmt.Errorf("%s has too many tags/mood values", prefix)
	}
	return nil
}

func filterMoveMCPSamples(samples []moveMCPSampleItem, r *http.Request) []moveMCPSampleItem {
	q := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("q")))
	tag := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("tag")))
	key := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("key")))
	bpmMin, hasBPMMin := parseFloatQuery(r, "bpm_min")
	bpmMax, hasBPMMax := parseFloatQuery(r, "bpm_max")
	limit := parseIntQuery(r, "limit", 200)
	if limit < 1 {
		limit = 200
	}
	if limit > 1000 {
		limit = 1000
	}
	out := make([]moveMCPSampleItem, 0, minInt(limit, len(samples)))
	for _, sample := range samples {
		if q != "" && !sampleMatchesText(sample, q) {
			continue
		}
		if tag != "" && !sampleHasToken(sample.Tags, tag) && !sampleHasToken(sample.Mood, tag) {
			continue
		}
		if key != "" && strings.ToLower(sample.Key) != key {
			continue
		}
		if hasBPMMin && (sample.BPM == nil || *sample.BPM < bpmMin) {
			continue
		}
		if hasBPMMax && (sample.BPM == nil || *sample.BPM > bpmMax) {
			continue
		}
		out = append(out, sample)
		if len(out) >= limit {
			break
		}
	}
	return out
}

func sampleMatchesText(sample moveMCPSampleItem, q string) bool {
	fields := []string{sample.Name, sample.Path, sample.RelativePath, sample.Key, sample.Mode}
	for _, field := range fields {
		if strings.Contains(strings.ToLower(field), q) {
			return true
		}
	}
	return sampleHasToken(sample.Tags, q) || sampleHasToken(sample.Mood, q)
}

func sampleHasToken(tokens []string, q string) bool {
	for _, tok := range tokens {
		if strings.Contains(strings.ToLower(tok), q) {
			return true
		}
	}
	return false
}

func parseFloatQuery(r *http.Request, key string) (float64, bool) {
	raw := strings.TrimSpace(r.URL.Query().Get(key))
	if raw == "" {
		return 0, false
	}
	v, err := strconv.ParseFloat(raw, 64)
	if err != nil {
		return 0, false
	}
	return v, true
}

func parseIntQuery(r *http.Request, key string, fallback int) int {
	raw := strings.TrimSpace(r.URL.Query().Get(key))
	if raw == "" {
		return fallback
	}
	v, err := strconv.Atoi(raw)
	if err != nil {
		return fallback
	}
	return v
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func writeMoveSongAtomic(path string, song any) error {
	data, err := json.MarshalIndent(song, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	var check map[string]any
	if err := json.Unmarshal(data, &check); err != nil {
		return err
	}
	if err := writeFileAtomic(path, data, 0o644); err != nil {
		return err
	}
	chownToAbleton(path)
	return nil
}

func writeFileAtomic(path string, data []byte, mode os.FileMode) error {
	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, "."+filepath.Base(path)+".tmp.*")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	cleanup := func() { _ = os.Remove(tmpPath) }
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		cleanup()
		return err
	}
	if err := tmp.Close(); err != nil {
		cleanup()
		return err
	}
	if err := os.Chmod(tmpPath, mode); err != nil {
		cleanup()
		return err
	}
	if err := os.Rename(tmpPath, path); err != nil {
		cleanup()
		return err
	}
	return nil
}
