package main

// /proc parsers for the CPU page.
//
// This is a Go port of src/host/rt_thread_audit.c. The manager is a separate
// process, so the C original — which reads /proc/self — cannot answer for
// MoveOriginal, and cgo is not worth ~40 lines of text parsing.
//
// The rule that matters, from the C original: /proc/<pid>/stat's `comm` is
// bracketed and may contain spaces AND parentheses. Move's firmware literally
// names six of its threads "Audio Main/SPI"; a name like "worker (2)" has both.
// So comm is delimited by the LAST ')' in the line, never tokenised on
// whitespace — tokenising shifts every field after comm, and the shifted values
// read as SCHED_OTHER at priority 0. That is a clean all-clear from a parser
// looking in the wrong place, which is worse than no parser at all: the whole
// point of the realtime panel is finding threads that inherited FIFO 70 from
// the SPI callback and starve Move's own `Link Main` at FIFO 35.

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Linux scheduling policies, spelled out so nothing here needs Linux headers
// (and the tests run on the dev Mac).
const (
	schedOther = 0
	schedFIFO  = 1
	schedRR    = 2
)

// Field numbers in proc(5), 1-based, counting the bracketed comm as field 2.
const (
	fieldPPid      = 4
	fieldUtime     = 14
	fieldStime     = 15
	fieldProcessor = 39
	fieldRTPrio    = 40
	fieldPolicy    = 41
)

// ProcStat is one parsed /proc/<pid>/stat (or .../task/<tid>/stat) line.
type ProcStat struct {
	PID    int
	PPid   int // parent pid — walking it is how a module fork is found (see perf_forks.go)
	Comm   string
	Utime  uint64 // clock ticks — USER_HZ is applied at the call site
	Stime  uint64
	CPU    int // last core it ran on; core 3 is meant to be SPI's alone
	RTPrio int
	Policy int
}

// IsRealtime reports FIFO or RR at a nonzero priority.
func (p ProcStat) IsRealtime() bool {
	if p.Policy != schedFIFO && p.Policy != schedRR {
		return false
	}
	return p.RTPrio > 0
}

// parseProcStatLine parses one /proc stat line. ok is false for a malformed or
// truncated line.
func parseProcStatLine(line string) (ProcStat, bool) {
	var out ProcStat

	open := strings.IndexByte(line, '(')
	// LAST ')' — see the file header. IndexByte here is the bug this parser
	// exists to avoid.
	closeIdx := strings.LastIndexByte(line, ')')
	if open < 0 || closeIdx < 0 || closeIdx < open {
		return out, false
	}

	pid, err := strconv.Atoi(strings.TrimSpace(line[:open]))
	if err != nil || pid <= 0 {
		return out, false
	}
	out.PID = pid
	out.Comm = line[open+1 : closeIdx]

	// Everything after the ')' is field 3 onward, all whitespace-separated.
	field := 2 // we have just consumed comm
	var gotCPU, gotPrio, gotPolicy bool

	for _, tok := range strings.Fields(line[closeIdx+1:]) {
		field++
		switch field {
		case fieldPPid:
			out.PPid, _ = strconv.Atoi(tok)
		case fieldUtime:
			out.Utime, _ = strconv.ParseUint(tok, 10, 64)
		case fieldStime:
			out.Stime, _ = strconv.ParseUint(tok, 10, 64)
		case fieldProcessor:
			out.CPU, _ = strconv.Atoi(tok)
			gotCPU = true
		case fieldRTPrio:
			out.RTPrio, _ = strconv.Atoi(tok)
			gotPrio = true
		case fieldPolicy:
			out.Policy, _ = strconv.Atoi(tok)
			gotPolicy = true
		}
		if field >= fieldPolicy {
			break
		}
	}

	// A line that stops before field 41 tells us nothing about scheduling, and
	// reporting it as SCHED_OTHER 0 would be a false all-clear.
	if !gotCPU || !gotPrio || !gotPolicy {
		return ProcStat{}, false
	}
	return out, true
}

// CoreStat is one `cpu` / `cpuN` line of /proc/stat, in jiffies.
type CoreStat struct {
	Name  string
	Busy  uint64
	Total uint64
}

