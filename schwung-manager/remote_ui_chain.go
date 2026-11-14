package main

import (
	"context"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"time"
)

// ---------------------------------------------------------------------------
// What a chain slot and the master bus HOLD, and how a module load ends.
// ---------------------------------------------------------------------------
//
// The Remote UI used to know fx1/fx2/midi_fx1 for a slot and fx1..fx4 for the
// master bus. The device has eight of each: a third audio FX loaded, ran and
// made sound while the browser could not see it, and Master FX 5-8 did not
// exist. The positions are now DISCOVERED from the device's own listings --
// the chain's `fx_count` / `midi_fx_count` and the shim's one-shot
// `master_fx:modules` -- and only positions that exist are read.

// The caps, in ONE place. Each mirrors a C constant and
// TestChainCapsMatchTheCHeaders parses the header so the two cannot drift.
const (
	// MASTER_FX_SLOTS, src/host/shadow_chain_mgmt.h.
	masterFxSlotCount = 8
	// MAX_AUDIO_FX, src/modules/chain/dsp/chain_internal.h.
	chainMaxAudioFx = 8
	// MAX_MIDI_FX, src/modules/chain/dsp/chain_internal.h.
	chainMaxMidiFx = 8
)

// slotChain is one slot's chain as the device reports it. A section whose
// reads did not all complete is NOT KNOWN, and must never be reported as
// empty: a timed-out read is not news about the slot (see CLAUDE.md, "A param
// read has THREE answers").
type slotChain struct {
	Synth       string
	SynthKnown  bool
	MidiFX      []string // midi_fx1..N, trailing empties trimmed
	MidiFXKnown bool
	FX          []string // fx1..N, trailing empties trimmed
	FXKnown     bool
}

// readSlotChain asks a slot what it holds: synth_module, then each section's
// count and only the positions that count names. An empty slot is three
// reads; nothing is read for a position that does not exist.
//
// The first read that does not complete ends the walk -- under contention
// every read behind it would wait out its own timeout, and the answer would
// be unknown anyway. An ANSWERED error (the shim replied, e.g. no chain
// instance) is a real "nothing here".
func readSlotChain(get paramGetter, slot uint8) slotChain {
	var ch slotChain
	ch.Synth, ch.SynthKnown = readSlotParam(get, slot, "synth_module")
	if !ch.SynthKnown {
		return ch
	}
	if ch.MidiFX, ch.MidiFXKnown = readSlotSection(get, slot, "midi_fx", chainMaxMidiFx); !ch.MidiFXKnown {
		return ch
	}
	ch.FX, ch.FXKnown = readSlotSection(get, slot, "fx", chainMaxAudioFx)
	return ch
}

// readSlotParam is one read. ok=false: it did not complete. An ANSWERED error
// (the shim replied, e.g. no chain instance in the slot) is a real "".
func readSlotParam(get paramGetter, slot uint8, key string) (string, bool) {
	v, err := get(slot, key)
	if err != nil {
		return "", paramAnswered(err)
	}
	return strings.TrimSpace(v), true
}

// readSlotSection reads "<prefix>_count" and then "<prefix>N_module" for each
// position it names (prefix "fx" or "midi_fx"), stopping at the first read
// that does not complete. A hole in FRONT of a module is kept -- position i
// IS <prefix>(i+1) -- but trailing empties are dropped, as shadow_ui.js does.
func readSlotSection(get paramGetter, slot uint8, prefix string, maxN int) ([]string, bool) {
	raw, ok := readSlotParam(get, slot, prefix+"_count")
	if !ok {
		return nil, false
	}
	n := 0
	if raw != "" {
		v, err := strconv.Atoi(raw)
		if err != nil || v < 0 {
			return nil, false // garbage is not a count of zero
		}
		n = v
	}
	if n > maxN {
		n = maxN
	}
	mods := make([]string, 0, n)
	for i := 1; i <= n; i++ {
		v, ok := readSlotParam(get, slot, prefix+strconv.Itoa(i)+"_module")
		if !ok {
			return nil, false
		}
		mods = append(mods, v)
	}
	for len(mods) > 0 && mods[len(mods)-1] == "" {
		mods = mods[:len(mods)-1]
	}
	return mods, true
}

