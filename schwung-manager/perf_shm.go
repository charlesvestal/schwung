package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"sync"
	"syscall"
)

// PerfShm reads the SPI frame-budget snapshot the shim publishes to
// /schwung-perf. It is the ONLY per-module attribution that exists on this
// device: modules are not processes — every slot synth, slot FX, Master FX and
// overtake DSP is a .so running on the SPI callback inside MoveOriginal — so
// /proc can say what MoveOriginal costs in total and can never split it.
//
// Field offsets are hand-mirrored from schwung_perf_snapshot_t in
// src/host/perf_snapshot.h. If that struct gains, loses or reorders a field
// without this block moving with it, every number below silently lands in the
// wrong label and the CPU page draws a confident picture of the wrong thing.
// That is why the C side bumps SCHWUNG_PERF_VERSION on any layout change and
// why Read() refuses a version it does not know rather than rendering it.
type PerfShm struct {
	data []byte
	mu   sync.Mutex

	// Test seam, nil in production. Called between copying the payload out and
	// re-reading seq, so a test can simulate the writer landing a snapshot
	// mid-read.
	testHookAfterCopy func()
}

// Errors a read can report. Each one is a DIFFERENT finding, and the whole
// point of returning them rather than zeros: "the shim is not running", "the
// shim is older than this manager" and "everything is idle" would otherwise
// all draw the same 0% bar.
var (
	// ErrPerfAbsent — no segment, or one too short to be this struct.
	ErrPerfAbsent = errors.New("perf snapshot unavailable (shim not running?)")
	// ErrPerfMagic — the segment is not ours.
	ErrPerfMagic = errors.New("perf snapshot magic mismatch")
	// ErrPerfTorn — the seqlock never settled: the writer was mid-snapshot for
	// every attempt, or landed one while we were copying.
	ErrPerfTorn = errors.New("perf snapshot read torn (writer active)")
)

// PerfVersionError names both versions, because the fix is a deploy action and
// a bare "mismatch" does not tell you which half is stale.
type PerfVersionError struct {
	Got, Want uint32
}

func (e *PerfVersionError) Error() string {
	return fmt.Sprintf("perf snapshot version %d, this manager understands %d "+
		"- deploy the shim and the manager together", e.Got, e.Want)
}

const (
	perfShmPath = "/dev/shm/schwung-perf"

	// SCHWUNG_PERF_MAGIC / _VERSION / _SHM_SIZE from perf_snapshot.h.
	perfMagic   = 0x50455246 // "PERF"
	perfVersion = 1
	perfShmSize = 4096

	// PERF_CHAIN_SLOTS / PERF_MASTER_FX_SLOTS from perf_snapshot.h.
	perfChainSlots    = 4
	perfMasterFXSlots = 8

	// One (avg, max) uint64 pair.
	perfSectionStride = 16

	// The 21 granular pre-ioctl sections and the 3 post-ioctl chunks have
	// exactly the same (avg, max) shape and are contiguous in the C struct, so
	// they are walked as ONE run of 24. Counting the post chunks as a separate
	// block and adding their size again is a 48-byte error that puts every
	// per-slot number below in the wrong field.
	perfGranularSectionCount = 21
	perfPostChunkCount       = 3
	perfSectionCount         = perfGranularSectionCount + perfPostChunkCount
)

// Byte offsets into schwung_perf_snapshot_t. The leading scalars are written
// out literally (they are what the compiler dumped); everything from the
// sections run down is DERIVED, so raising a slot cap moves the fields behind
// it automatically instead of leaving two constants disagreeing.
const (
	perfOffMagic         = 0
	perfOffVersion       = 4
	perfOffSeq           = 8
	perfOffFrameReady    = 12
	perfOffGranularReady = 16
	perfOffSampleWindow  = 20
	// uint64 from here on, so the compiler padded to 24.
	perfOffFramePeriodUs = 24

	perfOffFrameTotalAvg = 32
	perfOffFrameTotalMax = 40
	perfOffFramePreAvg   = 48
	perfOffFramePreMax   = 56
	perfOffFrameIoctlAvg = 64
	perfOffFrameIoctlMax = 72
	perfOffFramePostAvg  = 80
	perfOffFramePostMax  = 88

	perfOffSections = 96
	// Declared so the offsets test can assert where the post-ioctl chunks
	// begin (432). Nothing indexes off it — the run above covers them.
	perfOffPostChunks = perfOffSections + perfGranularSectionCount*perfSectionStride

	perfOffSlotRenderAvg = perfOffSections + perfSectionCount*perfSectionStride
	perfOffSlotRenderMax = perfOffSlotRenderAvg + perfChainSlots*8
	perfOffSlotSynthAvg  = perfOffSlotRenderMax + perfChainSlots*8
	perfOffSlotSynthMax  = perfOffSlotSynthAvg + perfChainSlots*8
	perfOffSlotFxAvg     = perfOffSlotSynthMax + perfChainSlots*8
	perfOffSlotFxMax     = perfOffSlotFxAvg + perfChainSlots*8

	perfOffMfxAvg = perfOffSlotFxMax + perfChainSlots*8
	perfOffMfxMax = perfOffMfxAvg + perfMasterFXSlots*8

	perfOffOvertakeGenAvg = perfOffMfxMax + perfMasterFXSlots*8
	perfOffOvertakeGenMax = perfOffOvertakeGenAvg + 8
	perfOffOvertakeFxAvg  = perfOffOvertakeGenMax + 8
	perfOffOvertakeFxMax  = perfOffOvertakeFxAvg + 8

	perfOffProbeBurstMax   = perfOffOvertakeFxMax + 8
	perfOffJackAudioHits   = perfOffProbeBurstMax + 4
	perfOffJackAudioMisses = perfOffJackAudioHits + 4
	perfOffOverrunCount    = perfOffJackAudioMisses + 4

	// The last field this reader touches. Anything shorter than this cannot be
	// decoded at all.
	perfMinReadableBytes = perfOffOverrunCount + 4
)

