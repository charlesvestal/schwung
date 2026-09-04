package main

import (
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// buildSnapshot lays out a synthetic /schwung-perf payload. Offsets mirror
// src/host/perf_snapshot.h; tests/host/test_perf_shm_offsets.sh pins them for real.
func buildSnapshot(magic, version, seq uint32) []byte {
	b := make([]byte, perfShmSize)
	binary.LittleEndian.PutUint32(b[perfOffMagic:], magic)
	binary.LittleEndian.PutUint32(b[perfOffVersion:], version)
	binary.LittleEndian.PutUint32(b[perfOffSeq:], seq)
	binary.LittleEndian.PutUint64(b[perfOffFramePeriodUs:], 2902)
	binary.LittleEndian.PutUint32(b[perfOffSampleWindow:], 1000)
	binary.LittleEndian.PutUint64(b[perfOffSlotSynthAvg:], 412)
	return b
}

func TestPerfReadGood(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(perfMagic, perfVersion, 8)}
	snap, err := p.Read()
	if err != nil {
		t.Fatalf("wanted a clean read, got %v", err)
	}
	if snap.FramePeriodUs != 2902 {
		t.Fatalf("FramePeriodUs = %d, want 2902", snap.FramePeriodUs)
	}
	if snap.SlotSynthAvg[0] != 412 {
		t.Fatalf("SlotSynthAvg[0] = %d, want 412", snap.SlotSynthAvg[0])
	}
	if snap.SampleWindowFrames != 1000 {
		t.Fatalf("SampleWindowFrames = %d, want 1000", snap.SampleWindowFrames)
	}
}

// An odd seq means the writer is mid-snapshot. Returning zeros here would say
// "everything is idle", which is the exact lie a failed read must never tell.
func TestPerfOddSeqIsAFailureNotZeros(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(perfMagic, perfVersion, 7)}
	snap, err := p.Read()
	if !errors.Is(err, ErrPerfTorn) {
		t.Fatalf("odd seq must report ErrPerfTorn, got err=%v", err)
	}
	if snap != nil {
		t.Fatal("a failed read must return no snapshot at all - a caller " +
			"handed zeros will draw a picture of an idle device")
	}
}

// seq changing between the two samples means the writer landed a snapshot
// while we were copying it.
// A writer that keeps landing snapshots under us never settles. The hook must
// ADVANCE seq on every call, not rewrite the same value: rewriting the same
// value means attempt 2 reads that value as `before`, sees it unchanged after,
// and legitimately succeeds — so a hook that does not advance is testing the
// retry loop's success path while claiming to test its failure path.
func TestPerfTornReadNeverSettles(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(perfMagic, perfVersion, 8)}
	next := uint32(10)
	p.testHookAfterCopy = func() {
		binary.LittleEndian.PutUint32(p.data[perfOffSeq:], next)
		next += 2 // stay even, so it is "moved", never "in flight"
	}
	snap, err := p.Read()
	if !errors.Is(err, ErrPerfTorn) {
		t.Fatalf("a seq that never settles must report ErrPerfTorn, got %v", err)
	}
	if snap != nil {
		t.Fatal("a failed read must return no snapshot at all")
	}
}

// The other half of the same rule, and the reason both shapes retry: a SINGLE
// collision is recoverable. The writer commits once per ~2.9 s, so a reader
// that lands on it will almost certainly succeed one copy later. Failing here
// would cost the caller a whole poll interval to learn nothing.
func TestPerfTornReadRecoversOnRetry(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(perfMagic, perfVersion, 8)}
	calls := 0
	p.testHookAfterCopy = func() {
		calls++
		if calls == 1 {
			// Collide exactly once, then hold still.
			binary.LittleEndian.PutUint32(p.data[perfOffSeq:], 10)
		}
	}
	snap, err := p.Read()
	if err != nil {
		t.Fatalf("one collision must be retried, not reported: %v", err)
	}
	if snap == nil {
		t.Fatal("wanted a snapshot from the retry")
	}
	if calls < 2 {
		t.Fatalf("expected a second attempt, hook ran %d time(s)", calls)
	}
}

func TestPerfBadMagic(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(0xDEADBEEF, perfVersion, 8)}
	if _, err := p.Read(); !errors.Is(err, ErrPerfMagic) {
		t.Fatalf("wanted ErrPerfMagic, got %v", err)
	}
}

// A version mismatch is the deploy-coupling failure: shim and manager ship
// together, so the page must say so by name rather than render garbage.
func TestPerfVersionMismatchNamesBothVersions(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(perfMagic, perfVersion+1, 8)}
	_, err := p.Read()
	var ve *PerfVersionError
	if !errors.As(err, &ve) {
		t.Fatalf("wanted a PerfVersionError, got %v", err)
	}
	if ve.Got != perfVersion+1 || ve.Want != perfVersion {
		t.Fatalf("PerfVersionError must carry both versions, got %+v", ve)
	}
}