// moduleOf answers what a component holds. known=false: its section was not
// read this time, so the caller must keep what it had.
func (ch slotChain) moduleOf(comp string) (mod string, known bool) {
	if comp == "synth" {
		return ch.Synth, ch.SynthKnown
	}
	if n, ok := chainPosition(comp, "midi_fx", chainMaxMidiFx); ok {
		if !ch.MidiFXKnown {
			return "", false
		}
		if n <= len(ch.MidiFX) {
			return ch.MidiFX[n-1], true
		}
		return "", true
	}
	if n, ok := chainPosition(comp, "fx", chainMaxAudioFx); ok {
		if !ch.FXKnown {
			return "", false
		}
		if n <= len(ch.FX) {
			return ch.FX[n-1], true
		}
		return "", true
	}
	return "", false
}

// loaded lists the components holding a module, in enumeration order
// (synth, midi_fx1.., fx1..). Unknown sections contribute nothing.
func (ch slotChain) loaded() []string {
	var out []string
	if ch.SynthKnown && ch.Synth != "" {
		out = append(out, "synth")
	}
	if ch.MidiFXKnown {
		for i, m := range ch.MidiFX {
			if m != "" {
				out = append(out, "midi_fx"+strconv.Itoa(i+1))
			}
		}
	}
	if ch.FXKnown {
		for i, m := range ch.FX {
			if m != "" {
				out = append(out, "fx"+strconv.Itoa(i+1))
			}
		}
	}
	return out
}

// unknownSections names what the page must keep rather than reset.
func (ch slotChain) unknownSections() []string {
	var out []string
	if !ch.SynthKnown {
		out = append(out, "synth")
	}
	if !ch.MidiFXKnown {
		out = append(out, "midi_fx")
	}
	if !ch.FXKnown {
		out = append(out, "fx")
	}
	return out
}

// chainPosition parses "fx3" (prefix "fx") into 3, within 1..maxN. "midi_fx3"
// does not parse as an "fx" position.
func chainPosition(comp, prefix string, maxN int) (int, bool) {
	if !strings.HasPrefix(comp, prefix) {
		return 0, false
	}
	n, err := strconv.Atoi(comp[len(prefix):])
	if err != nil || n < 1 || n > maxN || strconv.Itoa(n) != comp[len(prefix):] {
		return 0, false
	}
	return n, true
}

// isChainComponent reports whether comp names a slot component: the synth or
// any audio / MIDI FX position up to the chain's caps. Master FX keys
// ("master_fx:fx1:...") split differently and are excluded.
func isChainComponent(comp string) bool {
	if comp == "synth" {
		return true
	}
	if _, ok := chainPosition(comp, "midi_fx", chainMaxMidiFx); ok {
		return true
	}
	_, ok := chainPosition(comp, "fx", chainMaxAudioFx)
	return ok
}

// slotInfoFrom builds the slot_info message from one listing, so describing a
// slot to N clients costs one walk, not N.
func slotInfoFrom(slot uint8, ch slotChain) wsSlotInfo {
	info := wsSlotInfo{
		Type:      "slot_info",
		Slot:      slot,
		Synth:     ch.Synth,
		FXCap:     chainMaxAudioFx,
		MidiFXCap: chainMaxMidiFx,
		Unknown:   ch.unknownSections(),
	}
	if ch.FXKnown {
		info.FX = append([]string{}, ch.FX...)
	}
	if ch.MidiFXKnown {
		info.MidiFX = append([]string{}, ch.MidiFX...)
	}
	// The old fixed fields, for a page cached from before the lists existed.
	info.FX1, _ = ch.moduleOf("fx1")
	info.FX2, _ = ch.moduleOf("fx2")
	info.MidiFX1, _ = ch.moduleOf("midi_fx1")
	return info
}

// masterFxChain is the master bus, positional (entry i IS master_fx:fx(i+1)),
// never compacted. Known=false: the listing did not complete.
type masterFxChain struct {
	Modules []string
	Known   bool
}

