package main

import (
	"os"
	"path/filepath"
	"testing"
)

func writeModuleJSON(t *testing.T, base, sub, id, body string) {
	t.Helper()
	dir := filepath.Join(base, "modules", sub, id)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "module.json"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestDiscoverSlotModules(t *testing.T) {
	base := t.TempDir()
	writeModuleJSON(t, base, "sound_generators", "osirus",
		`{"id":"osirus","name":"Osirus","capabilities":{"chainable":true,"component_type":"sound_generator"}}`)
	writeModuleJSON(t, base, "sound_generators", "linein",
		`{"id":"linein","name":"Line In","component_type":"sound_generator","capabilities":{"chainable":true,"audio_in":true}}`)
	writeModuleJSON(t, base, "audio_fx", "freeverb",
		`{"id":"freeverb","name":"Freeverb","component_type":"audio_fx","capabilities":{"chainable":true}}`)
	writeModuleJSON(t, base, "tools", "song-mode",
		`{"id":"song-mode","name":"Song Mode","component_type":"tool","tool_config":{"interactive":true,"skip_file_browser":true}}`)
	writeModuleJSON(t, base, "tools", "wav-player",
		`{"id":"wav-player","name":"WAV Player","component_type":"tool"}`)
	writeModuleJSON(t, base, "tools", "dbx",
		`{"id":"dbx","name":"DBX","component_type":"tool","standalone":true,"tool_config":{"interactive":true,"skip_file_browser":true}}`)
	writeModuleJSON(t, base, "overtake", "m8",
		`{"id":"m8","name":"M8","component_type":"overtake"}`)
	writeModuleJSON(t, base, "sound_generators", "standalone",
		`{"id":"standalone","name":"Standalone","component_type":"sound_generator"}`)

	got := discoverSlotModules(base)
	want := []SlotModule{
		{ID: "freeverb", Name: "Freeverb", ComponentType: "audio_fx",
			DSPPath: filepath.Join(base, "modules", "audio_fx", "freeverb", "dsp.so")},
		{ID: "m8", Name: "M8", ComponentType: "overtake"},
		{ID: "linein", Name: "Line In", ComponentType: "sound_generator", DeviceOnly: true},
		{ID: "osirus", Name: "Osirus", ComponentType: "sound_generator"},
		{ID: "dbx", Name: "DBX", ComponentType: "tool", DeviceOnly: true},
		{ID: "song-mode", Name: "Song Mode", ComponentType: "tool"},
		{ID: "wav-player", Name: "WAV Player", ComponentType: "tool", DeviceOnly: true},
	}
	if len(got) != len(want) {
		t.Fatalf("got %d modules %+v, want %d", len(got), got, len(want))
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("entry %d: got %+v, want %+v", i, got[i], want[i])
		}
	}
}

// A browser load is checked on the SERVER, not only in the page: any client on
// the network can send a "<comp>:module" write, and a line-in sound generator
// loaded that way skips the device's speaker-feedback gate. The rule is the
// loader list's own: the value must be something /api/slot-modules offered for
// that kind of position, and not DeviceOnly.
func TestModuleLoadRefusal(t *testing.T) {
	base := t.TempDir()
	writeModuleJSON(t, base, "sound_generators", "osirus",
		`{"id":"osirus","name":"Osirus","capabilities":{"chainable":true,"component_type":"sound_generator"}}`)
	writeModuleJSON(t, base, "sound_generators", "linein",
		`{"id":"linein","name":"Line In","component_type":"sound_generator","capabilities":{"chainable":true,"audio_in":true}}`)
	writeModuleJSON(t, base, "audio_fx", "freeverb",
		`{"id":"freeverb","name":"Freeverb","component_type":"audio_fx","dsp":"freeverb.so","capabilities":{"chainable":true}}`)
	writeModuleJSON(t, base, "midi_fx", "arp",
		`{"id":"arp","name":"Arp","component_type":"midi_fx","capabilities":{"chainable":true}}`)
	fvPath := filepath.Join(base, "modules", "audio_fx", "freeverb", "freeverb.so")

	cases := []struct {
		key, value string
		allowed    bool
	}{
		// Not a module load: never gated.
		{"synth:cutoff", "0.5", true},
		// Clears always pass.
		{"synth:module", "none", true},
		{"fx1:module", "", true},
		{"master_fx:fx3:module", "", true},
		// Slot positions take an id of the matching type.
		{"synth:module", "osirus", true},
		{"fx2:module", "freeverb", true},
		{"midi_fx1:module", "arp", true},
		{"synth:module", "freeverb", false}, // wrong type for the position
		{"fx1:module", "osirus", false},     // wrong type for the position
		{"synth:module", "nonexistent", false},
		{"synth:module", "../../bin/x", false},
		// Line-in: listed, never loadable from a browser.
		{"synth:module", "linein", false},
		// Master FX takes the discovered dsp PATH, nothing else.
		{"master_fx:fx1:module", fvPath, true},
		{"master_fx:fx8:module", fvPath, true},
		{"master_fx:fx1:module", "freeverb", false},
		{"master_fx:fx1:module", "/data/UserData/evil.so", false},
		// Keys this loader does not write pass through unchanged.
		{"bus1:fx2:module", "whatever", true},
	}
	for _, c := range cases {
		reason := moduleLoadRefusal(base, c.key, c.value)
		if c.allowed && reason != "" {
			t.Errorf("%s=%q refused (%s), want allowed", c.key, c.value, reason)
		}
		if !c.allowed && reason == "" {
			t.Errorf("%s=%q allowed, want refused", c.key, c.value)
		}
	}
}