// The offset block is hand-mirrored from the C header, and the derived ones are
// written as arithmetic so a cap change cannot leave two of them disagreeing.
// This pins the arithmetic against the numbers dumped from the compiler.
func TestPerfOffsetsMatchTheHeader(t *testing.T) {
	for _, c := range []struct {
		name string
		got  int
		want int
	}{
		{"magic", perfOffMagic, 0},
		{"version", perfOffVersion, 4},
		{"seq", perfOffSeq, 8},
		{"frame_ready", perfOffFrameReady, 12},
		{"granular_ready", perfOffGranularReady, 16},
		{"sample_window_frames", perfOffSampleWindow, 20},
		{"frame_period_us", perfOffFramePeriodUs, 24},
		{"frame_total_avg", perfOffFrameTotalAvg, 32},
		{"frame_total_max", perfOffFrameTotalMax, 40},
		{"frame_pre_avg", perfOffFramePreAvg, 48},
		{"frame_pre_max", perfOffFramePreMax, 56},
		{"frame_ioctl_avg", perfOffFrameIoctlAvg, 64},
		{"frame_ioctl_max", perfOffFrameIoctlMax, 72},
		{"frame_post_avg", perfOffFramePostAvg, 80},
		{"frame_post_max", perfOffFramePostMax, 88},
		{"sections", perfOffSections, 96},
		{"post chunks", perfOffPostChunks, 432},
		{"slot_render_avg", perfOffSlotRenderAvg, 480},
		{"slot_render_max", perfOffSlotRenderMax, 512},
		{"slot_synth_avg", perfOffSlotSynthAvg, 544},
		{"slot_synth_max", perfOffSlotSynthMax, 576},
		{"slot_fx_avg", perfOffSlotFxAvg, 608},
		{"slot_fx_max", perfOffSlotFxMax, 640},
		{"mfx_avg", perfOffMfxAvg, 672},
		{"mfx_max", perfOffMfxMax, 736},
		{"overtake_gen_avg", perfOffOvertakeGenAvg, 800},
		{"overtake_gen_max", perfOffOvertakeGenMax, 808},
		{"overtake_fx_avg", perfOffOvertakeFxAvg, 816},
		{"overtake_fx_max", perfOffOvertakeFxMax, 824},
		{"slot_probe_burst_max", perfOffProbeBurstMax, 832},
		{"jack_audio_hits", perfOffJackAudioHits, 836},
		{"jack_audio_misses", perfOffJackAudioMisses, 840},
		{"overrun_count", perfOffOverrunCount, 844},
	} {
		if c.got != c.want {
			t.Errorf("offset %s = %d, want %d", c.name, c.got, c.want)
		}
	}
	if len(perfSectionNames) != perfSectionCount {
		t.Fatalf("perfSectionNames has %d entries, want %d - a name without a "+
			"matching C field relabels every section after it",
			len(perfSectionNames), perfSectionCount)
	}
}

// The manager and the shim start independently, and on this device the manager
// WINS: measured after a reboot, it came up 304 ms before the shim created
// /schwung-perf. A single attach at construction therefore returned nil for the
// life of the process, and the page reported "the shim is not running" about a
// shim that was running fine — a confidently WRONG finding, which is worse than
// the zeros this page was built to avoid.
func TestPerfSegmentAttachesLateWhenTheShimStartsSecond(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "schwung-perf")

	orig := perfShmPath
	perfShmPath = path
	t.Cleanup(func() { perfShmPath = orig })

	app := &App{}

	// The shim has not created it yet.
	if got := app.perfSegment(); got != nil {
		t.Fatal("wanted nil while the segment does not exist")
	}

	// The shim comes up.
	if err := os.WriteFile(path, buildSnapshot(perfMagic, perfVersion, 8), 0o644); err != nil {
		t.Fatal(err)
	}

	shm := app.perfSegment()
	if shm == nil {
		t.Fatal("the consumer must retry - a segment that appears after startup " +
			"is the normal case on a cold boot, not an edge case")
	}
	if _, err := shm.Read(); err != nil {
		t.Fatalf("a late attach must yield a usable segment: %v", err)
	}

	// And it must not re-open once attached.
	if app.perfSegment() != shm {
		t.Fatal("perfSegment should cache the mapping after a successful attach")
	}
}

// Leaving the page without pressing Stop leaves the baseline behind, because
// only the Stop route calls reset. Coming back much later must PRIME rather
// than diff against a stale sample, which would render a very long average as
// though it were a one-second one.
func TestCPUStaleBaselinePrimesRatherThanLying(t *testing.T) {
	c := &cpuSampler{clkTck: 100}
	base := time.Unix(1000, 0)

	c.buildProcessView([]ProcStat{{PID: 1, Comm: "MoveOriginal", Utime: 500}}, base)

	// An hour later, with an hour of accumulated CPU.
	view := c.buildProcessView(
		[]ProcStat{{PID: 1, Comm: "MoveOriginal", Utime: 500 + 360000}},
		base.Add(time.Hour))

	if !view.Priming {
		t.Fatal("a baseline an hour old must prime again - diffing against it " +
			"renders a one-hour average as a one-second reading")
	}

	// The very next poll, one second on, is a real delta again.
	view = c.buildProcessView(
		[]ProcStat{{PID: 1, Comm: "MoveOriginal", Utime: 500 + 360000 + 50}},
		base.Add(time.Hour).Add(time.Second))
	if view.Priming {
		t.Fatal("the sample after re-priming must produce a delta")
	}
	if len(view.Rows) == 0 || view.Rows[0].Percent < 49 || view.Rows[0].Percent > 51 {
		t.Fatalf("wanted ~50%% after re-priming, got %+v", view.Rows)
	}
}
