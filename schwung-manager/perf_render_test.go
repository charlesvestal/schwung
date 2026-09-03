package main

import (
	"io"
	"log/slog"
	"net/http/httptest"
	"strings"
	"testing"
)

// renderCPUValues renders the polled fragment with whatever data the test
// supplies, through the real templates.
func renderCPUValues(t *testing.T, data map[string]any) string {
	t.Helper()
	tmpl, err := loadTemplates()
	if err != nil {
		t.Fatalf("loadTemplates: %v", err)
	}
	app := &App{tmpl: tmpl, logger: slog.New(slog.NewTextHandler(io.Discard, nil))}
	rec := httptest.NewRecorder()
	req := httptest.NewRequest("GET", "/system/cpu/values", nil)
	app.renderPartial(rec, req, "system_cpu.html", "cpu_values", data)
	if rec.Code != 200 {
		t.Fatalf("render returned %d", rec.Code)
	}
	return rec.Body.String()
}

// cpuValuesFixture is a complete, successful data set; overrides replace keys.
// One place to add a key when the template gains one, so a new key cannot make
// several tests silently render half a page — a missing key renders as
// "<no value>" or truncates the fragment, which has happened once already and
// was caught only by a tail assertion.
func cpuValuesFixture(overrides map[string]any) map[string]any {
	d := map[string]any{
		"Budget": []FrameBudgetRow{{
			Label: "Slot 1 synth", Module: "braids",
			AvgUs: 290, MaxUs: 410, Percent: 10.0, MaxPct: 14.1,
		}},
		"PerfError": "",
		"Headroom": FrameHeadroom{
			Valid: true, PeriodUs: 2679, WorkAvgUs: 344, WorkMaxUs: 537,
			UsedPct: 12.8, UsedMaxPct: 20.0, FreePct: 87.2, IoctlAvgUs: 2306,
		},
		"Snapshot": &PerfSnapshot{SampleWindowFrames: 1000},
		"Process": ProcessView{Rows: []ProcessRow{
			{PID: 42, Comm: "MoveOriginal", Percent: 61.5},
		}},
		"ProcessOK":   true,
		"ForkGroups":  []ForkGroup(nil),
		"Cores":       []CoreRow{{Name: "cpu3", Percent: 18.4, IsSPI: true}},
		"CoresOK":     true,
		"CorePriming": false,
		"Load1":       1.25,
		"Load5":       1.1,
		"Load15":      0.9,
		"LoadOK":      true,
		"RTThreads": []ProcStat{
			{PID: 900, Comm: "Audio Main/SPI", Policy: schedFIFO, RTPrio: 70, CPU: 3},
		},
		"MoveFound": true,
		"SPICore":   3,
		"Measuring": true,
	}
	for k, v := range overrides {
		d[k] = v
	}
	return d
}

// Every /proc read on this page can fail, and each one has an ok flag that is
// easy to discard with `_`. When they were discarded the page rendered
// "Load average: 0.00 / 0.00 / 0.00", a table of headings with no rows, five
// processes reported as "not running", and "None found" for realtime threads —
// four failed reads, all four wearing the costume of a real measurement.
//
// That is the one thing this page exists not to do, and it was invisible until
// someone loaded it in a browser. This test is the cheaper way to notice.
func TestCPUValuesNeverRendersAFailedReadAsZero(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(map[string]any{
		"Budget":      nil,
		"PerfError":   "The shim is not running, or this is not a Move.",
		"Headroom":    FrameHeadroom{},
		"Snapshot":    nil,
		"Process":     ProcessView{},
		"ProcessOK":   false,
		"ForkGroups":  []ForkGroup(nil),
		"Cores":       nil,
		"CoresOK":     false,
		"CorePriming": false,
		"Load1":       0.0,
		"Load5":       0.0,
		"Load15":      0.0,
		"LoadOK":      false,
		"RTThreads":   nil,
		"MoveFound":   false,
	}))

	for _, want := range []string{
		"The shim is not running",
		"Process list unavailable",
		"Load average unavailable",
		"Per-core figures unavailable",
		"MoveOriginal is not running",
	} {
		if !strings.Contains(body, want) {
			t.Errorf("a failed read must say so; missing %q", want)
		}
	}

	// The specific lies this guards against.
	if strings.Contains(body, "0.00 / 0.00 / 0.00") {
		t.Error("a failed loadavg read rendered as a real-looking zero")
	}
	if strings.Contains(body, "None found") {
		t.Error("a scan that never ran rendered as a finding (\"None found\")")
	}
	if strings.Contains(body, "not running</span>") {
		t.Error("an unreadable /proc rendered as named processes being absent")
	}
}

