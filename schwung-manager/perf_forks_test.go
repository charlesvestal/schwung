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

// kidsFor builds a run of forked children at the given CPU percentages. They
// all wear "MoveOriginal" because that is what a fork inherits — the naming
// this file does can never come from the process itself.
func kidsFor(cpu ...float64) []ForkedProc {
	out := make([]ForkedProc, 0, len(cpu))
	for i, c := range cpu {
		out = append(out, ForkedProc{PID: 1200 + i, Comm: "MoveOriginal",
			Core: i, Percent: c})
	}
	return out
}

func TestAttributeDeclaredWins(t *testing.T) {
	got := attributeForks(kidsFor(90, 80, 70), []LoadedModule{
		{ID: "jp8000", Forks: true},
		{ID: "9w9"},
	})
	if len(got) != 1 {
		t.Fatalf("attributeForks returned %d groups, want 1 — one module declares "+
			"that it forks, so there is nothing to be ambiguous about", len(got))
	}
	g := got[0]
	if g.Module != "jp8000" {
		t.Errorf("Module = %q, want \"jp8000\" — a declared capability is the "+
			"authoritative answer and must beat any inference", g.Module)
	}
	if g.Inferred {
		t.Error("Inferred is true for a DECLARED module — the page would tell the " +
			"user to doubt the one attribution we are actually sure of")
	}
	if g.TotalPercent < 239 || g.TotalPercent > 241 {
		t.Errorf("TotalPercent = %v, want ~240 — the whole point of this page is "+
			"the real cost of work that left the SPI callback", g.TotalPercent)
	}
	if len(g.Procs) != 3 {
		t.Errorf("len(Procs) = %d, want 3 — the per-pid rows are how a user sees "+
			"the work is spread across cores", len(g.Procs))
	}
}

func TestAttributeInferredIsMarked(t *testing.T) {
	got := attributeForks(kidsFor(50), []LoadedModule{{ID: "jp8000"}})
	if len(got) != 1 {
		t.Fatalf("attributeForks returned %d groups, want 1", len(got))
	}
	if got[0].Module != "jp8000" {
		t.Errorf("Module = %q, want \"jp8000\" — with exactly one synth loaded "+
			"and nothing declared, that synth is the only candidate", got[0].Module)
	}
	if !got[0].Inferred {
		t.Error("Inferred is false on a GUESS — nothing declared forks_processes, " +
			"so this name is inference; an unlabelled guess on a CPU page sends " +
			"the user optimising a module that may not be costing anything")
	}
}

func TestAttributeAmbiguousIsUnattributed(t *testing.T) {
	got := attributeForks(kidsFor(50), []LoadedModule{{ID: "jp8000"}, {ID: "9w9"}})
	if len(got) != 1 {
		t.Fatalf("attributeForks returned %d groups, want 1 — the cost is real "+
			"and must be shown even when we cannot say whose it is", len(got))
	}
	if got[0].Module != "" {
		t.Errorf("Module = %q, want \"\" — two loaded synths and no declaration is "+
			"not a guess worth making; naming one of them at random is worse than "+
			"naming none", got[0].Module)
	}
	if got[0].TotalPercent < 49 || got[0].TotalPercent > 51 {
		t.Errorf("TotalPercent = %v, want ~50 — nothing is hidden for want of a "+
			"name", got[0].TotalPercent)
	}
}

func TestAttributeNoModulesIsUnattributed(t *testing.T) {
	got := attributeForks(kidsFor(10, 20), nil)
	if len(got) != 1 {
		t.Fatalf("attributeForks returned %d groups, want 1 — forked processes "+
			"exist whether or not we could read the rig", len(got))
	}
	if got[0].Module != "" {
		t.Errorf("Module = %q, want \"\" — with no candidates at all there is "+
			"nobody to attribute to", got[0].Module)
	}
	if len(got[0].Procs) != 2 {
		t.Errorf("len(Procs) = %d, want 2 — the pids are the finding", len(got[0].Procs))
	}
}

func TestAttributeNoChildrenIsNoGroups(t *testing.T) {
	got := attributeForks(nil, []LoadedModule{{ID: "jp8000", Forks: true}})
	if len(got) != 0 {
		t.Errorf("attributeForks returned %d groups for zero children, want 0 — an "+
			"empty group renders as a panel claiming a module forks and costs "+
			"nothing, which is a measurement we never made", len(got))
	}
}

func TestAttributeTwoDeclarersDoNotDoubleCount(t *testing.T) {
	got := attributeForks(kidsFor(30, 40), []LoadedModule{
		{ID: "jp8000", Forks: true},
		{ID: "stems", Forks: true},
	})
	if len(got) != 2 {
		t.Fatalf("attributeForks returned %d groups, want 2 — one per declarer",
			len(got))
	}
	total := 0
	for _, g := range got {
		total += len(g.Procs)
	}
	if total != 2 {
		t.Errorf("groups hold %d procs in total, want 2 — a pid counted twice "+
			"doubles the reported CPU of a device that is not doing that work",
			total)
	}
}

// The fork panel must take its numbers from the MEASUREMENT, not the displayed
// rows. Rows drops everything under the 0.5% floor, and a forked child rarely
// carries an always-listed name - the one seen on the device reported as
// "Audio Main/SPI" - so sourcing from Rows renders 0% for a value that was
// computed and discarded.
func TestForkedProcsUseTheUnfilteredMeasurement(t *testing.T) {
	kids := []ProcStat{
		{PID: 1200, Comm: "Audio Main/SPI", CPU: 1},
		{PID: 1201, Comm: "MoveOriginal", CPU: 2},
	}
	// What AllPercent holds: every measured pid, floor or not.
	all := map[int]float64{1200: 0.2, 1201: 93.0}

	got := forkedProcsWithCPU(kids, all)
	if len(got) != 2 {
		t.Fatalf("got %d procs, want 2", len(got))
	}
	if got[0].Percent != 0.2 {
		t.Errorf("pid 1200 = %.2f%%, want 0.2%% - a child under the display "+
			"floor must still report its real cost, not 0", got[0].Percent)
	}
	if got[1].Percent != 93.0 {
		t.Errorf("pid 1201 = %.1f%%, want 93", got[1].Percent)
	}
}
