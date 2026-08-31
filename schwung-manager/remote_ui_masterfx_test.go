package main

import "testing"

// A Master FX position is loaded by dsp PATH (shadow_master_fx_slot_load takes
// one), so reading its module key back returns that path — while a chain slot's
// equivalent holds a bare id. The same module is therefore "4k-eq" on a track
// and a .so path on the master bus, which is why findModuleWebUI found no panel
// there and why the tab labelled the position with a path (#354).
func TestMasterFxModuleID(t *testing.T) {
	cases := []struct{ in, want string }{
		{"/data/UserData/schwung/modules/audio_fx/4k-eq/4k-eq.so", "4k-eq"},
		{"/data/UserData/schwung/modules/audio_fx/tape-echo2/tape-echo2.so", "tape-echo2"},
		// Already an id (a shim that stores one): passed through untouched.
		{"4k-eq", "4k-eq"},
		// Nothing loaded.
		{"", ""},
	}
	for _, c := range cases {
		if got := masterFxModuleID(c.in); got != c.want {
			t.Errorf("masterFxModuleID(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}
