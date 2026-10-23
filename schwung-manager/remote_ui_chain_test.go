// What the Remote UI learns about a chain, how many reads that costs, and how
// a module load ends. The fake answers through the same paramGetter shape the
// production code reads with (module_webui_links_test.go's fakeParams).

package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// The Go caps mirror three C constants. A copy that is not pinned drifts --
// tests/host/test_master_fx_slots_js.sh exists for the same reason on the JS
// side -- so read the headers and compare.
func TestChainCapsMatchTheCHeaders(t *testing.T) {
	cases := []struct {
		header, define string
		goValue        int
	}{
		{"../src/host/shadow_chain_mgmt.h", "MASTER_FX_SLOTS", masterFxSlotCount},
		{"../src/modules/chain/dsp/chain_internal.h", "MAX_AUDIO_FX", chainMaxAudioFx},
		{"../src/modules/chain/dsp/chain_internal.h", "MAX_MIDI_FX", chainMaxMidiFx},
	}
	for _, c := range cases {
		b, err := os.ReadFile(filepath.FromSlash(c.header))
		if err != nil {
			t.Fatalf("read %s: %v", c.header, err)
		}
		re := regexp.MustCompile(`(?m)^#define\s+` + c.define + `\s+(\d+)\b`)
		m := re.FindSubmatch(b)
		if m == nil {
			t.Fatalf("%s: no `#define %s <n>` found", c.header, c.define)
		}
		n, _ := strconv.Atoi(string(m[1]))
		if n != c.goValue {
			t.Errorf("%s is %d in %s but %d in remote_ui_chain.go", c.define, n, c.header, c.goValue)
		}
	}
}

// scriptedParams is fakeParams with time: a value can be a function of how
// many times its key has been read, and any key can be made to time out.
type scriptedParams struct {
	mu    sync.Mutex
	vals  map[string]func(n int) (string, error)
	reads []string
	count map[string]int
}

func newScripted() *scriptedParams {
	return &scriptedParams{vals: map[string]func(int) (string, error){}, count: map[string]int{}}
}

func (s *scriptedParams) set(k, v string) {
	s.vals[k] = func(int) (string, error) { return v, nil }
}

func (s *scriptedParams) timeout(k string) {
	s.vals[k] = func(int) (string, error) {
		return "", errors.New("param response timeout (reqID=7)")
	}
}

func (s *scriptedParams) get(slot uint8, key string) (string, error) {
	s.mu.Lock()
	k := fmt.Sprintf("%d/%s", slot, key)
	s.reads = append(s.reads, k)
	n := s.count[k]
	s.count[k] = n + 1
	f, ok := s.vals[k]
	s.mu.Unlock()
	if !ok {
		return "", errors.New("param get error: not served")
	}
	return f(n)
}

func (s *scriptedParams) readKeys() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.reads...)
}

// --- Discovery -----------------------------------------------------------

// A third audio FX was invisible to the Remote UI while it ran and made
// sound. The chain's own count names it, and only the positions the count
// names are read: no blind fx4..fx8.
func TestSlotChainFindsEveryListedPositionAndNoOthers(t *testing.T) {
	f := newScripted()
	f.set("1/synth_module", "obxd")
	f.set("1/midi_fx_count", "0")
	f.set("1/fx_count", "3")
	f.set("1/fx1_module", "freeverb")
	f.set("1/fx2_module", "")
	f.set("1/fx3_module", "tape-echo2")

	ch := readSlotChain(f.get, 1)
	if !ch.SynthKnown || !ch.MidiFXKnown || !ch.FXKnown {
		t.Fatalf("every read answered, got %+v", ch)
	}
	if got, want := strings.Join(ch.FX, ","), "freeverb,,tape-echo2"; got != want {
		t.Errorf("FX = %q, want %q (a hole in FRONT is kept: position i IS fx(i+1))", got, want)
	}
	if got, want := strings.Join(ch.loaded(), ","), "synth,fx1,fx3"; got != want {
		t.Errorf("loaded = %q, want %q", got, want)
	}
	wantReads := []string{"1/synth_module", "1/midi_fx_count", "1/fx_count",
		"1/fx1_module", "1/fx2_module", "1/fx3_module"}
	if got := f.readKeys(); strings.Join(got, " ") != strings.Join(wantReads, " ") {
		t.Errorf("reads = %v, want %v", got, wantReads)
	}
}

