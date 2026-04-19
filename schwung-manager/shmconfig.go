package main

import (
	"encoding/binary"
	"math"
	"os"
	"sync"
	"syscall"
)

// ShmConfig provides direct read/write access to the shadow_control_t shared
// memory segment. This allows the web server to apply settings changes
// instantly without going through the JS tick() path (which causes SIGABRT).
//
// Field offsets must match shadow_control_t in src/host/shadow_constants.h.
type ShmConfig struct {
	data []byte
	mu   sync.Mutex
}

// Byte offsets into shadow_control_t (64 bytes total).
// Derived from the C struct layout (ARM64, naturally aligned).
const (
	offDisplayMode    = 0  // uint8
	offShadowReady    = 1  // uint8
	offShouldExit     = 2  // uint8
	offMidiReady      = 3  // uint8
	offWriteIdx       = 4  // uint8
	offReadIdx        = 5  // uint8
	offUISlot         = 6  // uint8
	offUIFlags        = 7  // uint8
	offUIPatchIndex   = 8  // uint16
	offReserved16     = 10 // uint16
	offUIRequestID    = 12 // uint32
	offShimCounter    = 16 // uint32
	offSelectedSlot   = 20 // uint8
	offShiftHeld      = 21 // uint8
	offOvertakeMode   = 22 // uint8
	offRestartMove    = 23 // uint8
	offTTSEnabled     = 24 // uint8
	offTTSVolume      = 25 // uint8
	offTTSPitch       = 26 // uint16
	offTTSSpeed       = 28 // float32
	offOverlayKnobs   = 32 // uint8
	offDisplayMirror  = 33 // uint8
	offTTSEngine      = 34 // uint8
	offPinChallenge   = 35 // uint8
	offDisplayOverlay = 36 // uint8
	offOverlayRectX   = 37 // uint8
	offOverlayRectY   = 38 // uint8
	offOverlayRectW   = 39 // uint8
	offOverlayRectH   = 40 // uint8
	offTTSDebounce    = 42 // uint16 (aligned to 2)
	offSetPages       = 44 // uint8
	// ... more fields follow but not needed for config
	offSkipbackReqVol  = 52 // uint8
	offOpenToolCmd     = 56 // uint8 — 0=none, 1=open tool
	offSkipbackSeconds = 60 // uint16 — 30/60/120/180/240/300
	shmControlSize     = 64
)

const shmPath = "/dev/shm/schwung-control"

// OpenShmConfig opens and mmaps the shared memory control segment.
// Returns nil if the segment doesn't exist (not running on device).
func OpenShmConfig() *ShmConfig {
	f, err := os.OpenFile(shmPath, os.O_RDWR, 0)
	if err != nil {
		return nil
	}
	defer f.Close()

	data, err := syscall.Mmap(int(f.Fd()), 0, shmControlSize,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return nil
	}

	return &ShmConfig{data: data}
}

func (s *ShmConfig) getU8(off int) uint8 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.data[off]
}

func (s *ShmConfig) setU8(off int, v uint8) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.data[off] = v
}

func (s *ShmConfig) getU16(off int) uint16 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return binary.LittleEndian.Uint16(s.data[off:])
}

func (s *ShmConfig) setU16(off int, v uint16) {
	s.mu.Lock()
	defer s.mu.Unlock()
	binary.LittleEndian.PutUint16(s.data[off:], v)
}

func (s *ShmConfig) getF32(off int) float32 {
	s.mu.Lock()
	defer s.mu.Unlock()
	bits := binary.LittleEndian.Uint32(s.data[off:])
	return math.Float32frombits(bits)
}

func (s *ShmConfig) setF32(off int, v float32) {
	s.mu.Lock()
	defer s.mu.Unlock()
	binary.LittleEndian.PutUint32(s.data[off:], math.Float32bits(v))
}

// --- Public accessors for config settings ---

func (s *ShmConfig) DisplayMirror() bool      { return s.getU8(offDisplayMirror) != 0 }
func (s *ShmConfig) SetDisplayMirror(v bool)   { s.setU8(offDisplayMirror, boolU8(v)) }

func (s *ShmConfig) OverlayKnobsMode() uint8  { return s.getU8(offOverlayKnobs) }
func (s *ShmConfig) SetOverlayKnobsMode(v uint8) { s.setU8(offOverlayKnobs, v) }

func (s *ShmConfig) TTSEnabled() bool          { return s.getU8(offTTSEnabled) != 0 }
func (s *ShmConfig) SetTTSEnabled(v bool)      { s.setU8(offTTSEnabled, boolU8(v)) }

func (s *ShmConfig) TTSEngine() uint8          { return s.getU8(offTTSEngine) }
func (s *ShmConfig) SetTTSEngine(v uint8)      { s.setU8(offTTSEngine, v) }

func (s *ShmConfig) TTSSpeed() float32         { return s.getF32(offTTSSpeed) }
func (s *ShmConfig) SetTTSSpeed(v float32)     { s.setF32(offTTSSpeed, v) }

func (s *ShmConfig) TTSPitch() uint16          { return s.getU16(offTTSPitch) }
func (s *ShmConfig) SetTTSPitch(v uint16)      { s.setU16(offTTSPitch, v) }

func (s *ShmConfig) TTSVolume() uint8          { return s.getU8(offTTSVolume) }
func (s *ShmConfig) SetTTSVolume(v uint8)      { s.setU8(offTTSVolume, v) }

func (s *ShmConfig) TTSDebounce() uint16       { return s.getU16(offTTSDebounce) }
func (s *ShmConfig) SetTTSDebounce(v uint16)   { s.setU16(offTTSDebounce, v) }

func (s *ShmConfig) SetPagesEnabled() bool     { return s.getU8(offSetPages) != 0 }
func (s *ShmConfig) SetSetPagesEnabled(v bool) { s.setU8(offSetPages, boolU8(v)) }

func (s *ShmConfig) SkipbackRequireVolume() bool     { return s.getU8(offSkipbackReqVol) != 0 }
func (s *ShmConfig) SetSkipbackRequireVolume(v bool)  { s.setU8(offSkipbackReqVol, boolU8(v)) }

func (s *ShmConfig) SkipbackSeconds() uint16          { return s.getU16(offSkipbackSeconds) }
func (s *ShmConfig) SetSkipbackSeconds(v uint16)      { s.setU16(offSkipbackSeconds, v) }

func (s *ShmConfig) SetOpenToolCmd(v uint8) { s.setU8(offOpenToolCmd, v) }

func boolU8(v bool) uint8 {
	if v {
		return 1
	}
	return 0
}
