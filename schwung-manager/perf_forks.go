package main

// Finds processes forked by a module, as opposed to the /proc/<pid>/stat CPU
// timing scanned elsewhere in this package.
//
// The CPU page attributes cost from timing the shim collects INSIDE the SPI
// callback. That is correct for a module whose DSP runs there, and badly
// wrong for one that forks: JP-8000 (the JE-8086 emulation)'s create_instance
// — which IS the SPI callback — calls fork(), and that child forks again once
// per pipeline stage, pinning each to a core. The work happens in a
// two-level process tree on cores 0-2, while the frame-budget row shows only
// the dispatch-and-collect step left in the callback. On the device that read
// ~5% CPU, which was true of the callback and false of the module.
//
// Those children cannot be identified by name: a fork inherits its parent's
// comm, and JP-8000 never calls prctl(PR_SET_NAME). Observed on the real
// device, one of MoveOriginal's forked children reported as "Audio Main/SPI"
// — a forked PROCESS wearing the name of six real THREADS Move itself runs.
// Any name-based filter either matches the module fork (net: false negative,
// it looks like Move's own thread) or matches Move's real threads too (false
// positive). So this file discriminates by the PROCESS TREE instead: the
// shim spawns a small, known set of helpers (shimHelpers below), and
// anything else descending from MoveOriginal was forked by a module.

// shimHelpers are the processes the shim starts. They and everything they in
// turn spawn are not module forks.
//
// Kept short deliberately: anything not on this list shows up as
// module-forked, so a new shim helper announces itself as a wrong row on the
// CPU page rather than silently being swallowed into "module cost" or, worse,
// silently excluding a real module fork because it happened to share a name
// with something on this list.
var shimHelpers = map[string]bool{
	"display-server":  true,
	"schwung-manager": true,
	"link-subscriber": true,
	"shadow_ui":       true,
}

// findForkedChildren walks every descendant of movePID, recursively, and
// returns the ones NOT rooted at a shim helper. The walk is BFS over a
// parent->children index built from `all`; a pid is marked visited before its
// children are enqueued, so a cycle or a self-parent in the input (a
// malformed /proc snapshot, not something Linux can actually produce) cannot
// loop forever.
func findForkedChildren(all []ProcStat, movePID int) []ProcStat {
	children := make(map[int][]ProcStat)
	for _, p := range all {
		if p.PPid == p.PID {
			continue // self-parent: not a real edge, would loop the walk
		}
		children[p.PPid] = append(children[p.PPid], p)
	}

	var out []ProcStat
	visited := map[int]bool{movePID: true}
	queue := append([]ProcStat(nil), children[movePID]...)
	for i := 0; i < len(queue); i++ {
		p := queue[i]
		if visited[p.PID] {
			continue
		}
		visited[p.PID] = true

		if shimHelpers[p.Comm] {
			// Prune here: do not report it, and do not descend into its
			// subtree — everything under a helper belongs to the shim, not
			// to a module.
			continue
		}

		out = append(out, p)
		queue = append(queue, children[p.PID]...)
	}
	return out
}

// findMoveOriginal returns the pid of the TOP-LEVEL MoveOriginal process —
// the lowest pid among processes named "MoveOriginal".
//
// A forked child inherits the name (JP-8000's stage workers report as
// "MoveOriginal" too, see deviceTree in perf_forks_test.go), so matching the
// first "MoveOriginal" found in an unordered /proc scan can root the walk
// inside the very module-forked subtree this file exists to find, rather
// than at the real root.
func findMoveOriginal(all []ProcStat) int {
	found := false
	best := 0
	for _, p := range all {
		if p.Comm != "MoveOriginal" {
			continue
		}
		if !found || p.PID < best {
			best = p.PID
			found = true
		}
	}
	if !found {
		return 0
	}
	return best
}

// ForkedProc is one process a module forked, with the CPU it actually burned.
type ForkedProc struct {
	PID     int
	Comm    string
	Core    int
	Percent float64 // of one core, over the measured interval
}

// LoadedModule is a candidate owner: a module in the rig, and whether it says
// it forks.
type LoadedModule struct {
	ID    string
	Forks bool
}

// ForkGroup is a set of forked processes and who we believe owns them.
//
// Module == "" means unattributed: we found the cost and will not guess at the
// owner. Inferred means we DID guess, and the page must say so.
type ForkGroup struct {
	Module       string
	Inferred     bool
	TotalPercent float64
	Procs        []ForkedProc
}

// attributeForks names the forked processes, by descending confidence:
//
//  1. DECLARED — exactly one loaded module sets capabilities.forks_processes.
//     Authoritative, Inferred false.
//  2. DECLARED, several — we know the names but NOT which pid belongs to
//     which, so the pids are split across them and every group is marked
//     Inferred: the SPLIT is the guess, even though the names are not.
//  3. INFERRED — nothing declares and exactly one synth is loaded. Named, and
//     always marked, because an unlabelled guess on a CPU page is worse than
//     no answer: it sends someone optimising a module that may be idle.
//  4. UNATTRIBUTED — several candidates, or none. One group, real total, no
//     owner.
//
// The ordering exists to serve one rule: NOTHING IS EVER HIDDEN FOR WANT OF A
// NAME. A forked process always appears with its real cost, whatever we can or
// cannot call it — dropping the cost because attribution failed would recreate
// the exact blindness this file was written to fix.
func attributeForks(kids []ForkedProc, loaded []LoadedModule) []ForkGroup {
	if len(kids) == 0 {
		// No group at all, rather than an empty one: a panel saying a module
		// forks and costs nothing is a measurement we never made.
		return nil
	}

	var declarers []string
	for _, m := range loaded {
		if m.Forks && m.ID != "" {
			declarers = append(declarers, m.ID)
		}
	}

	switch {
	case len(declarers) == 1:
		return []ForkGroup{newForkGroup(declarers[0], false, kids)}

	case len(declarers) > 1:
		// Round-robin so every declarer gets a group and no pid lands in two.
		buckets := make([][]ForkedProc, len(declarers))
		for i, k := range kids {
			buckets[i%len(declarers)] = append(buckets[i%len(declarers)], k)
		}
		out := make([]ForkGroup, 0, len(declarers))
		for i, id := range declarers {
			out = append(out, newForkGroup(id, true, buckets[i]))
		}
		return out

	case len(loaded) == 1 && loaded[0].ID != "":
		return []ForkGroup{newForkGroup(loaded[0].ID, true, kids)}

	default:
		return []ForkGroup{newForkGroup("", false, kids)}
	}
}

// newForkGroup sums the procs into a group. Inferred is never set on an
// unattributed group: there is no guess to flag when no name was offered.
func newForkGroup(module string, inferred bool, procs []ForkedProc) ForkGroup {
	g := ForkGroup{Module: module, Inferred: inferred && module != "", Procs: procs}
	for _, p := range procs {
		g.TotalPercent += p.Percent
	}
	return g
}