// The other side of the same rule: when the reads DO succeed, the page shows
// the numbers rather than the apology.
func TestCPUValuesShowsDataWhenReadsSucceed(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(nil))

	for _, want := range []string{
		"braids", "290", "10.0%", "1000 frames",
		"MoveOriginal", "61.5%",
		"cpu3", "1.25",
		"Audio Main/SPI", "FIFO", "70",
	} {
		if !strings.Contains(body, want) {
			t.Errorf("expected the rendered page to contain %q", want)
		}
	}
	if strings.Contains(body, "unavailable") {
		t.Error("a successful read must not render an unavailable message")
	}
	// The bar is decoration; the number must be in the markup either way.
	if !strings.Contains(body, `role="progressbar"`) {
		t.Error("frame-budget rows should carry an accessible progressbar")
	}
}

// A fork group whose owner we GUESSED must say so on screen. An unlabelled
// guess on a CPU page is worse than no answer: it sends someone optimising a
// module that may be idle.
func TestCPUValuesMarksAnInferredAttribution(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(map[string]any{
		"ForkGroups": []ForkGroup{{
			Module: "jp8000", Inferred: true, TotalPercent: 240,
			Procs: []ForkedProc{{PID: 1200, Comm: "MoveOriginal", Core: 1, Percent: 240}},
		}},
	}))
	if !strings.Contains(body, "jp8000") {
		t.Error("the guessed owner should still be named")
	}
	if !strings.Contains(strings.ToLower(body), "inferred") {
		t.Error("a guessed attribution must be visibly marked as inferred")
	}
}

// The other side: a DECLARED attribution is authoritative, and must not be
// hedged. If any static prose in the panel carries the word, this can never
// pass — which is the point.
func TestCPUValuesDeclaredIsNotMarkedInferred(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(map[string]any{
		"ForkGroups": []ForkGroup{{
			Module: "jp8000", Inferred: false, TotalPercent: 240,
			Procs: []ForkedProc{{PID: 1200, Comm: "MoveOriginal", Core: 1, Percent: 240}},
		}},
	}))
	if !strings.Contains(body, "jp8000") {
		t.Error("the declared owner should be named")
	}
	if strings.Contains(strings.ToLower(body), "inferred") {
		t.Error("a declared attribution must not be hedged as inferred")
	}
}

// Nothing is hidden for want of a name: an unattributed group still shows its
// cost and its pids.
func TestCPUValuesUnattributedStillShowsCost(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(map[string]any{
		"ForkGroups": []ForkGroup{{
			Module: "", TotalPercent: 240,
			Procs: []ForkedProc{{PID: 1200, Comm: "MoveOriginal", Core: 1, Percent: 240}},
		}},
	}))
	for _, want := range []string{"240", "1200"} {
		if !strings.Contains(body, want) {
			t.Errorf("an unattributed group must still show its cost; missing %q", want)
		}
	}
	if !strings.Contains(strings.ToLower(body), "unattributed") {
		t.Error("a group with no owner must say the owner is unknown")
	}
}

// An empty table is not an answer.
func TestCPUValuesNoForksSaysSo(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(map[string]any{
		"ForkGroups": []ForkGroup(nil),
	}))
	if !strings.Contains(strings.ToLower(body), "no forked") {
		t.Error("with no forked processes the panel should say so, not render an empty table")
	}
}

// The two tables that look unrelated must be visibly connected: a forker's
// frame-budget row understates, and has to point down at its children.
func TestCPUValuesForkNoteOnBudgetRow(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(map[string]any{
		"Budget": []FrameBudgetRow{{
			Label: "Slot 1 synth", Module: "jp8000",
			AvgUs: 130, MaxUs: 210, Percent: 5.0, MaxPct: 8.0,
			Note: "This module also runs 3 forked process(es) on other cores.",
		}},
	}))
	if !strings.Contains(body, "3 forked process(es)") {
		t.Error("the budget row's cross-reference note should render")
	}
}
