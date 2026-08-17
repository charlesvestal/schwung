package main

import (
	"fmt"
	"os"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// ShmWebParamSetRing provides fire-and-forget param writes via the
// /schwung-web-param-set shared memory ring buffer. The shim drains
// entries each audio block (~3ms), giving near-instant latency.
//
// Layout must match web_param_set_ring_t in shadow_constants.h.
type ShmWebParamSetRing struct {
	data []byte
	mu   sync.Mutex
}

// Offsets into web_param_set_ring_t.
//
// SPSC protocol (2026-07-19 rework — requires the paired shim): write_idx is a
// MONOTONIC uint8 producer cursor (entry slot = write_idx % 32; 256 % 32 == 0
// so the wrap is seamless) and reserved[0] (byte 2) is the shim-published
// consumer cursor. The old protocol had the shim RESET write_idx to 0 after
// draining, racing the producer's read-modify-write — an edit landing
// mid-drain was orphaned or a previous batch was re-applied (double-nudge).
// Now neither side writes the other's cursor. Header updates go through a CAS
// on the 4-byte header word: it preserves the shim's concurrent byte-2 store
// and gives release ordering so entry bytes are visible before the cursor.
const (
	webRingOffWriteIdx = 0 // uint8, monotonic producer cursor
	webRingOffReady    = 1 // uint8, legacy change signal (still bumped)
	webRingOffReadIdx  = 2 // uint8, monotonic consumer cursor (shim-published)
	// reserved byte at 3

	webEntryStart  = 4 // first entry starts at byte 4
	webEntrySlot   = 0 // uint8 at offset 0 within entry
	// reserved[3] at 1-3
	webEntryKey    = 4   // char[64]
	webEntryValue  = 68  // char[256]
	webEntrySize   = 324 // 4 + 64 + 256

	webMaxEntries = 32
	webKeyLen     = 64
	webValueLen   = 256
	webRingSize   = 4 + webMaxEntries*webEntrySize // header + entries
)

const shmWebParamSetPath = "/dev/shm/schwung-web-param-set"

// OpenShmWebParamSetRing opens the web param set ring buffer.
// Returns nil if the segment doesn't exist.
func OpenShmWebParamSetRing() *ShmWebParamSetRing {
	f, err := os.OpenFile(shmWebParamSetPath, os.O_RDWR, 0)
	if err != nil {
		return nil
	}
	defer f.Close()

	data, err := syscall.Mmap(int(f.Fd()), 0, webRingSize,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return nil
	}

	return &ShmWebParamSetRing{data: data}
}

// SetParam writes a set request into the ring buffer. Fire-and-forget:
// returns immediately, shim processes on next audio block (~3ms).
func (r *ShmWebParamSetRing) SetParam(slot uint8, key, value string) error {
	if len(key) >= webKeyLen {
		return fmt.Errorf("key too long (%d >= %d)", len(key), webKeyLen)
	}
	if len(value) >= webValueLen {
		return fmt.Errorf("value too long (%d >= %d)", len(value), webValueLen)
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	hdr := (*uint32)(unsafe.Pointer(&r.data[0])) // 4-byte header word, offset 0 (aligned)
	old := atomic.LoadUint32(hdr)
	head := uint8(old)       // byte 0 (little-endian ARM64) = write_idx
	tail := uint8(old >> 16) // byte 2 = shim-published read_idx
	if head-tail >= webMaxEntries {
		// Consumer hasn't caught up (or an unpaired old shim never publishes
		// read_idx) — report full; the caller falls back to the mailbox.
		return fmt.Errorf("web param set ring full")
	}

	// Write entry at the monotonic slot
	entryOff := webEntryStart + (int(head)%webMaxEntries)*webEntrySize
	r.data[entryOff+webEntrySlot] = slot

	// Write key (null-terminated)
	keyOff := entryOff + webEntryKey
	for i := 0; i < webKeyLen; i++ {
		r.data[keyOff+i] = 0
	}
	copy(r.data[keyOff:], key)

	// Write value (null-terminated)
	valOff := entryOff + webEntryValue
	for i := 0; i < webValueLen; i++ {
		r.data[valOff+i] = 0
	}
	copy(r.data[valOff:], value)

	// Publish: advance write_idx + bump ready via CAS on the header word —
	// preserves the shim's concurrent read_idx byte and orders the entry
	// stores before the cursor becomes visible.
	for {
		old = atomic.LoadUint32(hdr)
		ready := uint8(old >> 8)
		newHdr := (old &^ 0x0000FFFF) | uint32(head+1) | uint32(ready+1)<<8
		if atomic.CompareAndSwapUint32(hdr, old, newHdr) {
			break
		}
	}
	return nil
}

// =========================================================================
// ShmWebParamNotifyRing — reads param change notifications from the shim
// =========================================================================

// ShmWebParamNotifyRing reads the notify ring that the shim writes to
// whenever a param changes. Provides push-based updates for the web UI.
//
// Layout must match web_param_notify_ring_t in shadow_constants.h.
type ShmWebParamNotifyRing struct {
	data      []byte
	lastReady uint8
}

const (
	notifyMaxEntries = 64
	notifyEntrySize  = webEntrySize // same layout as set entries
	notifyRingSize   = 4 + notifyMaxEntries*notifyEntrySize
)

const shmWebParamNotifyPath = "/dev/shm/schwung-web-param-notify"

// ParamChange represents a single param value change from the shim.
type ParamChange struct {
	Slot  uint8
	Key   string
	Value string
}

// OpenShmWebParamNotifyRing opens the notify ring. Returns nil if not available.
func OpenShmWebParamNotifyRing() *ShmWebParamNotifyRing {
	f, err := os.OpenFile(shmWebParamNotifyPath, os.O_RDWR, 0)
	if err != nil {
		return nil
	}
	defer f.Close()

	data, err := syscall.Mmap(int(f.Fd()), 0, notifyRingSize,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return nil
	}

	return &ShmWebParamNotifyRing{data: data, lastReady: data[webRingOffReady]}
}

// Drain reads all pending notifications and resets the ring.
// Returns nil if no new data. Non-blocking.
func (r *ShmWebParamNotifyRing) Drain() []ParamChange {
	ready := r.data[webRingOffReady]
	if ready == r.lastReady {
		return nil // no new data
	}
	r.lastReady = ready

	count := int(r.data[webRingOffWriteIdx])
	if count <= 0 || count > notifyMaxEntries {
		r.data[webRingOffWriteIdx] = 0
		return nil
	}

	// Read entries
	changes := make([]ParamChange, 0, count)
	for i := 0; i < count; i++ {
		entryOff := webEntryStart + i*notifyEntrySize
		slot := r.data[entryOff+webEntrySlot]

		keyOff := entryOff + webEntryKey
		key := cString(r.data[keyOff : keyOff+webKeyLen])

		valOff := entryOff + webEntryValue
		value := cString(r.data[valOff : valOff+webValueLen])

		if key != "" {
			changes = append(changes, ParamChange{Slot: slot, Key: key, Value: value})
		}
	}

	// Reset ring
	r.data[webRingOffWriteIdx] = 0

	return changes
}

// cString extracts a null-terminated string from a byte slice.
func cString(b []byte) string {
	for i, c := range b {
		if c == 0 {
			return string(b[:i])
		}
	}
	return string(b)
}
