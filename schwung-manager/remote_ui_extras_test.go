package main

import (
	"errors"
	"os"
	"reflect"
	"regexp"
	"strconv"
	"testing"
	"time"
)

// decl parses a chain_params document the way the manager reads one off the
// wire, so the tests exercise the JSON tags rather than hand-built structs.
func decl(t *testing.T, raw string) []chainParam {
	t.Helper()
	params, ok := parseChainParams(raw, nil)
	if !ok {
		t.Fatalf("chain_params did not parse: %s", raw)
	}
	return params
}

// Extra keys name values that own no cell of their own. The pump reads them by
// name, so the list must be: every distinct key, in declaration order, minus
// any that is already a real param (the main loop fetches those).
func TestExtraKeysOf(t *testing.T) {
	params := decl(t, `[
		{"key":"grid","viz":{"kind":"custom:roll","extra_keys":["prog","status"]}},
		{"key":"sel","viz":{"kind":"custom:roll","extra_keys":["prog","patterns",""]}},
		{"key":"status"},
		{"key":"rate","viz":{"kind":"custom:roll","extra_keys":["status","bypassed"]}}
	]`)
	got := extraKeysOf(params)
	want := []string{"prog", "patterns", "bypassed"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("extraKeysOf = %v, want %v", got, want)
	}
	if got := extraKeysOf(decl(t, `[{"key":"a"},{"key":"b"}]`)); len(got) != 0 {
		t.Fatalf("a component declaring no extras must list none, got %v", got)
	}
}

// The device reads extras from THREE spellings, and a browser that read fewer
// was blind to a module the device drew correctly.
func TestExtraKeysOfReadsEveryDeviceSpelling(t *testing.T) {
	params := decl(t, `[
		{"key":"a","viz":{"kind":"custom:x","extraKeys":["camel"]}},
		{"key":"face","type":"canvas","as_page":true,"extra_keys":["canvas_snake"]},
		{"key":"face2","type":"canvas","as_page":true,"extraKeys":["canvas_camel"]},
		{"key":"b","viz":{"kind":"custom:x","extra_keys":["snake"],"extraKeys":["ignored"]}}
	]`)
	got := extraKeysOf(params)
	want := []string{"camel", "canvas_snake", "canvas_camel", "snake"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("extraKeysOf = %v, want %v (snake_case wins when both are present, as on the device)", got, want)
	}
}

// Per declaration, the first four distinct names — the device's cap, so an
// over-declared module costs no more here than on the hardware.
func TestExtraKeysOfCapsLikeTheDevice(t *testing.T) {
	params := decl(t, `[{"key":"a","viz":{"extra_keys":["k1","k1","k2","k3","k4","k5"]}}]`)
	got := extraKeysOf(params)
	want := []string{"k1", "k2", "k3", "k4"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("extraKeysOf = %v, want %v", got, want)
	}

	// And a component-wide ceiling, because the browser reads every page's
	// extras at once and the device only reads the page on screen.
	var many []chainParam
	for i := 0; i < 10; i++ {
		var p chainParam
		p.Key = "p" + strconv.Itoa(i)
		for j := 0; j < 4; j++ {
			p.Viz.ExtraKeys = append(p.Viz.ExtraKeys, "x"+strconv.Itoa(i)+"_"+strconv.Itoa(j))
		}
		many = append(many, p)
	}
	if got := extraKeysOf(many); len(got) != maxRemoteExtraKeys {
		t.Fatalf("component-wide extras = %d, want the %d ceiling", len(got), maxRemoteExtraKeys)
	}
}

// The cap is the device's constant, not a copy of its value that can drift.
func TestMaxDeclaredExtraKeysMatchesViz(t *testing.T) {
	src, err := os.ReadFile("../src/shared/param_pages/viz.mjs")
	if err != nil {
		t.Skipf("viz.mjs not reachable from here: %v", err)
	}
	m := regexp.MustCompile(`export const MAX_DECLARED_EXTRA_KEYS = (\d+);`).FindSubmatch(src)
	if m == nil {
		t.Fatal("MAX_DECLARED_EXTRA_KEYS not found in viz.mjs")
	}
	if n, _ := strconv.Atoi(string(m[1])); n != maxDeclaredExtraKeys {
		t.Fatalf("maxDeclaredExtraKeys = %d, viz.mjs says %d", maxDeclaredExtraKeys, n)
	}
}

// Only a completed, parsed read is news about what a module declares. The
// other answers must not be cached as "declares nothing" — that latched the
// pump off for the session whenever the first read came in while the module
// was still loading.
func TestParseChainParamsDefinitive(t *testing.T) {
	cases := []struct {
		name string
		raw  string
		err  error
		want bool
	}{
		{"a parsed declaration", `[{"key":"a"}]`, nil, true},
		{"a parsed EMPTY declaration is still an answer", `[]`, nil, true},
		{"timed out", "", errors.New("timeout"), false},
		{"served empty (module still loading)", "", nil, false},
		{"truncated at the buffer cap", `[{"key":"a"`, nil, false},
	}
	for _, c := range cases {
		if _, ok := parseChainParams(c.raw, c.err); ok != c.want {
			t.Errorf("%s: definitive = %v, want %v", c.name, ok, c.want)
		}
	}
}

func TestExtrasDeclFreshness(t *testing.T) {
	now := time.Now()
	old := now.Add(-extrasRetryInterval - time.Millisecond)
	if !(extrasDecl{definitive: true, at: old}).fresh(now) {
		t.Error("a definitive declaration must not expire (only a module change invalidates it)")
	}
	if (extrasDecl{definitive: false, at: old}).fresh(now) {
		t.Error("a non-definitive declaration must be re-asked after extrasRetryInterval")
	}
	if !(extrasDecl{definitive: false, at: now}).fresh(now) {
		t.Error("a non-definitive declaration must still throttle the re-ask")
	}
}

// Two overlapping pushes of one component answer in channel order, and the
// browser then receives an older value after a newer one. The gate refuses a
// second START until the first is done, and throttles restarts.
func TestExtrasGate(t *testing.T) {
	ru := &RemoteUI{}
	if !ru.extrasGate(0, "synth", 0) {
		t.Fatal("first push must start")
	}
	if ru.extrasGate(0, "synth", 0) {
		t.Fatal("a second push must not start while the first is in flight")
	}
	if !ru.extrasGate(0, "fx1", 0) || !ru.extrasGate(1, "synth", 0) {
		t.Fatal("the gate is per slot AND component")
	}
	ru.extrasDone(0, "synth")
	if ru.extrasGate(0, "synth", time.Hour) {
		t.Fatal("a finished push must still honour the throttle")
	}
	if !ru.extrasGate(0, "synth", 0) {
		t.Fatal("a finished push outside the throttle must allow the next")
	}
}

// A module swap drops the declaration AND the last-sent values, which belonged
// to the previous module.
func TestInvalidateExtraKeysDropsValues(t *testing.T) {
	ru := &RemoteUI{
		extrasKeys:  map[string]extrasDecl{"0|synth": {keys: []string{"prog"}, definitive: true}},
		extrasValue: map[string]string{"0|synth:prog": "a", "0|fx1:prog": "b", "1|synth:prog": "c"},
	}
	ru.invalidateExtraKeys(0, "synth")
	if _, ok := ru.extrasKeys["0|synth"]; ok {
		t.Error("declaration survived invalidation")
	}
	if _, ok := ru.extrasValue["0|synth:prog"]; ok {
		t.Error("last-sent value survived invalidation")
	}
	if len(ru.extrasValue) != 2 {
		t.Errorf("invalidation touched other components: %v", ru.extrasValue)
	}
}