// readMasterFxChain is ONE read: `master_fx:modules`, the shim's own answer
// for the whole bus. A shim too old to serve it ANSWERS with an error, and
// only then are the positions read one by one.
func readMasterFxChain(get paramGetter) masterFxChain {
	raw, err := get(0, "master_fx:modules")
	if err != nil {
		if !paramAnswered(err) {
			return masterFxChain{}
		}
		return readMasterFxPositions(get)
	}
	var entries []struct {
		ID   string `json:"id"`
		Path string `json:"path"`
	}
	if json.Unmarshal([]byte(raw), &entries) != nil {
		return masterFxChain{}
	}
	mods := make([]string, masterFxSlotCount)
	for i := 0; i < len(entries) && i < masterFxSlotCount; i++ {
		id := entries[i].ID
		if id == "" {
			id = entries[i].Path
		}
		mods[i] = masterFxModuleID(id)
	}
	return masterFxChain{Modules: mods, Known: true}
}

func readMasterFxPositions(get paramGetter) masterFxChain {
	mods := make([]string, masterFxSlotCount)
	for i := range mods {
		v, err := get(0, masterFxComp(i+1)+":module")
		if err != nil && !paramAnswered(err) {
			return masterFxChain{}
		}
		if err == nil {
			mods[i] = masterFxModuleID(strings.TrimSpace(v))
		}
	}
	return masterFxChain{Modules: mods, Known: true}
}

// masterFxComp names position n (1-based): "master_fx:fx3".
func masterFxComp(n int) string { return "master_fx:fx" + strconv.Itoa(n) }

// loaded lists the master components holding a module, in position order.
func (m masterFxChain) loaded() []string {
	var out []string
	for i, id := range m.Modules {
		if id != "" {
			out = append(out, masterFxComp(i+1))
		}
	}
	return out
}

func masterFxInfoFrom(m masterFxChain) wsMasterFxInfo {
	info := wsMasterFxInfo{Type: "master_fx_info", Cap: masterFxSlotCount}
	if !m.Known {
		info.Unknown = true
		return info
	}
	info.FX = append([]string{}, m.Modules...)
	at := func(i int) string {
		if i < len(m.Modules) {
			return m.Modules[i]
		}
		return ""
	}
	info.FX1, info.FX2, info.FX3, info.FX4 = at(0), at(1), at(2), at(3)
	return info
}

// ---------------------------------------------------------------------------
// How a module load ENDS
// ---------------------------------------------------------------------------

type loadOutcome int

const (
	loadLanded loadOutcome = iota
	loadFailed
	loadTimedOut
	loadCancelled
)

type loadResult struct {
	Outcome loadOutcome
	Detail  string // the device's own error text, when it gave one
}

// loadWaitTiming bounds a wait. Production uses defaultLoadWait; tests shrink
// it.
type loadWaitTiming struct {
	Settle    time.Duration // before the first read
	Poll      time.Duration // between reads
	Cap       time.Duration // give up: the slot is re-described as it stands
	FailGrace time.Duration // a served mismatch must persist this long to be a failure
}

var defaultLoadWait = loadWaitTiming{
	Settle:    moduleSwapSettle,
	Poll:      250 * time.Millisecond,
	Cap:       moduleSwapWait,
	FailGrace: time.Second,
}

// sleepCtx waits d or until ctx ends; false if ctx ended.
func sleepCtx(ctx context.Context, d time.Duration) bool {
	if d <= 0 {
		return ctx.Err() == nil
	}
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-t.C:
		return true
	}
}

