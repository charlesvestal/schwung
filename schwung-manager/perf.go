package main

// The join layer for the CPU page: it puts the shim's frame-budget snapshot
// next to /proc, WITHOUT ever adding the two together.
//
// They measure different things. The frame budget is time spent inside ONE SPI
// callback on core 3, and it is the only per-module attribution that exists —
// every slot synth, slot FX, Master FX and overtake DSP is a .so called from
// that callback inside MoveOriginal, so /proc can say what MoveOriginal costs
// in total and can never split it by module. Process CPU is whole-process time
// across cores 0-3. A number that mixed them would be meaningless in both
// directions, so nothing here sums across the boundary.

import (
	"errors"
	"net/http"
	"sort"
	"strconv"
	"sync"
	"time"
)

// nominalFramePeriodUs is 128 frames at 44100 Hz — the SPI frame period the
// shim is paced to. It is a FALLBACK ONLY: the snapshot carries the measured
// period, and that is what percentages divide by whenever it is nonzero.
const nominalFramePeriodUs = 2902

// processFloorPercent hides the long tail of idle system processes. A process
// below this is noise, not a finding.
const processFloorPercent = 0.5

// alwaysListedProcesses appear even at 0%, so that a missing link-subscriber
// reads as ABSENT rather than as silence. "not running" and "running but idle"
// are different findings, and a list that simply omitted the process would
// render them identically — which is the same mistake as drawing a failed read
// as a 0% bar.
var alwaysListedProcesses = []string{
	"MoveOriginal", "link-subscriber", "shadow_ui", "jackd", "schwung-manager",
}

// FrameBudgetRow is one attributed slice of the SPI frame. Percent is against
// the frame period, so it is "share of the realtime budget on core 3" and
// never comparable with a ProcessRow.
type FrameBudgetRow struct {
	Label   string
	Module  string
	AvgUs   uint64
	MaxUs   uint64
	Percent float64
	MaxPct  float64
	Note    string
}

// ProcessRow is one process's CPU over the MEASURED interval between two
// samples. Absent means the process was not found at all.
type ProcessRow struct {
	PID     int
	Comm    string
	Percent float64
	Absent  bool
}

// ProcessView is the process half of the page. Priming means we have exactly
// one sample and therefore NO percentages: a single /proc read is the
// process's lifetime average, not its current CPU, and rendering it as either
// would be a lie.
type ProcessView struct {
	Priming bool
	Rows    []ProcessRow
}

// cpuSampler holds the previous /proc sample so the next one can be a delta.
type cpuSampler struct {
	mu     sync.Mutex
	clkTck int64

	prevProcs map[int]uint64 // pid -> utime+stime in clock ticks
	prevAt    time.Time
}

// newCPUSampler returns a sampler with USER_HZ = 100.
//
// Go has no portable sysconf(_SC_CLK_TCK), and 100 is what every Move kernel
// seen so far reports. It is named rather than inlined because if it is ever
// wrong, every percentage on the page is wrong by exactly the same factor —
// which is the kind of error that looks entirely plausible and is invisible
// without something to point at.
func newCPUSampler() *cpuSampler {
	return &cpuSampler{clkTck: 100}
}

// reset drops the previous sample, so the next call primes again.
func (c *cpuSampler) reset() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.prevProcs = nil
	c.prevAt = time.Time{}
}

