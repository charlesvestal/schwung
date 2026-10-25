package main

// slot_modules.go -- what the Remote UI's slot loader may put in a slot.
//
// The Remote UI could always WRITE "<comp>:module" (set_param passes any key
// through to the shim), but it had no way to know what was installed, so the
// only module picker was the one on the device. GET /api/slot-modules lists the
// chainable modules on disk, grouped by the chain position they can fill,
// plus the overtake modules and tools the Tool tab can launch.
//
// Read from disk, never from the catalog: a module the catalog does not carry
// (a custom install, a built-in like freeverb) is still loadable, and the
// catalog may be unreachable.
//
// A sound generator that consumes line-in is listed but marked DeviceOnly. The
// device's picker runs the speaker-feedback gate before loading one
// (feedback_gate.mjs); a write from a phone would skip it, so the web picker
// refuses rather than re-implementing the gate.
//
// A tool (component_type "tool") is listed too. Only one that the device
// starts straight away (tool_config interactive + skip_file_browser) can be
// launched from a phone: the launch command carries a file path, so a tool
// that opens a file browser, a set picker or a standalone binary first is
// marked DeviceOnly (tool_launch.mjs decides the same order on the device).

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
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
	Standalone    bool   `json:"standalone"`
	Capabilities  struct {
		Chainable     bool   `json:"chainable"`
		AudioIn       bool   `json:"audio_in"`
		ComponentType string `json:"component_type"`
		Standalone    bool   `json:"standalone"`
	} `json:"capabilities"`
	ToolConfig *struct {
		Interactive     bool `json:"interactive"`
		SkipFileBrowser bool `json:"skip_file_browser"`
		SetPicker       bool `json:"set_picker"`
	} `json:"tool_config"`
}

// launchesDirectly mirrors toolLaunchKind() == "interactive".
func (m *slotModuleFile) launchesDirectly() bool {
	if m.Standalone || m.Capabilities.Standalone || m.ToolConfig == nil {
		return false
	}
	tc := m.ToolConfig
	return !tc.SetPicker && tc.Interactive && tc.SkipFileBrowser
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
			// Overtake modules and tools are listed for the Tool tab's
			// launcher; everything else must be chainable into a slot position.
			if ct != "overtake" && ct != "tool" && (!m.Capabilities.Chainable || !slotComponentTypes[ct]) {
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
				DeviceOnly: (ct == "sound_generator" && m.Capabilities.AudioIn) ||
					(ct == "tool" && !m.launchesDirectly()),
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

// slotModuleKey matches the chain-slot positions this loader writes:
// "synth:module", "fx<N>:module", "midi_fx<N>:module". masterFxModuleKey
// matches a master-bus position, which is loaded by dsp PATH.
var (
	slotModuleKey     = regexp.MustCompile(`^(synth|fx[0-9]+|midi_fx[0-9]+):module$`)
	masterFxModuleKey = regexp.MustCompile(`^master_fx:fx[0-9]+:module$`)
)

// moduleLoadRefusal decides a module write on the SERVER. It returns "" when
// the write may go to the device, or why it may not.
//
// The page greys out what it cannot load, but any client can send the write,
// and the device's speaker-feedback gate runs in its own picker -- a line-in
// sound generator loaded from here would skip it. So the rule the loader list
// already states is enforced here too: the value must be something
// discoverSlotModules offers for that kind of position, and not DeviceOnly. A
// master-bus position takes the discovered dsp path and nothing else, which
// also keeps an arbitrary path from reaching dlopen.
//
// Clears ("" / "none") always pass. Keys this loader does not write (a bus
// insert, a parameter) pass through unchanged.
func moduleLoadRefusal(base, key, value string) string {
	isSlot := slotModuleKey.MatchString(key)
	isMaster := masterFxModuleKey.MatchString(key)
	if !isSlot && !isMaster {
		return ""
	}
	if value == "" || value == "none" {
		return ""
	}
	want := "audio_fx"
	if isSlot {
		switch comp := strings.TrimSuffix(key, ":module"); {
		case comp == "synth":
			want = "sound_generator"
		case strings.HasPrefix(comp, "midi_fx"):
			want = "midi_fx"
		}
	}
	for _, m := range discoverSlotModules(base) {
		if m.ComponentType != want {
			continue
		}
		if (isMaster && m.DSPPath == value) || (isSlot && m.ID == value) {
			if m.DeviceOnly {
				return m.Name + " can only be loaded on the device"
			}
			return ""
		}
	}
	return "not an installed " + want + " module: " + value
}
