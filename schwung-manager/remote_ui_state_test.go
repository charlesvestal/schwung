package main

import "testing"

// The "state" fast path once accepted anything that parsed as a JSON object.
// A module's save blob is a JSON object too, so an opaque {"s": "v6|..."}
// "succeeded", pushed one useless key, and skipped the sweep that fetches the
// real params. The gate is: does the snapshot contain a key this component
// actually declares.
func TestStateCoversParams(t *testing.T) {
	declared := []chainParam{{Key: "sel"}, {Key: "rate"}, {Key: ""}}

	cases := []struct {
		name   string
		values map[string]string
		params []chainParam
		want   bool
	}{
		{"opaque save blob is not a param map",
			map[string]string{"midi_fx1:s": "v6|9|2|"}, declared, false},
		{"one declared key is enough",
			map[string]string{"midi_fx1:rate": "3", "midi_fx1:s": "x"}, declared, true},
		{"the key must be prefixed with THIS component",
			map[string]string{"synth:sel": "1"}, declared, false},
		{"empty snapshot covers nothing",
			map[string]string{}, declared, false},
		{"an undeclarable component keeps the fast path",
			map[string]string{"midi_fx1:s": "x"}, nil, true},
	}
	for _, c := range cases {
		if got := stateCoversParams(c.values, "midi_fx1", c.params); got != c.want {
			t.Errorf("%s: got %v, want %v", c.name, got, c.want)
		}
	}
}