// buildProcessView turns one /proc scan into a view, using the stored previous
// sample for the delta.
func (c *cpuSampler) buildProcessView(procs []ProcStat, now time.Time) ProcessView {
	c.mu.Lock()
	defer c.mu.Unlock()

	cur := make(map[int]uint64, len(procs))
	byPID := make(map[int]ProcStat, len(procs))
	for _, p := range procs {
		cur[p.PID] = p.Utime + p.Stime
		byPID[p.PID] = p
	}

	// Prime when there is no usable predecessor: we hold a lifetime average and
	// nothing else, so say so rather than render it.
	//
	// A predecessor older than this is not a baseline, it is a different
	// session. Leaving the page without pressing Stop keeps the old sample
	// around (only the Stop route calls reset), so returning an hour later
	// would diff against an hour-old reading and render a one-HOUR average as
	// though it were a one-second one - plausible-looking and wrong, which is
	// the same lie the priming state exists to prevent. Prime again instead.
	// Generous against the 1 s poll, so an ordinary slow response or a briefly
	// backgrounded tab still yields a real delta.
	const maxBaselineAge = 10 * time.Second

	if c.prevProcs == nil || c.prevAt.IsZero() || now.Sub(c.prevAt) > maxBaselineAge {
		c.prevProcs = cur
		c.prevAt = now
		return ProcessView{Priming: true}
	}

	elapsed := now.Sub(c.prevAt)

	always := make(map[string]bool, len(alwaysListedProcesses))
	for _, n := range alwaysListedProcesses {
		always[n] = true
	}
	seen := make(map[string]bool, len(alwaysListedProcesses))

	var rows []ProcessRow
	for pid, total := range cur {
		prev, ok := c.prevProcs[pid]
		if !ok {
			continue // new since the last sample; it has no delta yet
		}
		// A counter that went backwards means the pid was reused. Skip it
		// rather than reporting the wrap as a huge percentage.
		if total < prev {
			continue
		}
		p := byPID[pid]
		pct := cpuPercent(total-prev, c.clkTck, elapsed)

		if always[p.Comm] {
			seen[p.Comm] = true
		} else if pct < processFloorPercent {
			continue
		}
		rows = append(rows, ProcessRow{PID: pid, Comm: p.Comm, Percent: pct})
	}

	for _, name := range alwaysListedProcesses {
		if !seen[name] {
			rows = append(rows, ProcessRow{Comm: name, Absent: true})
		}
	}

	sort.SliceStable(rows, func(i, j int) bool {
		return rows[i].Percent > rows[j].Percent
	})

	c.prevProcs = cur
	c.prevAt = now
	return ProcessView{Rows: rows}
}

// buildFrameBudget attributes the snapshot's timings to labelled rows.
//
// An EMPTY slot is not a slot at 0% - it is not a row. But "empty" and "we
// could not read what is there" are DIFFERENT, and only the first may hide a
// row: a position whose identity read failed still gets a row whenever it shows
// time, labelled so, because a slot burning CPU that vanishes from the page
// because we could not read its NAME is the worst failure this page has.
func buildFrameBudget(snap *PerfSnapshot, slotModules, mfxModules map[int]moduleID) []FrameBudgetRow {
	if snap == nil {
		return nil
	}

	period := float64(snap.FramePeriodUs)
	if period <= 0 {
		period = nominalFramePeriodUs
	}

	var rows []FrameBudgetRow
	add := func(label, module string, avg, max uint64, note string) {
		rows = append(rows, FrameBudgetRow{
			Label:   label,
			Module:  module,
			AvgUs:   avg,
			MaxUs:   max,
			Percent: float64(avg) / period * 100,
			MaxPct:  float64(max) / period * 100,
			Note:    note,
		})
	}

	// label decides how a position is named, and whether it may be hidden.
	// Answered-and-empty is the only case that hides anything.
	label := func(m moduleID) (name string, show bool) {
		switch {
		case m.Loaded():
			return m.Name, true
		case !m.Answered:
			return "(name unread)", true
		default:
			return "", false
		}
	}

	for i := 0; i < perfChainSlots; i++ {
		mod, show := label(slotModules[i])
		if !show {
			continue
		}
		n := strconv.Itoa(i + 1)
		if snap.SlotSynthAvg[i] > 0 || snap.SlotSynthMax[i] > 0 {
			add("Slot "+n+" synth", mod, snap.SlotSynthAvg[i], snap.SlotSynthMax[i], "")
		}
		if snap.SlotFxAvg[i] > 0 || snap.SlotFxMax[i] > 0 {
			add("Slot "+n+" FX", mod, snap.SlotFxAvg[i], snap.SlotFxMax[i], "")
		}
	}

	for i := 0; i < perfMasterFXSlots; i++ {
		mod, show := label(mfxModules[i])
		if !show {
			continue
		}
		if snap.MfxAvg[i] > 0 || snap.MfxMax[i] > 0 {
			add("Master FX "+strconv.Itoa(i+1), mod, snap.MfxAvg[i], snap.MfxMax[i], "")
		}
	}

	if snap.OvertakeGenAvg > 0 || snap.OvertakeGenMax > 0 {
		add("Overtake generator", "", snap.OvertakeGenAvg, snap.OvertakeGenMax, "")
	}
	if snap.OvertakeFxAvg > 0 || snap.OvertakeFxMax > 0 {
		add("Overtake FX", "", snap.OvertakeFxAvg, snap.OvertakeFxMax, "")
	}

	for i, name := range perfSectionNames {
		if i >= perfSectionCount {
			break
		}
		if snap.SectionAvg[i] == 0 && snap.SectionMax[i] == 0 {
			continue
		}
		note := ""
		if name == "Process MIDI (incl. MIDI FX)" {
			note = "MIDI FX run event-driven inside the chain host, so their " +
				"cost lands here and is not separable per module."
		}
		add(name, "", snap.SectionAvg[i], snap.SectionMax[i], note)
	}

	sort.SliceStable(rows, func(i, j int) bool { return rows[i].AvgUs > rows[j].AvgUs })
	return rows
}