// An empty slot costs THREE reads, against four fixed reads before.
func TestEmptySlotIsThreeReads(t *testing.T) {
	f := newScripted()
	f.set("0/synth_module", "")
	f.set("0/midi_fx_count", "0")
	f.set("0/fx_count", "0")
	ch := readSlotChain(f.get, 0)
	if len(ch.loaded()) != 0 || len(ch.unknownSections()) != 0 {
		t.Fatalf("got %+v, want a known empty slot", ch)
	}
	if n := len(f.readKeys()); n != 3 {
		t.Errorf("an empty slot made %d reads, want 3: %v", n, f.readKeys())
	}
}

// A count past the cap is clamped -- the chain cannot hold more, and a garbage
// count must not turn into a hundred reads.
func TestSlotChainCountIsClampedToTheCap(t *testing.T) {
	f := newScripted()
	f.set("0/synth_module", "")
	f.set("0/midi_fx_count", "0")
	f.set("0/fx_count", "40")
	for i := 1; i <= 40; i++ {
		f.set(fmt.Sprintf("0/fx%d_module", i), "x")
	}
	ch := readSlotChain(f.get, 0)
	if len(ch.FX) != chainMaxAudioFx {
		t.Errorf("len(FX) = %d, want the cap %d", len(ch.FX), chainMaxAudioFx)
	}
}

// The eighth position of each kind is a real position: the '+' after seven
// FX writes fx8.
func TestIsChainComponentCoversEveryPositionUpToTheCaps(t *testing.T) {
	yes := []string{"synth", "fx1", "fx8", "midi_fx1", "midi_fx8"}
	no := []string{"fx0", "fx9", "fx01", "midi_fx9", "master_fx:fx1", "fx", "bus1", "lfo1"}
	for _, c := range yes {
		if !isChainComponent(c) {
			t.Errorf("isChainComponent(%q) = false", c)
		}
	}
	for _, c := range no {
		if isChainComponent(c) {
			t.Errorf("isChainComponent(%q) = true", c)
		}
	}
}

// --- null vs "" ----------------------------------------------------------

// A timed-out read is not an empty position. It used to be: the module id was
// read as `modID, _ :=`, so a timeout became "", and the page reset the
// component -- wiping its cached hierarchy and values on one bad read.
func TestTimedOutSectionIsUnknownNotEmpty(t *testing.T) {
	f := newScripted()
	f.set("2/synth_module", "obxd")
	f.set("2/midi_fx_count", "1")
	f.set("2/midi_fx1_module", "arp")
	f.timeout("2/fx_count")

	ch := readSlotChain(f.get, 2)
	if !ch.SynthKnown || !ch.MidiFXKnown || ch.FXKnown {
		t.Fatalf("got %+v, want synth + midi_fx known, fx unknown", ch)
	}
	if mod, known := ch.moduleOf("fx1"); known {
		t.Errorf("moduleOf(fx1) = (%q, known); a section that was not read must not answer", mod)
	}

	info := slotInfoFrom(2, ch)
	b, _ := json.Marshal(info)
	var wire map[string]any
	_ = json.Unmarshal(b, &wire)
	if wire["fx"] != nil {
		t.Errorf("wire fx = %v, want null for an unread section", wire["fx"])
	}
	if u, _ := wire["unknown"].([]any); len(u) != 1 || u[0] != "fx" {
		t.Errorf("wire unknown = %v, want [fx]", wire["unknown"])
	}
	if mf, _ := wire["midi_fx"].([]any); len(mf) != 1 || mf[0] != "arp" {
		t.Errorf("wire midi_fx = %v, want [arp]", wire["midi_fx"])
	}
}

