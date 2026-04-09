package main

import (
	"fmt"
	"os"
	"sync"
	"syscall"
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
const (
	webRingOffWriteIdx = 0 // uint8
	webRingOffReady    = 1 // uint8
	// reserved[2] at 2-3

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

	idx := int(r.data[webRingOffWriteIdx])
	if idx >= webMaxEntries {
		return fmt.Errorf("web param set ring full")
	}

	// Write entry
	entryOff := webEntryStart + idx*webEntrySize
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

	// Increment write_idx and toggle ready
	r.data[webRingOffWriteIdx] = uint8(idx + 1)
	r.data[webRingOffReady]++

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