// describePerfError turns a failed read into a sentence.
//
// Every branch says something DIFFERENT, on purpose. "the shim is not
// running", "the shim is older than this manager", "the segment is not ours"
// and "the writer never settled" are four separate findings with four separate
// fixes, and none of them is "the device is idle". A failed read must NEVER be
// rendered as zeros — a 0% bar would tell the same story for all of them and
// for a genuinely quiet device too.
func describePerfError(err error) string {
	if err == nil {
		return ""
	}

	var ve *PerfVersionError
	if errors.As(err, &ve) {
		// Already names both versions and the deploy action.
		return ve.Error()
	}

	switch {
	case errors.Is(err, ErrPerfAbsent):
		return "The shim is not running, or this is not a Move. No frame-budget " +
			"data is available — this is not the same as an idle device."
	case errors.Is(err, ErrPerfMagic):
		return "/schwung-perf holds something unexpected. Restart the shim so " +
			"the segment is recreated."
	case errors.Is(err, ErrPerfTorn):
		return "The snapshot did not settle across three reads. Try again in a moment."
	}
	return "Frame-budget read failed: " + err.Error()
}

// moduleID is what a module-identity read produced, keeping the THREE answers
// apart: a name, "this position is empty", and "the read did not complete".
//
// Collapsing the last two is a real hazard here. The param channel is a single
// slot shared with the shadow UI, so a read can legitimately fail to be served
// — and if a failed read is treated as "empty", a slot that is BURNING CPU
// disappears from the page entirely, because the row is drawn only where a
// module id was found. On a page whose whole job is finding what costs time,
// silently hiding a busy slot is the worst failure available.
type moduleID struct {
	Name     string
	Answered bool // the channel served us, whatever it said
}

// Loaded reports whether a module is actually there.
func (m moduleID) Loaded() bool { return m.Answered && m.Name != "" }

// moduleIDCache holds module identities between polls.
//
// These reads are not free: each one is a request the SHIM serves on the SPI
// callback. Twelve of them per second (4 slots + 8 Master FX) measurably moved
// the `Param requests` section — its max went from ~36 us to ~140 us once this
// page started polling, which is the instrument perturbing the thing it is
// measuring. Module identity changes only when someone loads or swaps one, so
// it is re-read on a slow cadence and reused in between.
type moduleIDCache struct {
	mu     sync.Mutex
	slots  map[int]moduleID
	mfx    map[int]moduleID
	readAt time.Time
}

// moduleIDRefresh is how often identities are re-read. Long against the 1 s
// poll: a module swap shows up within this, and in exchange the page stops
// adding twelve SPI-served requests every second.
const moduleIDRefresh = 15 * time.Second

// moduleIDs returns slot and Master FX identities, re-reading them only when
// the cache has gone stale.
func (app *App) moduleIDs() (slots, mfx map[int]moduleID) {
	app.moduleIDs_.mu.Lock()
	defer app.moduleIDs_.mu.Unlock()

	fresh := app.moduleIDs_.slots != nil &&
		time.Since(app.moduleIDs_.readAt) < moduleIDRefresh
	if fresh {
		return app.moduleIDs_.slots, app.moduleIDs_.mfx
	}

	slots = make(map[int]moduleID, perfChainSlots)
	mfx = make(map[int]moduleID, perfMasterFXSlots)

	if app.shmParams != nil {
		for slot := 0; slot < perfChainSlots; slot++ {
			val, ok, err := app.shmParams.TryGetParam(uint8(slot), "synth_module")
			slots[slot] = moduleID{Name: val, Answered: ok && err == nil}
		}
		for i := 0; i < perfMasterFXSlots; i++ {
			key := "master_fx:" + strconv.Itoa(i) + ":module"
			val, ok, err := app.shmParams.TryGetParam(0, key)
			mfx[i] = moduleID{Name: val, Answered: ok && err == nil}
		}
	}

	app.moduleIDs_.slots = slots
	app.moduleIDs_.mfx = mfx
	app.moduleIDs_.readAt = time.Now()
	return slots, mfx
}

// --- handlers ---------------------------------------------------------------