// The first read that does not complete ends the walk: everything behind it
// would wait out its own timeout and still be unknown.
func TestSynthTimeoutStopsTheWalk(t *testing.T) {
	f := newScripted()
	f.timeout("0/synth_module")
	f.set("0/midi_fx_count", "0")
	f.set("0/fx_count", "0")
	ch := readSlotChain(f.get, 0)
	if got := strings.Join(ch.unknownSections(), ","); got != "synth,midi_fx,fx" {
		t.Errorf("unknown = %q, want all three", got)
	}
	if n := len(f.readKeys()); n != 1 {
		t.Errorf("made %d reads after a timeout, want 1: %v", n, f.readKeys())
	}
}

// A known-empty section is an empty LIST on the wire, never null, so the page
// can tell "nothing there" from "did not read".
func TestKnownEmptySectionIsAnEmptyListOnTheWire(t *testing.T) {
	f := newScripted()
	f.set("0/synth_module", "")
	f.set("0/midi_fx_count", "0")
	f.set("0/fx_count", "1")
	f.set("0/fx1_module", "") // a trailing empty is dropped
	b, _ := json.Marshal(slotInfoFrom(0, readSlotChain(f.get, 0)))
	s := string(b)
	for _, want := range []string{`"fx":[]`, `"midi_fx":[]`, `"fx_cap":8`, `"midi_fx_cap":8`} {
		if !strings.Contains(s, want) {
			t.Errorf("slot_info %s lacks %s", s, want)
		}
	}
	if strings.Contains(s, `"unknown"`) {
		t.Errorf("slot_info %s reports unknown for a fully read slot", s)
	}
}

// --- Master FX ---------------------------------------------------------------

func masterListing(ids map[int]string) string {
	parts := make([]string, masterFxSlotCount)
	for i := range parts {
		id := ids[i]
		path := ""
		if id != "" {
			path = "/data/UserData/schwung/modules/audio_fx/" + id + "/" + id + ".so"
		}
		parts[i] = fmt.Sprintf(`{"id":%q,"path":%q}`, id, path)
	}
	return "[" + strings.Join(parts, ",") + "]"
}

// The master bus is ONE read whatever it holds, and positions 5-8 exist.
func TestMasterFxChainIsOneReadAndSeesAllEight(t *testing.T) {
	f := newScripted()
	f.set("0/master_fx:modules", masterListing(map[int]string{0: "4k-eq", 6: "cloudseed"}))
	m := readMasterFxChain(f.get)
	if !m.Known || len(m.Modules) != masterFxSlotCount {
		t.Fatalf("got %+v", m)
	}
	if m.Modules[6] != "cloudseed" || m.Modules[0] != "4k-eq" {
		t.Errorf("Modules = %v", m.Modules)
	}
	if got := strings.Join(m.loaded(), ","); got != "master_fx:fx1,master_fx:fx7" {
		t.Errorf("loaded = %q", got)
	}
	if n := len(f.readKeys()); n != 1 {
		t.Errorf("made %d reads, want 1: %v", n, f.readKeys())
	}
	info := masterFxInfoFrom(m)
	if info.Cap != masterFxSlotCount || len(info.FX) != masterFxSlotCount || info.FX1 != "4k-eq" || info.Unknown {
		t.Errorf("info = %+v", info)
	}
}

// A listing that timed out says so, and carries no positions to believe.
func TestMasterFxTimeoutIsUnknown(t *testing.T) {
	f := newScripted()
	f.timeout("0/master_fx:modules")
	m := readMasterFxChain(f.get)
	if m.Known {
		t.Fatalf("got %+v, want unknown", m)
	}
	b, _ := json.Marshal(masterFxInfoFrom(m))
	if s := string(b); !strings.Contains(s, `"unknown":true`) || !strings.Contains(s, `"fx":null`) {
		t.Errorf("master_fx_info = %s, want unknown with no positions", s)
	}
	if n := len(f.readKeys()); n != 1 {
		t.Errorf("a timed-out listing must not fall back to per-position reads: %v", f.readKeys())
	}
}