// perfSectionNames labels the 24-entry run in STRUCT ORDER, and the order IS
// the contract. There is no name in the segment — only position — so inserting
// a name here without a matching C field silently relabels every section after
// it, and the page keeps rendering without complaint.
//
// "Process MIDI (incl. MIDI FX)" is where MIDI FX cost lands. MIDI FX have no
// per-frame render — they run event-driven inside the chain host's on_midi —
// so their cost is NOT separable per module and must never be presented as if
// it were.
var perfSectionNames = []string{
	"MIDI monitor", "Forward MIDI", "Mix audio", "UI requests", "Param requests",
	"Forward CC", "Process MIDI (incl. MIDI FX)", "JACK stash", "Drain DSP",
	"JACK wake", "Mix buffer", "TTS", "Display", "Clear LEDs", "JACK MIDI out",
	"UI MIDI out", "Flush LEDs", "Screen reader", "JACK pre", "JACK display",
	"CPU pin", "Post MIDI scan", "Post drain DSP", "Post render",
}

// PerfSnapshot is a decoded, self-consistent copy. It is only ever handed out
// alongside a nil error; a read that did not complete returns nil.
type PerfSnapshot struct {
	SampleWindowFrames uint32
	FramePeriodUs      uint64
	FrameReady         bool
	GranularReady      bool

	FrameTotalAvg, FrameTotalMax uint64
	FramePreAvg, FramePreMax     uint64
	FrameIoctlAvg, FrameIoctlMax uint64
	FramePostAvg, FramePostMax   uint64

	SectionAvg [perfSectionCount]uint64
	SectionMax [perfSectionCount]uint64

	SlotRenderAvg, SlotRenderMax [perfChainSlots]uint64
	SlotSynthAvg, SlotSynthMax   [perfChainSlots]uint64
	SlotFxAvg, SlotFxMax         [perfChainSlots]uint64

	MfxAvg, MfxMax [perfMasterFXSlots]uint64

	OvertakeGenAvg, OvertakeGenMax uint64
	OvertakeFxAvg, OvertakeFxMax   uint64

	ProbeBurstMax uint32
	OverrunCount  uint32
}

// OpenPerfShm maps the perf segment read-only. Returns nil when it is not
// there, matching OpenShmParams: off-device (developer machine, CI) is the
// common case and is not an error, it just means no CPU page.
func OpenPerfShm() *PerfShm {
	f, err := os.OpenFile(perfShmPath, os.O_RDONLY, 0)
	if err != nil {
		return nil
	}
	defer f.Close()

	// Never map more than the segment actually holds. A stale, SHORTER segment
	// left behind by an older shim would map fine and then SIGBUS on the first
	// touch past its end — in this process, at some arbitrary later moment,
	// with nothing to connect the crash to the cause. This mirrors the fstat
	// check the C side does in shadow_shm_map for the same reason.
	st, err := f.Stat()
	if err != nil || st.Size() < perfShmSize {
		return nil
	}

	data, err := syscall.Mmap(int(f.Fd()), 0, perfShmSize,
		syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		return nil
	}
	return &PerfShm{data: data}
}

