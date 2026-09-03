package main

// Module identity for the CPU page, read from DISK rather than from the shim.
//
// Every one of these reads used to be a param request, and a param request is
// served by the shim ON THE SPI CALLBACK. Twelve per refresh (4 slots + 8
// Master FX) measurably moved the thing this page exists to measure: the
// `Param requests` section's max went from ~36 us to ~140 us once the page
// started polling. An instrument that perturbs its subject by that much is not
// reporting the device, it is reporting itself. Identity is already on disk, so
// take it from there and leave the callback alone.
//
// THE SCHEMA IS NOT WHAT YOU WOULD GUESS. Checked against a real device:
//
//	active_set.txt      line 1 = the set uuid, line 2 = a human name
//	                    NOT the newest mtime. The device had 27 sets; mtime
//	                    order and glob order each pointed at a DIFFERENT
//	                    wrong one.
//
//	slot_N.json         chain.synth.module        NOT synth.module
//	                    chain.audio_fx[].type     the key is "type", not "module"
//	                    chain.midi_fx[].type
//
//	master_fx_N.json    module_id                 a different key again
//
// An empty position is `{}` or has `"synth": null`. That is a FINDING, not a
// failure — it reads as empty with Read set, distinct from a file we could not
// read at all.

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

// SlotState is one chain slot's contents. Read distinguishes "we parsed this
// file and the slot is empty" from "we never got to look" — the same three-way
// distinction moduleID keeps, at the file layer.
type SlotState struct {
	Synth   string
	AudioFX []string
	MidiFX  []string
	Read    bool
}

// SetState is the whole rig as the active set's files describe it.
type SetState struct {
	UUID     string
	Name     string
	Slots    [perfChainSlots]SlotState
	MasterFX [perfMasterFXSlots]string
}

// readSetState reads the ACTIVE set's slot and Master FX files.
//
// It errors rather than returning an empty rig when the set cannot be located:
// "there is nothing loaded" and "we could not find out what is loaded" are
// different findings, and only the first may be drawn as an empty page.
func readSetState(basePath string) (*SetState, error) {
	raw, err := os.ReadFile(filepath.Join(basePath, "active_set.txt"))
	if err != nil {
		return nil, err
	}
	lines := strings.Split(string(raw), "\n")
	uuid := strings.TrimSpace(lines[0])
	if uuid == "" {
		return nil, errors.New("active_set.txt has no set uuid on its first line")
	}
	set := &SetState{UUID: uuid}
	if len(lines) > 1 {
		set.Name = strings.TrimSpace(lines[1])
	}

	dir := filepath.Join(basePath, "set_state", uuid)
	info, err := os.Stat(dir)
	if err != nil {
		return nil, err
	}
	if !info.IsDir() {
		return nil, errors.New("set_state/" + uuid + " is not a directory")
	}

	for i := 0; i < perfChainSlots; i++ {
		set.Slots[i] = readSlotFile(filepath.Join(dir, "slot_"+strconv.Itoa(i)+".json"))
	}
	for i := 0; i < perfMasterFXSlots; i++ {
		set.MasterFX[i] = readMasterFXFile(
			filepath.Join(dir, "master_fx_"+strconv.Itoa(i)+".json"))
	}
	return set, nil
}

// readSlotFile models ONLY the identity keys. These files also carry each
// module's whole opaque state blob, which we neither need nor want to walk.
func readSlotFile(path string) SlotState {
	raw, err := os.ReadFile(path)
	if err != nil {
		return SlotState{} // Read stays false: we never got to look.
	}
	var f struct {
		Chain struct {
			Synth *struct {
				Module string `json:"module"`
			} `json:"synth"`
			AudioFX []struct {
				Type string `json:"type"`
			} `json:"audio_fx"`
			MidiFX []struct {
				Type string `json:"type"`
			} `json:"midi_fx"`
		} `json:"chain"`
	}
	if err := json.Unmarshal(raw, &f); err != nil {
		return SlotState{}
	}

	// Read is set BEFORE looking at the contents: the parse succeeding is what
	// it reports, and everything below may legitimately be empty.
	s := SlotState{Read: true}
	if f.Chain.Synth != nil {
		s.Synth = f.Chain.Synth.Module
	}
	for _, e := range f.Chain.AudioFX {
		if e.Type != "" {
			s.AudioFX = append(s.AudioFX, e.Type)
		}
	}
	for _, e := range f.Chain.MidiFX {
		if e.Type != "" {
			s.MidiFX = append(s.MidiFX, e.Type)
		}
	}
	return s
}

// readMasterFXFile returns the module id of one Master FX position.
func readMasterFXFile(path string) string {
	raw, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	var f struct {
		ModuleID string `json:"module_id"`
	}
	if err := json.Unmarshal(raw, &f); err != nil {
		return ""
	}
	return f.ModuleID
}

// moduleForksProcesses reports an installed module's forks_processes
// capability, and whether we could find out at all.
//
// Modules live at modules/<component_type>s/<id>/module.json and the type is
// not recorded anywhere we are reading here, so this globs rather than guessing
// a directory — a guess that missed would report a confident false.
func moduleForksProcesses(basePath, id string) (forks bool, ok bool) {
	matches, err := filepath.Glob(filepath.Join(basePath, "modules", "*", id, "module.json"))
	if err != nil || len(matches) == 0 {
		return false, false
	}
	raw, err := os.ReadFile(matches[0])
	if err != nil {
		return false, false
	}
	var f struct {
		Capabilities struct {
			ForksProcesses bool `json:"forks_processes"`
		} `json:"capabilities"`
	}
	if err := json.Unmarshal(raw, &f); err != nil {
		return false, false
	}
	return f.Capabilities.ForksProcesses, true
}

// moduleID is what a module-identity read produced, keeping the THREE answers
// apart: a name, "this position is empty", and "the read did not complete".
//
// Collapsing the last two is a real hazard here. If a failed read is treated as
// "empty", a slot that is BURNING CPU disappears from the page entirely,
// because the row is drawn only where a module id was found. On a page whose
// whole job is finding what costs time, silently hiding a busy slot is the
// worst failure available.
type moduleID struct {
	Name     string
	Answered bool // we found out, whatever the answer was
}

// Loaded reports whether a module is actually there.
func (m moduleID) Loaded() bool { return m.Answered && m.Name != "" }

// moduleIDCache holds the parsed set state between polls.
type moduleIDCache struct {
	mu     sync.Mutex
	set    *SetState
	readAt time.Time
}

// moduleIDRefresh is how often the set state is re-read. Short, because these
// are ordinary file reads on tmpfs-backed storage — the old 15 s was sized for
// param requests the SPI callback had to serve.
const moduleIDRefresh = 2 * time.Second