// perfSegment returns the mapped snapshot segment, attaching lazily.
//
// The manager and the shim start independently and the manager can WIN.
// Measured on the device after a reboot: the manager came up at 07:17:57.799
// and the shim created /schwung-perf at 07:17:58.103 - 304 ms later. A single
// OpenPerfShm() at construction therefore returned nil for the life of the
// process, and the page reported "the shim is not running" about a shim that
// was running fine.
//
// That is a WRONG finding, not a missing one, which makes it worse than the
// zeros this page exists to avoid: it does not fail to answer, it answers
// incorrectly and with confidence. The producer already retries until the
// segment exists (perf_shm_attach_tick in src/host/shim_worker.c); the consumer
// has to do the same. Once attached this is a nil check.
func (app *App) perfSegment() *PerfShm {
	app.perfMu.Lock()
	defer app.perfMu.Unlock()
	if app.perfShm == nil {
		app.perfShm = OpenPerfShm()
	}
	return app.perfShm
}

// handleSystemCPU renders the page shell only. It samples NOTHING: the first
// sample must be taken by the values endpoint so that the second one has a
// measured interval behind it rather than "however long the user looked at the
// page before it started polling".
func (app *App) handleSystemCPU(w http.ResponseWriter, r *http.Request) {
	app.render(w, r, "system_cpu.html", map[string]any{
		"Title":           "CPU",
		"Active":          "system",
		"FramePeriodUs":   nominalFramePeriodUs,
		"PollIntervalSec": 1,
	})
}

// handleSystemCPUValues is the polled fragment: one shm read, one /proc scan,
// one delta.
func (app *App) handleSystemCPUValues(w http.ResponseWriter, r *http.Request) {
	var snap *PerfSnapshot
	var perfErr error
	if shm := app.perfSegment(); shm == nil {
		perfErr = ErrPerfAbsent
	} else {
		snap, perfErr = shm.Read()
	}

	var budget []FrameBudgetRow
	if perfErr == nil {
		slotMods, mfxMods := app.moduleIDs()
		budget = buildFrameBudget(snap, slotMods, mfxMods)
	}

	// An empty scan is a FAILED READ, not a machine with no processes: there is
	// always at least this one. Distinguishing them matters because the
	// always-listed rows would otherwise report five processes as "not
	// running" when the truth is that we could not look.
	procs := scanProcesses()
	procsOK := len(procs) > 0
	view := app.cpuSampler.buildProcessView(procs, time.Now())

	// The ok flags are NOT discarded. /proc/stat and /proc/loadavg can fail —
	// they do not exist off Linux at all — and a discarded flag renders as
	// "Load average: 0.00 / 0.00 / 0.00" plus a table of headings with no
	// rows: a failed read wearing the costume of a real measurement. That is
	// the one thing this page must never do, and it is the same rule the
	// seqlock and the priming state follow.
	cores, coresOK := readCPUStat()
	load1, load5, load15, loadOK := readLoadAvg()

	// Realtime threads live inside MoveOriginal — that is where a module's
	// pthread_create from an entry point inherits FIFO 70 from the SPI
	// callback and starves Move's own Link Main at 35.
	var rtThreads []ProcStat
	moveFound := false
	for _, p := range procs {
		if p.Comm != "MoveOriginal" {
			continue
		}
		moveFound = true
		for _, t := range scanThreads(p.PID) {
			if t.IsRealtime() {
				rtThreads = append(rtThreads, t)
			}
		}
		break
	}
	sort.SliceStable(rtThreads, func(i, j int) bool {
		return rtThreads[i].RTPrio > rtThreads[j].RTPrio
	})

	app.renderPartial(w, r, "system_cpu.html", "cpu_values", map[string]any{
		"Budget":    budget,
		"PerfError": describePerfError(perfErr),
		"Snapshot":  snap,
		"Process":   view,
		"ProcessOK": procsOK,
		"Cores":     cores,
		"CoresOK":   coresOK,
		"Load1":     load1,
		"Load5":     load5,
		"Load15":    load15,
		"LoadOK":    loadOK,
		"RTThreads": rtThreads,
		// Without this, an absent MoveOriginal renders as "None found" — a
		// finding we never made, from a scan that never ran.
		"MoveFound": moveFound,
		"SPICore":   3,
		"Measuring": true,
	})
}

// handleSystemCPUIdle returns the page to its not-measuring state.
//
// The reset matters. A predecessor left over from before the pause would make
// the first sample after resuming average the delta across the ENTIRE gap —
// a number that looks perfectly plausible and is wrong by whatever fraction of
// the gap the user spent away. Priming again costs one poll and cannot lie.
func (app *App) handleSystemCPUIdle(w http.ResponseWriter, r *http.Request) {
	app.cpuSampler.reset()
	app.renderPartial(w, r, "system_cpu.html", "cpu_idle", map[string]any{
		"FramePeriodUs":   nominalFramePeriodUs,
		"PollIntervalSec": 1,
	})
}
