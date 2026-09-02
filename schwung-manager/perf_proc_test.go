package main

import (
	"strconv"
	"strings"
	"testing"
	"time"
)

// makeStatLine builds a synthetic /proc/<pid>/stat line: "pid (comm) S " then
// fields 3..41, with the interesting ones at their proc(5) positions.
func makeStatLine(pid int, comm string, utime, stime uint64, cpu, prio, policy int) string {
	// fields[i] is proc field i+3 — field 3 is the state letter.
	fields := make([]string, fieldPolicy-2)
	for i := range fields {
		fields[i] = "0"
	}
	set := func(field int, v string) { fields[field-3] = v }

	set(3, "S")
	set(fieldUtime, strconv.FormatUint(utime, 10))
	set(fieldStime, strconv.FormatUint(stime, 10))
	set(fieldProcessor, strconv.Itoa(cpu))
	set(fieldRTPrio, strconv.Itoa(prio))
	set(fieldPolicy, strconv.Itoa(policy))

	return strconv.Itoa(pid) + " (" + comm + ") " + strings.Join(fields, " ") + "\n"
}

func TestParseProcStatCommWithSpace(t *testing.T) {
	// Move names six of its threads exactly this.
	line := makeStatLine(1234, "Audio Main/SPI", 500, 100, 3, 70, schedFIFO)

	got, ok := parseProcStatLine(line)
	if !ok {
		t.Fatalf("parse failed on a well-formed line: %q", line)
	}
	if got.Comm != "Audio Main/SPI" {
		t.Errorf("Comm = %q, want %q", got.Comm, "Audio Main/SPI")
	}
	if got.Policy != schedFIFO || got.RTPrio != 70 {
		t.Errorf("policy/prio = %d/%d, want %d/70 — whitespace-tokenising comm "+
			"shifts EVERY field after it, and the shifted values read as "+
			"SCHED_OTHER 0: a clean all-clear from a parser looking in the "+
			"wrong place", got.Policy, got.RTPrio, schedFIFO)
	}
	if got.CPU != 3 {
		t.Errorf("CPU = %d, want 3 (same field shift)", got.CPU)
	}
	if got.Utime != 500 || got.Stime != 100 {
		t.Errorf("utime/stime = %d/%d, want 500/100 (same field shift)", got.Utime, got.Stime)
	}
	if !got.IsRealtime() {
		t.Errorf("IsRealtime() = false for FIFO 70 — this thread is exactly what the panel exists to find")
	}
}

func TestParseProcStatCommWithParen(t *testing.T) {
	// Both a space and parentheses: comm must be delimited on the LAST ')'.
	line := makeStatLine(77, "worker (2)", 1, 2, 0, 0, schedOther)

	got, ok := parseProcStatLine(line)
	if !ok {
		t.Fatalf("parse failed on a well-formed line: %q", line)
	}
	if got.Comm != "worker (2)" {
		t.Errorf("Comm = %q, want %q — comm must be delimited on the LAST ')', "+
			"not the first", got.Comm, "worker (2)")
	}
	if got.Policy != schedOther {
		t.Errorf("Policy = %d, want %d", got.Policy, schedOther)
	}
	if got.IsRealtime() {
		t.Errorf("IsRealtime() = true for SCHED_OTHER 0")
	}
}

func TestParseProcStatTruncatedIsNotOK(t *testing.T) {
	// A line that stops before field 41 says nothing about scheduling.
	if got, ok := parseProcStatLine("42 (short) S 1 2 3"); ok {
		t.Errorf("ok = true for a truncated line (got %+v) — reporting it as "+
			"SCHED_OTHER 0 would be a false all-clear", got)
	}
}

func TestParseCPUStat(t *testing.T) {
	// user nice system idle iowait irq softirq steal guest guest_nice
	text := strings.Join([]string{
		"cpu  100 20 80 700 50 0 10 0 0 0",
		"cpu0 25 5 20 175 10 0 2 0 0 0",
		"cpu1 25 5 20 175 10 0 2 0 0 0",
		"cpu2 25 5 20 175 10 0 3 0 0 0",
		"cpu3 25 5 20 175 20 0 3 0 0 0",
		"intr 12345 0 0",
		"ctxt 999",
	}, "\n")

	cores, ok := parseCPUStat(text)
	if !ok {
		t.Fatal("parseCPUStat returned ok=false on a valid sample")
	}
	if len(cores) != 5 {
		t.Fatalf("got %d entries, want 5 (aggregate + cpu0..cpu3); non-cpu lines must be skipped", len(cores))
	}
	if cores[0].Name != "cpu" {
		t.Errorf("index 0 = %q, want the aggregate %q", cores[0].Name, "cpu")
	}

	// cpu3: total 25+5+20+175+20+0+3 = 248; idle+iowait = 175+20 = 195.
	c3 := cores[4]
	if c3.Name != "cpu3" {
		t.Fatalf("cores[4].Name = %q, want cpu3", c3.Name)
	}
	if c3.Total != 248 {
		t.Errorf("cpu3 Total = %d, want 248 (sum of ALL numeric fields)", c3.Total)
	}
	if c3.Busy != 248-195 {
		t.Errorf("cpu3 Busy = %d, want %d — busy is total minus idle AND iowait "+
			"(fields 4 and 5), not idle alone", c3.Busy, 248-195)
	}
}

func TestCPUPercentUsesMeasuredElapsed(t *testing.T) {
	const clkTck = 100 // USER_HZ; 50 ticks = 0.5 s of CPU

	if got := cpuPercent(50, clkTck, 2*time.Second); got != 25 {
		t.Errorf("cpuPercent(50 ticks @100Hz, 2s) = %v, want 25 — the divisor "+
			"must be the MEASURED elapsed time; dividing by an assumed poll "+
			"interval is wrong by exactly the drift factor and looks plausible", got)
	}
	if got := cpuPercent(50, clkTck, time.Second); got != 50 {
		t.Errorf("cpuPercent(50 ticks @100Hz, 1s) = %v, want 50 — the same tick "+
			"delta must give a different percentage over a different elapsed", got)
	}
}

func TestCPUPercentZeroElapsedIsZeroNotInfinity(t *testing.T) {
	for _, d := range []time.Duration{0, -time.Second} {
		got := cpuPercent(50, 100, d)
		if got != 0 {
			t.Errorf("cpuPercent(elapsed=%v) = %v, want 0 (not Inf/NaN)", d, got)
		}
	}
	if got := cpuPercent(50, 0, time.Second); got != 0 {
		t.Errorf("cpuPercent(clkTck=0) = %v, want 0 (not Inf/NaN)", got)
	}
}
