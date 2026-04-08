package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// ShmParams provides access to the shadow_param_t shared memory segment for
// getting and setting module parameters. The protocol is request/response:
// only one request can be in-flight at a time (serialised by the Go mutex,
// with a wait-for-idle loop to handle contention with the JS shadow UI).
//
// Field offsets must match shadow_param_t in src/host/shadow_constants.h.
type ShmParams struct {
	data      []byte
	mu        sync.Mutex
	nextReqID atomic.Uint32
}

// Byte offsets into shadow_param_t.
// Struct layout (ARM64, packed uint8 fields then naturally aligned uint32):
//
//	uint8_t  request_type   @ 0
//	uint8_t  slot           @ 1
//	uint8_t  response_ready @ 2
//	uint8_t  error          @ 3
//	uint32_t request_id     @ 4
//	uint32_t response_id    @ 8
//	int32_t  result_len     @ 12
//	char     key[64]        @ 16
//	char     value[65536]   @ 80
const (
	paramOffRequestType   = 0
	paramOffSlot          = 1
	paramOffResponseReady = 2
	paramOffError         = 3
	paramOffRequestID     = 4
	paramOffResponseID    = 8
	paramOffResultLen     = 12
	paramOffKey           = 16
	paramOffValue         = 80

	paramKeyLen   = 64
	paramValueLen = 65536

	// SHADOW_PARAM_BUFFER_SIZE from shadow_constants.h
	shmParamSize = 65664

	// Timeouts — kept short to avoid blocking the poll loop when
	// contending with shadow_ui.js for the single param channel.
	paramIdleTimeout     = 200 * time.Millisecond
	paramResponseTimeout = 500 * time.Millisecond
	paramPollInterval    = 500 * time.Microsecond
)

const shmParamPath = "/dev/shm/schwung-param"

// OpenShmParams opens and mmaps the param shared memory segment.
// Returns nil if the segment doesn't exist (not running on device).
func OpenShmParams() *ShmParams {
	f, err := os.OpenFile(shmParamPath, os.O_RDWR, 0)
	if err != nil {
		return nil
	}
	defer f.Close()

	data, err := syscall.Mmap(int(f.Fd()), 0, shmParamSize,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return nil
	}

	return &ShmParams{data: data}
}

// TryGetParam is like GetParam but returns immediately if the mutex is held
// (e.g., by a concurrent SetParam from user interaction). Used by the poll loop
// so background polling never blocks user-initiated param changes.
func (s *ShmParams) TryGetParam(slot uint8, key string) (string, bool, error) {
	if !s.mu.TryLock() {
		return "", false, nil // busy, skip
	}
	defer s.mu.Unlock()

	if len(key) >= paramKeyLen {
		return "", true, fmt.Errorf("key too long (%d >= %d)", len(key), paramKeyLen)
	}

	if err := s.waitIdle(); err != nil {
		return "", true, err
	}

	reqID := s.nextReqID.Add(1)

	s.data[paramOffSlot] = slot
	s.data[paramOffResponseReady] = 0
	s.data[paramOffError] = 0
	binary.LittleEndian.PutUint32(s.data[paramOffRequestID:], reqID)

	copy(s.data[paramOffKey:paramOffKey+paramKeyLen], make([]byte, paramKeyLen))
	copy(s.data[paramOffKey:], key)

	s.data[paramOffRequestType] = 2

	if err := s.waitResponse(reqID); err != nil {
		s.data[paramOffRequestType] = 0
		return "", true, err
	}

	if s.data[paramOffError] != 0 {
		s.data[paramOffRequestType] = 0
		return "", true, fmt.Errorf("param get error (slot=%d key=%q)", slot, key)
	}

	resultLen := int32(binary.LittleEndian.Uint32(s.data[paramOffResultLen:]))
	if resultLen < 0 {
		s.data[paramOffRequestType] = 0
		return "", true, fmt.Errorf("param get failed (result_len=%d)", resultLen)
	}
	if int(resultLen) > paramValueLen {
		resultLen = int32(paramValueLen)
	}

	value := string(s.data[paramOffValue : paramOffValue+int(resultLen)])
	s.data[paramOffRequestType] = 0
	return value, true, nil
}

