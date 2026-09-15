package main

import (
	"reflect"
	"testing"
)

// viz.extra_keys name values that own no cell of their own. The pump reads
// them by name, so the list must be: every distinct key, in declaration
// order, minus any that is already a real param (the main loop fetches those).
func TestExtraKeysOf(t *testing.T) {
	viz := func(keys ...string) (v struct {
		ExtraKeys []string `json:"extra_keys"`
	}) {
		v.ExtraKeys = keys
		return
	}
	params := []chainParam{
		{Key: "grid", Viz: viz("prog", "status")},
		{Key: "sel", Viz: viz("prog", "patterns", "")},
		{Key: "status"}, // a real param: must not be listed as an extra
		{Key: "rate", Viz: viz("status", "bypassed")},
	}
	got := extraKeysOf(params)
	want := []string{"prog", "patterns", "bypassed"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("extraKeysOf = %v, want %v", got, want)
	}
	if got := extraKeysOf([]chainParam{{Key: "a"}, {Key: "b"}}); len(got) != 0 {
		t.Fatalf("a component declaring no extras must list none, got %v", got)
	}
}