// parseCPUStat reads the cpu/cpuN lines of /proc/stat. Index 0 is the
// aggregate. Only the deltas between two of these mean anything — a single
// sample is the machine's whole uptime.
func parseCPUStat(text string) ([]CoreStat, bool) {
	var out []CoreStat

	for _, line := range strings.Split(text, "\n") {
		f := strings.Fields(line)
		if len(f) < 5 || !strings.HasPrefix(f[0], "cpu") {
			continue
		}

		var total, idle uint64
		for i, tok := range f[1:] {
			v, err := strconv.ParseUint(tok, 10, 64)
			if err != nil {
				break
			}
			total += v
			// idle (index 3) and iowait (index 4) are the not-busy pair.
			if i == 3 || i == 4 {
				idle += v
			}
		}
		out = append(out, CoreStat{Name: f[0], Busy: total - idle, Total: total})
	}

	return out, len(out) > 0
}

// cpuPercent converts a tick delta into a percentage of one core over the
// MEASURED elapsed time.
//
// Dividing by the nominal poll interval instead is the trap: the page polls at
// ~1 Hz, but a slow response, a second browser, or a backgrounded tab all
// change the real interval, and the result is then wrong by exactly that factor
// while looking entirely plausible.
func cpuPercent(tickDelta uint64, clkTck int64, elapsed time.Duration) float64 {
	if elapsed <= 0 || clkTck <= 0 {
		return 0
	}
	return float64(tickDelta) / float64(clkTck) / elapsed.Seconds() * 100
}

// --- thin /proc readers -----------------------------------------------------
// I/O only; the parsing above is what carries the rules and the tests.

func readProcStat(pid int) (ProcStat, bool) {
	b, err := os.ReadFile("/proc/" + strconv.Itoa(pid) + "/stat")
	if err != nil {
		return ProcStat{}, false
	}
	return parseProcStatLine(string(b))
}

func scanProcesses() []ProcStat {
	ents, err := os.ReadDir("/proc")
	if err != nil {
		return nil
	}
	var out []ProcStat
	for _, e := range ents {
		pid, err := strconv.Atoi(e.Name())
		if err != nil || pid <= 0 {
			continue
		}
		// A process can exit between ReadDir and the read; skip it silently.
		if st, ok := readProcStat(pid); ok {
			out = append(out, st)
		}
	}
	return out
}

// scanThreads reads every /proc/<pid>/task/*/stat.
//
// Nothing here matches on NAMES, deliberately. A thread inherits its parent's
// comm, so a worker spawned from a module entry point — which IS the SPI
// callback — reports as "Audio Main/SPI", indistinguishable in `top`, in any
// thread list, or to its own author from the six real ones Move runs under that
// name. That invisibility is the whole reason such threads go unnoticed. The
// panel reports the SET of realtime threads and what they burn; a name cannot
// identify anything here.
func scanThreads(pid int) []ProcStat {
	dir := "/proc/" + strconv.Itoa(pid) + "/task"
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []ProcStat
	for _, e := range ents {
		if _, err := strconv.Atoi(e.Name()); err != nil {
			continue
		}
		b, err := os.ReadFile(filepath.Join(dir, e.Name(), "stat"))
		if err != nil {
			continue // thread exited between ReadDir and open
		}
		if st, ok := parseProcStatLine(string(b)); ok {
			out = append(out, st)
		}
	}
	return out
}

func readLoadAvg() (one, five, fifteen float64, ok bool) {
	b, err := os.ReadFile("/proc/loadavg")
	if err != nil {
		return 0, 0, 0, false
	}
	f := strings.Fields(string(b))
	if len(f) < 3 {
		return 0, 0, 0, false
	}
	var errs [3]error
	one, errs[0] = strconv.ParseFloat(f[0], 64)
	five, errs[1] = strconv.ParseFloat(f[1], 64)
	fifteen, errs[2] = strconv.ParseFloat(f[2], 64)
	for _, e := range errs {
		if e != nil {
			return 0, 0, 0, false
		}
	}
	return one, five, fifteen, true
}

func readCPUStat() ([]CoreStat, bool) {
	b, err := os.ReadFile("/proc/stat")
	if err != nil {
		return nil, false
	}
	return parseCPUStat(string(b))
}
