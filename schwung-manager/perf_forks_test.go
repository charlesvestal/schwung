package main

import (
	"testing"
	"time"
)

// deviceTree is the real device's process tree under MoveOriginal, plus the
// JP-8000 shape: a per-frame fork of the SPI callback that forks again once
// per pipeline stage, each pinned to a core. See perf_forks.go.
func deviceTree() []ProcStat {
	return []ProcStat{
		{PID: 785, Comm: "init", PPid: 1},
		{PID: 917, Comm: "MoveOriginal", PPid: 785},
		{PID: 927, Comm: "display-server", PPid: 917},
		{PID: 934, Comm: "schwung-manager", PPid: 917},
		{PID: 973, Comm: "link-subscriber", PPid: 917},
		{PID: 981, Comm: "shadow_ui", PPid: 917},
		{PID: 975, Comm: "Audio Main/SPI", PPid: 917}, // module fork
		// JP-8000: child of the callback, which forks per pipeline stage.
		{PID: 1200, Comm: "MoveOriginal", PPid: 917, CPU: 0},
		{PID: 1201, Comm: "MoveOriginal", PPid: 1200, CPU: 1},
		{PID: 1202, Comm: "MoveOriginal", PPid: 1200, CPU: 2},
		// A helper's own child must not be counted as a module fork.
		{PID: 1300, Comm: "sh", PPid: 934},
	}
}

func TestForksFindsGrandchildren(t *testing.T) {
	got := findForkedChildren(deviceTree(), 917)
	want := map[int]bool{975: true, 1200: true, 1201: true, 1202: true}
	seen := map[int]bool{}
	for _, p := range got {
		seen[p.PID] = true
	}
	for pid := range want {
		if !seen[pid] {
			t.Errorf("pid %d missing from findForkedChildren result — JP-8000's "+
				"stage workers (1201, 1202) are GRANDCHILDREN of MoveOriginal, "+
				"not children; a one-level scan misses the actual DSP work",
				pid)
		}
	}
}

func TestForksExcludesHelpersAndTheirChildren(t *testing.T) {
	got := findForkedChildren(deviceTree(), 917)
	excluded := map[int]bool{
		917:  true, // MoveOriginal itself
		927:  true, // display-server
		934:  true, // schwung-manager
		973:  true, // link-subscriber
		981:  true, // shadow_ui
		1300: true, // schwung-manager's own child
	}
	for _, p := range got {
		if excluded[p.PID] {
			t.Errorf("pid %d (%s) present in findForkedChildren result — it is a "+
				"shim helper or descends from one, not a module fork", p.PID, p.Comm)
		}
	}
}

func TestForksDoesNotFilterByName(t *testing.T) {
	got := findForkedChildren(deviceTree(), 917)
	seen := map[int]bool{}
	for _, p := range got {
		seen[p.PID] = true
	}
	if !seen[1200] {
		t.Errorf("pid 1200, named %q, missing — a fork inherits comm, so a name "+
			"filter deletes exactly the processes that matter", "MoveOriginal")
	}
	if !seen[975] {
		t.Errorf("pid 975, named %q, missing — Move also runs six real THREADS "+
			"by this name; a name filter cannot tell this forked PROCESS from them",
			"Audio Main/SPI")
	}
}

func TestForksTerminatesOnACycle(t *testing.T) {
	tree := []ProcStat{
		{PID: 917, Comm: "MoveOriginal", PPid: 785},
		{PID: 10, Comm: "a", PPid: 11},
		{PID: 11, Comm: "b", PPid: 10},
		{PID: 12, Comm: "c", PPid: 12}, // self-parent
	}
	// Root the walk at 10 so 10/11/12 are reachable and would loop if the
	// walk doesn't guard against cycles.
	tree[0].PPid = 785

	done := make(chan struct{})
	go func() {
		findForkedChildren(tree, 10)
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("findForkedChildren did not return within 1s — a cycle or " +
			"self-parent in the input must not hang the walk")
	}
}

func TestForksFindMoveOriginalPrefersTheTopLevel(t *testing.T) {
	got := findMoveOriginal(deviceTree())
	if got != 917 {
		t.Errorf("findMoveOriginal = %d, want 917 — a forked child inherits the "+
			"name too (pid 1200 is also \"MoveOriginal\"), so matching by name "+
			"alone can root the walk in the wrong place; the top-level process "+
			"is the lowest pid", got)
	}
}