// A shim that predates the listing ANSWERS with an error; only then are the
// positions read one at a time.
func TestMasterFxFallsBackOnlyForAnOldShim(t *testing.T) {
	f := newScripted()
	f.set("0/master_fx:fx2:module", "/data/UserData/schwung/modules/audio_fx/echo/echo.so")
	m := readMasterFxChain(f.get)
	if !m.Known || m.Modules[1] != "echo" {
		t.Fatalf("got %+v", m)
	}
	if n := len(f.readKeys()); n != 1+masterFxSlotCount {
		t.Errorf("made %d reads, want %d", n, 1+masterFxSlotCount)
	}
}

// --- How a load ends -------------------------------------------------------

var fastWait = loadWaitTiming{
	Settle:    0,
	Poll:      time.Millisecond,
	Cap:       300 * time.Millisecond,
	FailGrace: 10 * time.Millisecond,
}

func TestSlotLoadEndsWhenItLands(t *testing.T) {
	f := newScripted()
	f.vals["1/fx3_module"] = func(n int) (string, error) {
		if n < 2 {
			return "", errors.New("param response timeout (reqID=1)") // the loader holds the channel
		}
		return "tape-echo2", nil
	}
	r := waitSlotLoad(context.Background(), f.get, 1, "fx3", "tape-echo2", true, fastWait)
	if r.Outcome != loadLanded {
		t.Fatalf("outcome = %v, want landed", r)
	}
}

// A failed load leaves the position empty. Once the write was answered the
// load is over, so an empty read-back is the failure -- it used to spin the
// whole 20 s cap.
func TestSlotLoadEndsOnFailureNotTheCap(t *testing.T) {
	f := newScripted()
	f.set("0/fx2_module", "")
	start := time.Now()
	r := waitSlotLoad(context.Background(), f.get, 0, "fx2", "broken-fx", true, fastWait)
	if r.Outcome != loadFailed {
		t.Fatalf("outcome = %v, want failed", r)
	}
	if time.Since(start) >= fastWait.Cap {
		t.Errorf("a failure took the whole cap (%s)", time.Since(start))
	}
}

// The synth's load_error is the device's own words, and it can be set with the
// module still named (UI JSON over the channel's limit).
func TestSlotSynthLoadErrorIsReported(t *testing.T) {
	f := newScripted()
	f.set("0/synth_module", "huge")
	f.set("0/load_error", "UI buffer overflow")
	r := waitSlotLoad(context.Background(), f.get, 0, "synth", "huge", true, fastWait)
	if r.Outcome != loadFailed || r.Detail != "UI buffer overflow" {
		t.Fatalf("got %+v, want failed with the device's text", r)
	}
	if msg := loadFailureMessage(slotPositionLabel(0, "synth"), "huge", r); msg != "huge did not load into Slot 1 Synth: UI buffer overflow" {
		t.Errorf("message = %q", msg)
	}
}

// A write that timed out on our side may still be queued, so a mismatch proves
// nothing: only landing or the cap ends the wait. And a read that never
// completes is never a failure.
func TestSlotLoadUnansweredWriteWaitsForTheCap(t *testing.T) {
	f := newScripted()
	f.set("0/fx1_module", "old")
	r := waitSlotLoad(context.Background(), f.get, 0, "fx1", "new", false, fastWait)
	if r.Outcome != loadTimedOut {
		t.Fatalf("outcome = %v, want timed out", r)
	}
	g := newScripted()
	g.timeout("0/fx1_module")
	if r := waitSlotLoad(context.Background(), g.get, 0, "fx1", "new", true, fastWait); r.Outcome != loadTimedOut {
		t.Fatalf("timeouts only: outcome = %v, want timed out (never failed)", r)
	}
}

