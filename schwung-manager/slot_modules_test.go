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
