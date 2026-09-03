package main

import (
	"os"
	"path/filepath"
	"testing"
)

// realSlot0 is the shape a real slot_N.json actually has, trimmed of the module
// state blob. It is kept verbatim because every key here was got wrong at least
// once before being checked against a device: the module lives at
// chain.synth.module, and the FX entries are keyed "type", not "module".
const realSlot0 = `{"name":"s","version":1,"chain":{"custom_name":"Untitled",
 "input":"both","synth":{"module":"jp8000","config":{"state":"..."}},
 "audio_fx":[{"type":"ducker"}],"midi_fx":[{"type":"arp"}]}}`

// writeSet lays out base/set_state/<uuid>/... and points active_set.txt at it.
func writeSet(t *testing.T, base, uuid string, files map[string]string) {
	t.Helper()
	dir := filepath.Join(base, "set_state", uuid)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	for name, body := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	// Line 1 is the uuid, line 2 a human name. Both matter; only line 1 selects.
	if err := os.WriteFile(filepath.Join(base, "active_set.txt"),
		[]byte(uuid+"\nSet 11\n"), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestModulesReadsTheRealSchema(t *testing.T) {
	base := t.TempDir()
	writeSet(t, base, "uuid-a", map[string]string{
		"slot_0.json":      realSlot0,
		"slot_1.json":      `{"chain":{"synth":{"module":"9w9"}}}`,
		"master_fx_0.json": `{"module_id":"ottx"}`,
	})

	set, err := readSetState(base)
	if err != nil {
		t.Fatalf("readSetState: %v", err)
	}
	if set.UUID != "uuid-a" {
		t.Errorf("UUID = %q, want uuid-a (line 1 of active_set.txt)", set.UUID)
	}
	if set.Name != "Set 11" {
		t.Errorf("Name = %q, want %q (line 2 of active_set.txt)", set.Name, "Set 11")
	}
	if got := set.Slots[0].Synth; got != "jp8000" {
		t.Errorf("slot 0 synth = %q, want jp8000 — the key path is "+
			"chain.synth.module, NOT synth.module", got)
	}
	if got := set.Slots[1].Synth; got != "9w9" {
		t.Errorf("slot 1 synth = %q, want 9w9", got)
	}
	if len(set.Slots[0].AudioFX) != 1 || set.Slots[0].AudioFX[0] != "ducker" {
		t.Errorf("slot 0 audio_fx = %v, want [ducker] — the key is "+
			"chain.audio_fx[].type, NOT .module", set.Slots[0].AudioFX)
	}
	if len(set.Slots[0].MidiFX) != 1 || set.Slots[0].MidiFX[0] != "arp" {
		t.Errorf("slot 0 midi_fx = %v, want [arp] — the key is "+
			"chain.midi_fx[].type, NOT .module", set.Slots[0].MidiFX)
	}
	if set.MasterFX[0] != "ottx" {
		t.Errorf("master_fx 0 = %q, want ottx — master_fx_N.json uses "+
			"module_id, a different key again", set.MasterFX[0])
	}
}

func TestModulesEmptyIsNotFailure(t *testing.T) {
	base := t.TempDir()
	writeSet(t, base, "uuid-a", map[string]string{
		"slot_0.json": `{}`,
		"slot_1.json": `{"chain":{"synth":null}}`,
	})

	set, err := readSetState(base)
	if err != nil {
		t.Fatalf("readSetState: %v", err)
	}
	for _, i := range []int{0, 1} {
		if set.Slots[i].Synth != "" {
			t.Errorf("slot %d synth = %q, want empty", i, set.Slots[i].Synth)
		}
		if !set.Slots[i].Read {
			t.Errorf("slot %d: an empty position is a FINDING, not a failure. "+
				"Read must be true so that 'we looked and it is empty' and "+
				"'we could not look' do not share one representation.", i)
		}
	}
	// A slot file that does not exist is the other half of that distinction.
	if set.Slots[2].Read {
		t.Error("slot 2 has no file at all — Read must be false")
	}
}

func TestModulesUsesActiveSetNotNewest(t *testing.T) {
	base := t.TempDir()
	writeSet(t, base, "aaa-correct", map[string]string{
		"slot_0.json": `{"chain":{"synth":{"module":"correct"}}}`,
	})
	// Alphabetically later AND newer. Both orderings point here; neither is
	// the authority.
	zdir := filepath.Join(base, "set_state", "zzz-newer")
	if err := os.MkdirAll(zdir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(zdir, "slot_0.json"),
		[]byte(`{"chain":{"synth":{"module":"wrong"}}}`), 0o644); err != nil {
		t.Fatal(err)
	}

	set, err := readSetState(base)
	if err != nil {
		t.Fatalf("readSetState: %v", err)
	}
	if set.Slots[0].Synth != "correct" {
		t.Errorf("slot 0 synth = %q, want correct. active_set.txt is the ONLY "+
			"authority for which set is live — the device had 27 sets and mtime "+
			"order and glob order each chose a different wrong one.",
			set.Slots[0].Synth)
	}
}

func TestModulesMissingActiveSetIsAFailure(t *testing.T) {
	if _, err := readSetState(t.TempDir()); err == nil {
		t.Error("a missing active_set.txt must be an error, not a rig that " +
			"reads as entirely empty")
	}
}

func TestModulesActiveSetNamingAMissingDirIsAFailure(t *testing.T) {
	base := t.TempDir()
	if err := os.WriteFile(filepath.Join(base, "active_set.txt"),
		[]byte("no-such-uuid\nSet 1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := readSetState(base); err == nil {
		t.Error("active_set.txt naming a directory that does not exist must be " +
			"an error, not an empty rig")
	}
}

func TestModuleCapabilitiesReadsForksProcesses(t *testing.T) {
	base := t.TempDir()
	dir := filepath.Join(base, "modules", "sound_generators", "jp8000")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "module.json"),
		[]byte(`{"id":"jp8000","capabilities":{"forks_processes":true}}`), 0o644); err != nil {
		t.Fatal(err)
	}

	forks, ok := moduleForksProcesses(base, "jp8000")
	if !ok || !forks {
		t.Errorf("moduleForksProcesses = (%v, %v), want (true, true)", forks, ok)
	}
	if _, ok := moduleForksProcesses(base, "nosuch"); ok {
		t.Error("an unknown module must return ok=false, not a confident false")
	}
}