func TestSlotLoadStopsWhenTheClientGoes(t *testing.T) {
	f := newScripted()
	f.set("0/fx1_module", "old")
	ctx, cancel := context.WithCancel(context.Background())
	go func() { time.Sleep(20 * time.Millisecond); cancel() }()
	tm := fastWait
	tm.Cap = 10 * time.Second
	if r := waitSlotLoad(ctx, f.get, 0, "fx1", "new", false, tm); r.Outcome != loadCancelled {
		t.Fatalf("outcome = %v, want cancelled", r)
	}
}

// Master FX is ACCEPTED, then loaded on the shim's worker: the wait follows
// is_loading rather than sleeping 400 ms and re-describing a bundle that has
// not arrived.
func TestMasterFxLoadWaitsForIsLoading(t *testing.T) {
	f := newScripted()
	f.vals["0/master_fx:fx6:is_loading"] = func(n int) (string, error) {
		if n < 3 {
			return "1", nil
		}
		return "0", nil
	}
	f.set("0/master_fx:fx6:load_error", "0")
	path := "/data/UserData/schwung/modules/audio_fx/cloudseed/cloudseed.so"
	f.set("0/master_fx:fx6:module", path)
	r := waitMasterFxLoad(context.Background(), f.get, "master_fx:fx6", path, true, fastWait)
	if r.Outcome != loadLanded {
		t.Fatalf("outcome = %v, want landed", r)
	}
	if f.count["0/master_fx:fx6:is_loading"] < 4 {
		t.Errorf("stopped waiting while is_loading was still 1 (%d reads)", f.count["0/master_fx:fx6:is_loading"])
	}
}

// load_error is what tells a failed load from one still arriving -- both
// leave the position empty.
func TestMasterFxLoadEndsOnLoadError(t *testing.T) {
	f := newScripted()
	f.set("0/master_fx:fx2:is_loading", "0")
	f.set("0/master_fx:fx2:load_error", "1")
	f.set("0/master_fx:fx2:module", "")
	r := waitMasterFxLoad(context.Background(), f.get, "master_fx:fx2", "/x/broken/broken.so", true, fastWait)
	if r.Outcome != loadFailed {
		t.Fatalf("outcome = %v, want failed", r)
	}
	if msg := loadFailureMessage("Master FX 2", "broken", r); msg != "broken did not load into Master FX 2" {
		t.Errorf("message = %q", msg)
	}
}

// A shim without the load-state keys answers them with an error; the wait
// falls back to reading the module back.
func TestMasterFxLoadOnAShimWithoutLoadState(t *testing.T) {
	f := newScripted()
	path := "/data/UserData/schwung/modules/audio_fx/echo/echo.so"
	f.set("0/master_fx:fx1:module", path)
	if r := waitMasterFxLoad(context.Background(), f.get, "master_fx:fx1", path, true, fastWait); r.Outcome != loadLanded {
		t.Fatalf("outcome = %v, want landed", r)
	}
}

// --- The module page's links walk the same listings ------------------------

func TestWebUILinksFindPositionsPastTheOldFixedSet(t *testing.T) {
	f := &fakeParams{vals: map[string]string{
		"1/fx_count":          "5",
		"1/fx1_module":        "",
		"1/fx2_module":        "",
		"1/fx3_module":        "",
		"1/fx4_module":        "",
		"1/fx5_module":        "echo",
		"0/master_fx:modules": masterListing(map[int]string{7: "echo"}),
	}}
	got := findModuleWebUILinks(f.get, "echo", "audio_fx", testWebUIBase)
	if got.Unknown || len(got.Links) != 2 {
		t.Fatalf("got %+v, want Track 2 FX 5 and Master FX 8", got)
	}
	if got.Links[0].Where != "Track 2 FX 5" || got.Links[1].Where != "Master FX 8" {
		t.Errorf("links = %+v", got.Links)
	}
}
