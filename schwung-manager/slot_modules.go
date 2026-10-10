package main

// slot_modules.go -- what the Remote UI's slot loader may put in a slot.
//
// The Remote UI could always WRITE "<comp>:module" (set_param passes any key
// through to the shim), but it had no way to know what was installed, so the
// only module picker was the one on the device. GET /api/slot-modules lists the
// chainable modules on disk, grouped by the chain position they can fill,
// plus the overtake modules the Tool tab can launch.
//
// Read from disk, never from the catalog: a module the catalog does not carry
// (a custom install, a built-in like freeverb) is still loadable, and the
// catalog may be unreachable.
//
// A sound generator that consumes line-in is listed but marked DeviceOnly. The
// device's picker runs the speaker-feedback gate before loading one
// (feedback_gate.mjs); a write from a phone would skip it, so the web picker
// refuses rather than re-implementing the gate.

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"sort"
)

// SlotModule is one entry of the loader's list.
type SlotModule struct {
	ID            string `json:"id"`
	Name          string `json:"name"`
	ComponentType string `json:"component_type"`
	DeviceOnly    bool   `json:"device_only,omitempty"`
	// DSPPath is set for audio FX: the master bus loads a position by path.
	DSPPath string `json:"dsp_path,omitempty"`
}

// slotModuleFile is the part of module.json the loader reads.
type slotModuleFile struct {
	ID            string `json:"id"`
	Name          string `json:"name"`
	ComponentType string `json:"component_type"`
	DSP           string `json:"dsp"`
	Capabilities  struct {
		Chainable     bool   `json:"chainable"`
		AudioIn       bool   `json:"audio_in"`
		ComponentType string `json:"component_type"`
	} `json:"capabilities"`
}

// slotComponentTypes are the component types a chain slot position accepts.
var slotComponentTypes = map[string]bool{
	"sound_generator": true,
	"audio_fx":        true,
	"midi_fx":         true,
}

// discoverSlotModules lists every chainable module installed under base,
// sorted by type then name.
func discoverSlotModules(base string) []SlotModule {
	seen := map[string]bool{}
	out := []SlotModule{}
	for _, dir := range moduleInstallDirs(base) {
		entries, err := os.ReadDir(dir)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if !e.IsDir() {
				continue
			}
			data, err := os.ReadFile(filepath.Join(dir, e.Name(), "module.json"))
			if err != nil {
				continue
			}
			var m slotModuleFile
			if json.Unmarshal(data, &m) != nil || m.ID == "" || seen[m.ID] {
				continue
			}
			ct := m.ComponentType
			if ct == "" {
				ct = m.Capabilities.ComponentType
			}
			// Overtake modules are listed for the Tool tab's launcher;
			// everything else must be chainable into a slot position.
			if ct != "overtake" && (!m.Capabilities.Chainable || !slotComponentTypes[ct]) {
				continue
			}
			seen[m.ID] = true
			name := m.Name
			if name == "" {
				name = m.ID
			}
			sm := SlotModule{
				ID:            m.ID,
				Name:          name,
				ComponentType: ct,
				DeviceOnly:    ct == "sound_generator" && m.Capabilities.AudioIn,
			}
			if ct == "audio_fx" {
				dsp := m.DSP
				if dsp == "" {
					dsp = "dsp.so"
				}
				sm.DSPPath = filepath.Join(dir, e.Name(), dsp)
			}
			out = append(out, sm)
		}
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].ComponentType != out[j].ComponentType {
			return out[i].ComponentType < out[j].ComponentType
		}
		return out[i].Name < out[j].Name
	})
	return out
}

func (app *App) handleAPISlotModules(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	json.NewEncoder(w).Encode(discoverSlotModules(app.basePath))
}
