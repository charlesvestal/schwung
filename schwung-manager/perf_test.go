package main

import (
	"strings"
	"testing"
	"time"
)

func TestCPUFirstSampleIsPrimingNotZero(t *testing.T) {
	s := &cpuSampler{clkTck: 100}
	view := s.buildProcessView([]ProcStat{
		{PID: 1, Comm: "MoveOriginal", Utime: 1000, Stime: 500},
	}, time.Now())

	if !view.Priming {
		t.Errorf("first sample must report Priming: one /proc read is the process's "+
			"LIFETIME average, not its current CPU. got Priming=%v", view.Priming)
	}
	if len(view.Rows) != 0 {
		t.Errorf("first sample must yield no rows — a 0%% row is a lie about a "+
			"process we have not measured yet. got %d rows", len(view.Rows))
	}
}

func TestCPUSecondSampleUsesMeasuredInterval(t *testing.T) {
	s := &cpuSampler{clkTck: 100}
	t0 := time.Now()
	s.buildProcessView([]ProcStat{{PID: 1, Comm: "MoveOriginal", Utime: 1000}}, t0)

	// +200 ticks at 100 Hz = 2 s of CPU, over a 2 s wall interval = 100%.
	view := s.buildProcessView(
		[]ProcStat{{PID: 1, Comm: "MoveOriginal", Utime: 1200}},
		t0.Add(2*time.Second))

	if view.Priming {
		t.Fatal("second sample must not be priming — it has a predecessor")
	}
	var got float64
	var found bool
	for _, r := range view.Rows {
		if r.PID == 1 {
			got, found = r.Percent, true
		}
	}
	if !found {
		t.Fatal("MoveOriginal missing from the second sample")
	}
	if got < 99 || got > 101 {
		t.Errorf("percent must divide by MEASURED elapsed, not an assumed poll "+
			"interval: 200 ticks / 100 Hz over 2 s is 100%%, got %.2f%%", got)
	}
}

func TestCPUAlwaysListedProcessesAppearAtZero(t *testing.T) {
	s := &cpuSampler{clkTck: 100}
	t0 := time.Now()
	procs := []ProcStat{{PID: 1, Comm: "MoveOriginal", Utime: 1000}}
	s.buildProcessView(procs, t0)
	view := s.buildProcessView(procs, t0.Add(time.Second))

	seen := map[string]ProcessRow{}
	for _, r := range view.Rows {
		seen[r.Comm] = r
	}
	for _, name := range alwaysListedProcesses {
		if _, ok := seen[name]; !ok {
			t.Errorf("%q must be listed even at 0%%: omitting it makes "+
				"\"not running\" indistinguishable from \"running but idle\"", name)
		}
	}
	if r, ok := seen["link-subscriber"]; !ok || !r.Absent {
		t.Errorf("a process that is not running must be marked Absent, not "+
			"drawn as an idle 0%% row. got %+v", r)
	}
	if r := seen["MoveOriginal"]; r.Absent {
		t.Error("MoveOriginal was present in the scan and must not be marked Absent")
	}
}

func TestCPUFrameBudgetOmitsEmptySlots(t *testing.T) {
	snap := &PerfSnapshot{FramePeriodUs: 2902}
	snap.SlotSynthAvg = [perfChainSlots]uint64{290, 0, 0, 0}

	rows := buildFrameBudget(snap, map[int]moduleID{0: {Name: "braids", Answered: true}}, nil)

	if len(rows) != 1 {
		t.Fatalf("an empty slot is not a slot at 0%% — it is not a row. got %d rows: %+v",
			len(rows), rows)
	}
	if rows[0].Module != "braids" {
		t.Errorf("slot row must carry the module id from synth_module, got %q", rows[0].Module)
	}
	if rows[0].Percent < 9.5 || rows[0].Percent > 10.5 {
		t.Errorf("290us of a 2902us frame is ~10%%, got %.2f%%", rows[0].Percent)
	}
}

func TestCPUFrameBudgetFallsBackToNominalPeriod(t *testing.T) {
	snap := &PerfSnapshot{FramePeriodUs: 0}
	snap.SlotSynthAvg = [perfChainSlots]uint64{1451, 0, 0, 0}

	rows := buildFrameBudget(snap, map[int]moduleID{0: {Name: "braids", Answered: true}}, nil)

	if len(rows) != 1 {
		t.Fatalf("expected 1 row, got %d", len(rows))
	}
	if rows[0].Percent < 49.5 || rows[0].Percent > 50.5 {
		t.Errorf("a zero FramePeriodUs must fall back to the nominal %dus rather "+
			"than divide by zero: 1451us is ~50%%, got %.2f%%",
			nominalFramePeriodUs, rows[0].Percent)
	}
}

func TestCPUVersionMismatchIsExplicit(t *testing.T) {
	msg := describePerfError(&PerfVersionError{Got: 2, Want: 1})
	if !strings.Contains(msg, "deploy the shim and the manager together") {
		t.Errorf("a version mismatch must name the deploy action, not render as "+
			"zeros. got %q", msg)
	}
}

func TestCPUAbsentIsNotIdle(t *testing.T) {
	msg := describePerfError(ErrPerfAbsent)
	low := strings.ToLower(msg)
	if !strings.Contains(low, "not running") {
		t.Errorf("ErrPerfAbsent must say the shim is not running, got %q", msg)
	}
	if strings.Contains(low, "idle") && !strings.Contains(low, "not the same as an idle") {
		t.Errorf("a missing shim must never read as an idle device, got %q", msg)
	}
}

// "Empty" and "we could not read what is there" are different, and only the
// first may hide a row. A slot that is burning CPU must never disappear from
// the page because its NAME could not be read — that is the worst failure
// available to a page whose whole job is finding what costs time.
func TestCPUFrameBudgetShowsASlotWhoseNameCouldNotBeRead(t *testing.T) {
	snap := &PerfSnapshot{
		FramePeriodUs: 2902,
		SlotSynthAvg:  [perfChainSlots]uint64{290, 290, 0, 0},
	}
	rows := buildFrameBudget(snap, map[int]moduleID{
		0: {Name: "", Answered: false}, // the read failed
		1: {Name: "", Answered: true},  // genuinely empty
	}, nil)

	if len(rows) != 1 {
		t.Fatalf("got %d rows, want 1 - the unread slot must show, the empty "+
			"one must not", len(rows))
	}
	if rows[0].Module != "(name unread)" {
		t.Fatalf("Module = %q, want it to say the name was unread rather than "+
			"pass off a failed read as a module or hide the row", rows[0].Module)
	}
	if rows[0].Percent < 9.5 || rows[0].Percent > 10.5 {
		t.Fatalf("the timing must still be reported: Percent = %.2f, want ~10",
			rows[0].Percent)
	}
}
