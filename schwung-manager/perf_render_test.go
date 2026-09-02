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

// Every /proc read on this page can fail, and each one has an ok flag that is
// easy to discard with `_`. When they were discarded the page rendered
// "Load average: 0.00 / 0.00 / 0.00", a table of headings with no rows, five
// processes reported as "not running", and "None found" for realtime threads —
// four failed reads, all four wearing the costume of a real measurement.
//
// That is the one thing this page exists not to do, and it was invisible until
// someone loaded it in a browser. This test is the cheaper way to notice.
func TestCPUValuesNeverRendersAFailedReadAsZero(t *testing.T) {
	body := renderCPUValues(t, map[string]any{
		"Budget":    nil,
		"PerfError": "The shim is not running, or this is not a Move.",
		"Snapshot":  nil,
		"Process":   ProcessView{},
		"ProcessOK": false,
		"Cores":     nil,
		"CoresOK":   false,
		"Load1":     0.0,
		"Load5":     0.0,
		"Load15":    0.0,
		"LoadOK":    false,
		"RTThreads": nil,
		"MoveFound": false,
		"SPICore":   3,
		"Measuring": true,
	})

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
	body := renderCPUValues(t, map[string]any{
		"Budget": []FrameBudgetRow{{
			Label: "Slot 1 synth", Module: "braids",
			AvgUs: 290, MaxUs: 410, Percent: 10.0, MaxPct: 14.1,
		}},
		"PerfError": "",
		"Snapshot":  &PerfSnapshot{SampleWindowFrames: 1000},
		"Process": ProcessView{Rows: []ProcessRow{
			{PID: 42, Comm: "MoveOriginal", Percent: 61.5},
		}},
		"ProcessOK": true,
		"Cores":     []CoreStat{{Name: "cpu3", Busy: 45, Total: 245}},
		"CoresOK":   true,
		"Load1":     1.25, "Load5": 1.1, "Load15": 0.9,
		"LoadOK": true,
		"RTThreads": []ProcStat{
			{PID: 900, Comm: "Audio Main/SPI", Policy: schedFIFO, RTPrio: 70, CPU: 3},
		},
		"MoveFound": true,
		"SPICore":   3,
		"Measuring": true,
	})

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