// Read returns a consistent snapshot, or reports the read as FAILED.
//
// It NEVER returns a zero-valued snapshot with a nil error. "the read did not
// complete" and "the device is idle" are different sentences and a 0% bar
// would tell the same lie for both, so on any failure this returns (nil, err)
// and the caller has to say something about it.
//
// The consistency mechanism is the shim's seqlock: the writer does
// seq++ ... stores ... seq++ on the SPI callback and never blocks, so an ODD
// seq means a write is in flight and the same EVEN seq before and after the
// copy means nothing landed underneath us.
func (p *PerfShm) Read() (*PerfSnapshot, error) {
	if p == nil || len(p.data) < perfMinReadableBytes {
		return nil, ErrPerfAbsent
	}

	p.mu.Lock()
	defer p.mu.Unlock()

	// magic and version are the first 8 bytes precisely so they can be trusted
	// off a segment whose tail we may not understand. Check them before
	// touching anything else.
	if m := binary.LittleEndian.Uint32(p.data[perfOffMagic:]); m != perfMagic {
		return nil, fmt.Errorf("%w (read 0x%08X, want 0x%08X)",
			ErrPerfMagic, m, uint32(perfMagic))
	}
	if v := binary.LittleEndian.Uint32(p.data[perfOffVersion:]); v != perfVersion {
		return nil, &PerfVersionError{Got: v, Want: perfVersion}
	}

	// Three attempts at catching the writer between snapshots. It holds the
	// lock for a handful of stores on a ~2.9 ms cadence, so an odd seq three
	// times running means it is genuinely wedged, not that we were unlucky.
	//
	// The two failure shapes are deliberately NOT treated the same. An odd seq
	// is "not yet" and is worth another look immediately. A seq that MOVED
	// across the copy is "a snapshot landed underneath us" — the data we hold
	// is a mix of two frames and there is nothing to salvage from it, so it is
	// reported rather than papered over by an inline retry. Either way the
	// caller polls again; neither ever becomes a picture.
	for attempt := 0; attempt < 3; attempt++ {
		before := binary.LittleEndian.Uint32(p.data[perfOffSeq:])
		if before%2 != 0 {
			continue // write in flight
		}

		snap := p.decode()

		if p.testHookAfterCopy != nil {
			p.testHookAfterCopy()
		}

		if after := binary.LittleEndian.Uint32(p.data[perfOffSeq:]); after != before {
			return nil, fmt.Errorf("%w (seq %d -> %d during copy)",
				ErrPerfTorn, before, after)
		}
		return snap, nil
	}
	return nil, ErrPerfTorn
}

// decode copies the payload out. Called only between two seq reads; its result
// is discarded unless those two agree.
func (p *PerfShm) decode() *PerfSnapshot {
	u64 := func(off int) uint64 { return binary.LittleEndian.Uint64(p.data[off:]) }
	u32 := func(off int) uint32 { return binary.LittleEndian.Uint32(p.data[off:]) }

	s := &PerfSnapshot{
		SampleWindowFrames: u32(perfOffSampleWindow),
		FramePeriodUs:      u64(perfOffFramePeriodUs),
		FrameReady:         u32(perfOffFrameReady) != 0,
		GranularReady:      u32(perfOffGranularReady) != 0,

		FrameTotalAvg: u64(perfOffFrameTotalAvg),
		FrameTotalMax: u64(perfOffFrameTotalMax),
		FramePreAvg:   u64(perfOffFramePreAvg),
		FramePreMax:   u64(perfOffFramePreMax),
		FrameIoctlAvg: u64(perfOffFrameIoctlAvg),
		FrameIoctlMax: u64(perfOffFrameIoctlMax),
		FramePostAvg:  u64(perfOffFramePostAvg),
		FramePostMax:  u64(perfOffFramePostMax),

		OvertakeGenAvg: u64(perfOffOvertakeGenAvg),
		OvertakeGenMax: u64(perfOffOvertakeGenMax),
		OvertakeFxAvg:  u64(perfOffOvertakeFxAvg),
		OvertakeFxMax:  u64(perfOffOvertakeFxMax),

		ProbeBurstMax: u32(perfOffProbeBurstMax),
		OverrunCount:  u32(perfOffOverrunCount),
	}

	// One run of 24 interleaved (avg, max) pairs — the pre-ioctl sections and
	// the post-ioctl chunks together. See perfSectionCount.
	for i := 0; i < perfSectionCount; i++ {
		off := perfOffSections + i*perfSectionStride
		s.SectionAvg[i] = u64(off)
		s.SectionMax[i] = u64(off + 8)
	}

	for i := 0; i < perfChainSlots; i++ {
		s.SlotRenderAvg[i] = u64(perfOffSlotRenderAvg + i*8)
		s.SlotRenderMax[i] = u64(perfOffSlotRenderMax + i*8)
		s.SlotSynthAvg[i] = u64(perfOffSlotSynthAvg + i*8)
		s.SlotSynthMax[i] = u64(perfOffSlotSynthMax + i*8)
		s.SlotFxAvg[i] = u64(perfOffSlotFxAvg + i*8)
		s.SlotFxMax[i] = u64(perfOffSlotFxMax + i*8)
	}

	for i := 0; i < perfMasterFXSlots; i++ {
		s.MfxAvg[i] = u64(perfOffMfxAvg + i*8)
		s.MfxMax[i] = u64(perfOffMfxMax + i*8)
	}

	return s
}