// waitSlotLoad waits for a "<comp>:module" write on a chain slot to settle.
//
// The shim holds a slot load's ANSWER until its loader has committed (or
// failed), and serves nothing else meanwhile -- so once the write was
// answered, any read that is served describes the finished load. A position
// that then reads back as something other than what was written did not
// load: a failed FX leaves its position empty and a failed synth leaves
// synth_module "". That used to spin the whole 20 s cap.
//
// writeAnswered=false (the write timed out on our side) means the write may
// still be queued, or may never have arrived, so a mismatch proves nothing
// and only landing or the cap ends the wait.
//
// The synth also has `load_error` (chain_host.c), which can be set with the
// module still named -- a module whose UI JSON overflows the param channel
// loads without an instance -- so a landed synth is checked once.
func waitSlotLoad(ctx context.Context, get paramGetter, slot uint8, comp, want string,
	writeAnswered bool, tm loadWaitTiming) loadResult {
	deadline := time.Now().Add(tm.Cap)
	if !sleepCtx(ctx, tm.Settle) {
		return loadResult{Outcome: loadCancelled}
	}
	synthErr := func() string {
		if comp != "synth" {
			return ""
		}
		e, err := get(slot, "load_error")
		if err != nil {
			return ""
		}
		return strings.TrimSpace(e)
	}
	var mismatchSince time.Time
	for {
		got, err := get(slot, comp+"_module")
		if err == nil {
			got = strings.TrimSpace(got)
			if got == want {
				if want != "" {
					if e := synthErr(); e != "" {
						return loadResult{Outcome: loadFailed, Detail: e}
					}
				}
				return loadResult{Outcome: loadLanded}
			}
			if writeAnswered {
				if mismatchSince.IsZero() {
					mismatchSince = time.Now()
				}
				if time.Since(mismatchSince) >= tm.FailGrace {
					return loadResult{Outcome: loadFailed, Detail: synthErr()}
				}
			}
		}
		if !time.Now().Before(deadline) {
			return loadResult{Outcome: loadTimedOut}
		}
		if !sleepCtx(ctx, tm.Poll) {
			return loadResult{Outcome: loadCancelled}
		}
	}
}

// waitMasterFxLoad waits for a "master_fx:fxN:module" write to settle.
//
// Unlike a slot, a Master FX write is ACCEPTED, not loaded: the shim answers
// at once and loads on its worker. `<comp>:is_loading` is "1" until it is
// done and `<comp>:load_error` is "1" if it failed -- which is the only way to
// tell a failure from a load still arriving, since a failed position is empty
// exactly as an arriving one is. Re-describing at a fixed 400 ms handed a
// large bundle's subscribers an empty hierarchy and chain_params.
func waitMasterFxLoad(ctx context.Context, get paramGetter, comp, want string,
	writeAnswered bool, tm loadWaitTiming) loadResult {
	deadline := time.Now().Add(tm.Cap)
	if !sleepCtx(ctx, tm.Settle) {
		return loadResult{Outcome: loadCancelled}
	}
	wantID := masterFxModuleID(want)
	for {
		loading, err := get(0, comp+":is_loading")
		legacy := err != nil && paramAnswered(err) // a shim with no load state
		if (err == nil && strings.TrimSpace(loading) != "1") || legacy {
			failed := false
			if !legacy {
				if e, err := get(0, comp+":load_error"); err == nil && strings.TrimSpace(e) == "1" {
					failed = true
				}
			}
			// A failure is believed only for OUR write: a load_error left from
			// an earlier request reads the same until this one is taken.
			if failed && writeAnswered {
				return loadResult{Outcome: loadFailed}
			}
			if got, err := get(0, comp+":module"); err == nil {
				if masterFxModuleID(strings.TrimSpace(got)) == wantID {
					return loadResult{Outcome: loadLanded}
				}
				if writeAnswered && !legacy {
					// Settled, not failed, and not ours: a later write replaced it.
					return loadResult{Outcome: loadLanded}
				}
			}
		}
		if !time.Now().Before(deadline) {
			return loadResult{Outcome: loadTimedOut}
		}
		if !sleepCtx(ctx, tm.Poll) {
			return loadResult{Outcome: loadCancelled}
		}
	}
}

// loadFailureMessage is what the page shows when a load does not land.
func loadFailureMessage(where, moduleID string, r loadResult) string {
	if moduleID == "" {
		moduleID = "the module"
	}
	switch r.Outcome {
	case loadFailed:
		msg := fmt.Sprintf("%s did not load into %s", moduleID, where)
		if r.Detail != "" {
			msg += ": " + r.Detail
		}
		return msg
	case loadTimedOut:
		return fmt.Sprintf("%s had not loaded into %s after %s", moduleID, where, moduleSwapWait)
	}
	return ""
}

// slotPositionLabel is "Slot 2 FX 3" for messages.
func slotPositionLabel(slot uint8, comp string) string {
	label := comp
	switch {
	case comp == "synth":
		label = "Synth"
	case strings.HasPrefix(comp, "midi_fx"):
		label = "MIDI FX " + strings.TrimPrefix(comp, "midi_fx")
	case strings.HasPrefix(comp, "fx"):
		label = "FX " + strings.TrimPrefix(comp, "fx")
	}
	return fmt.Sprintf("Slot %d %s", slot+1, label)
}