// SetParamFast writes a param without waiting for the shim's response.
// Latency: ~5ms idle wait + ~3ms shim processing = ~8ms total.
// Safe for knob dragging where the next value overwrites the previous.
func (s *ShmParams) SetParamFast(slot uint8, key, value string) error {
	if len(key) >= paramKeyLen {
		return fmt.Errorf("key too long (%d >= %d)", len(key), paramKeyLen)
	}
	if len(value) >= paramValueLen {
		return fmt.Errorf("value too long (%d >= %d)", len(value), paramValueLen)
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	// Short idle wait — if shadow_ui.js is mid-request, bail quickly.
	deadline := time.Now().Add(10 * time.Millisecond)
	for s.data[paramOffRequestType] != 0 {
		if time.Now().After(deadline) {
			return fmt.Errorf("param channel busy")
		}
		time.Sleep(paramPollInterval)
	}

	reqID := s.nextReqID.Add(1)

	s.data[paramOffSlot] = slot
	s.data[paramOffResponseReady] = 0
	s.data[paramOffError] = 0
	binary.LittleEndian.PutUint32(s.data[paramOffRequestID:], reqID)

	copy(s.data[paramOffKey:paramOffKey+paramKeyLen], make([]byte, paramKeyLen))
	copy(s.data[paramOffKey:], key)

	copy(s.data[paramOffValue:paramOffValue+len(value)], value)
	s.data[paramOffValue+len(value)] = 0

	// Fire and forget — shim processes on next audio block (~3ms).
	s.data[paramOffRequestType] = 1
	return nil
}

// GetParam reads a parameter from the given chain slot.
func (s *ShmParams) GetParam(slot uint8, key string) (string, error) {
	if len(key) >= paramKeyLen {
		return "", fmt.Errorf("key too long (%d >= %d)", len(key), paramKeyLen)
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	if err := s.waitIdle(); err != nil {
		return "", err
	}

	reqID := s.nextReqID.Add(1)

	// Write fields (request_type last to signal the request).
	s.data[paramOffSlot] = slot
	s.data[paramOffResponseReady] = 0
	s.data[paramOffError] = 0
	binary.LittleEndian.PutUint32(s.data[paramOffRequestID:], reqID)

	// Write null-terminated key.
	copy(s.data[paramOffKey:paramOffKey+paramKeyLen], make([]byte, paramKeyLen))
	copy(s.data[paramOffKey:], key)

	// Signal: get request.
	s.data[paramOffRequestType] = 2

	// Wait for response.
	if err := s.waitResponse(reqID); err != nil {
		s.data[paramOffRequestType] = 0 // clean up
		return "", err
	}

	// Check error flag.
	if s.data[paramOffError] != 0 {
		s.data[paramOffRequestType] = 0
		return "", fmt.Errorf("param get error (slot=%d key=%q)", slot, key)
	}

	// Read result.
	resultLen := int32(binary.LittleEndian.Uint32(s.data[paramOffResultLen:]))
	if resultLen < 0 {
		s.data[paramOffRequestType] = 0
		return "", fmt.Errorf("param get failed (result_len=%d)", resultLen)
	}
	if int(resultLen) > paramValueLen {
		resultLen = int32(paramValueLen)
	}

	value := string(s.data[paramOffValue : paramOffValue+int(resultLen)])

	s.data[paramOffRequestType] = 0
	return value, nil
}

// SetParam writes a parameter to the given chain slot.
func (s *ShmParams) SetParam(slot uint8, key, value string) error {
	if len(key) >= paramKeyLen {
		return fmt.Errorf("key too long (%d >= %d)", len(key), paramKeyLen)
	}
	if len(value) >= paramValueLen {
		return fmt.Errorf("value too long (%d >= %d)", len(value), paramValueLen)
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	if err := s.waitIdle(); err != nil {
		return err
	}

	reqID := s.nextReqID.Add(1)

	// Write fields (request_type last to signal the request).
	s.data[paramOffSlot] = slot
	s.data[paramOffResponseReady] = 0
	s.data[paramOffError] = 0
	binary.LittleEndian.PutUint32(s.data[paramOffRequestID:], reqID)

	// Write null-terminated key.
	copy(s.data[paramOffKey:paramOffKey+paramKeyLen], make([]byte, paramKeyLen))
	copy(s.data[paramOffKey:], key)

	// Write null-terminated value.
	copy(s.data[paramOffValue:paramOffValue+len(value)], value)
	s.data[paramOffValue+len(value)] = 0

	// Signal: set request.
	s.data[paramOffRequestType] = 1

	// Wait for response.
	if err := s.waitResponse(reqID); err != nil {
		s.data[paramOffRequestType] = 0 // clean up
		return err
	}

	if s.data[paramOffError] != 0 {
		s.data[paramOffRequestType] = 0
		return fmt.Errorf("param set error (slot=%d key=%q)", slot, key)
	}

	s.data[paramOffRequestType] = 0
	return nil
}

// waitIdle spins until request_type == 0, indicating the channel is free.
// Must be called with s.mu held.
func (s *ShmParams) waitIdle() error {
	deadline := time.Now().Add(paramIdleTimeout)
	for s.data[paramOffRequestType] != 0 {
		if time.Now().After(deadline) {
			return fmt.Errorf("param channel busy (timeout waiting for idle)")
		}
		time.Sleep(paramPollInterval)
	}
	return nil
}

// waitResponse spins until response_ready != 0 and response_id matches reqID.
// Must be called with s.mu held.
func (s *ShmParams) waitResponse(reqID uint32) error {
	deadline := time.Now().Add(paramResponseTimeout)
	for {
		if s.data[paramOffResponseReady] != 0 {
			respID := binary.LittleEndian.Uint32(s.data[paramOffResponseID:])
			if respID == reqID {
				return nil
			}
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("param response timeout (reqID=%d)", reqID)
		}
		time.Sleep(paramPollInterval)
	}
}
