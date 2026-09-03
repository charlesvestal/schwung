package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
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
	// Set when we commit a request whose response we will never read
	// (SetParamFast). Nobody will consume that response, so the next claim may
	// bin it immediately rather than wait out paramStealAfter. Guarded by mu.
	orphan bool
}

// Byte offsets into shadow_param_t.
// Struct layout (ARM64, packed uint8 fields then naturally aligned uint32/64):
//
//	uint8_t  request_type   @ 0
//	uint8_t  slot           @ 1
//	uint8_t  response_ready @ 2
//	uint8_t  error          @ 3
//	uint32_t request_id     @ 4
//	uint32_t response_id    @ 8
//	int32_t  result_len     @ 12
//	uint64_t trace_id       @ 16   (OTLP trace context, Phase 2b)
//	uint64_t parent_span_id @ 24   (OTLP trace context, Phase 2b)
//	char     key[64]        @ 32
//	char     value[65536]   @ 96
//
// NOTE: the two uint64 trace fields were added to shadow_param_t by the OTLP
// trace work (5a5aa645) and pushed key/value down by 16 bytes. This Go mirror
// must track that or every request's key lands at the wrong offset, the shim
// reads an empty key, and every GET returns empty (remote UI shows "no module"
// and default slot params).
const (
	paramOffRequestType   = 0
	paramOffSlot          = 1
	paramOffResponseReady = 2
	paramOffError         = 3
	paramOffRequestID     = 4
	paramOffResponseID    = 8
	paramOffResultLen     = 12
	paramOffTraceID       = 16
	paramOffParentSpanID  = 24
	paramOffKey           = 32
	paramOffValue         = 96

	paramKeyLen   = 64
	paramValueLen = 65536

	// SHADOW_PARAM_BUFFER_SIZE from shadow_constants.h
	shmParamSize = 65664

	// Timeouts — kept short to avoid blocking the poll loop when
	// contending with shadow_ui.js for the single param channel.
	paramIdleTimeout     = 200 * time.Millisecond
	paramResponseTimeout = 500 * time.Millisecond
	paramPollInterval    = 500 * time.Microsecond

	// SHADOW_PARAM_CLAIMED from shadow_constants.h. Written by a
	// compare-and-swap to take the channel; see claim().
	paramClaimed = 0xFF

	// SHADOW_PARAM_WEB_REQ_ID_BASE from shadow_constants.h.
	//
	// Request IDs MUST NOT overlap with shadow_ui's, which counts up from 1.
	// waitResponse matches on response_id == reqID, so overlapping ranges let
	// this process match the shadow UI's response and read ITS value —
	// silently wrong data rather than an error. The shim already documents
	// this split ("web-originated requests use req_id >= 0xFFFF0000") and
	// keys its skip-re-notify check on it, but nextReqID started at 0, so
	// both processes were minting 1, 2, 3, ... and the invariant was never
	// actually held.
	paramWebReqIDBase = 0xFFFF0000

	// response_ready is byte 2 of the 32-bit word at offset 0.
	paramRespReadyMask = uint32(0x00FF0000)

	// Discard an unread response older than this; only reachable if a
	// client died between being answered and consuming its answer.
	// Comfortably longer than any request deadline.
	paramStealAfter = 250 * time.Millisecond
)

// A var, not a const, so a test can point it at a temp file.
var shmParamPath = "/dev/shm/schwung-param"

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

	s := &ShmParams{data: data}
	// Start above the shadow UI's range so the two processes can never mint
	// the same request id. See paramWebReqIDBase.
	s.nextReqID.Store(paramWebReqIDBase)
	return s
}

// claim takes the param channel with an atomic compare-and-swap.
//
// The channel is a single slot shared with the shadow_ui process, and the old
// sequence — spin until request_type reads 0, then write it — is a
// time-of-check/time-of-use race: both processes can observe idle in the same
// window, both write, and the loser spins for a response_id that never
// arrives until its timeout expires. Measured on device, that cost the shadow
// UI one failed read per second, each blocking it for 100-200ms and returning
// null to its caller.
//
// Claiming with a CAS makes observe-and-take indivisible. paramClaimed is a
// value the servicer knows to skip, because the key and request id are not
// written yet.
//
// Go has no byte-wide CAS, and request_type shares its 32-bit word with
// slot / response_ready / error — which the servicer writes — so a plain
// 32-bit CAS would silently stomp them. Instead CAS the whole word,
// requiring byte 0 to be zero and carrying the other three bytes through
// unchanged. If any of them moved under us the CAS simply fails and we
// retry, which is exactly the behaviour we want.
//
// Must be called with s.mu held.
func (s *ShmParams) claim() error { return s.claimFor(paramIdleTimeout) }

func (s *ShmParams) claimFor(timeout time.Duration) error {
	word := (*uint32)(unsafe.Pointer(&s.data[0]))
	deadline := time.Now().Add(timeout)
	stealAt := time.Now().Add(paramStealAfter)
	for {
		w := atomic.LoadUint32(word)
		// Free means idle AND no unread response. The protocol had no
		// acknowledgement step: the shim sets response_ready=1 and clears
		// request_type in the same publish, and this claim used to look only
		// at request_type and then zero response_ready — destroying an answer
		// the shadow UI was still waiting for, which then blocked for its full
		// 100ms deadline and returned null. Measured at ~1/sec on device, and
		// it is the UI's remaining stall. Byte 2 of this word is
		// response_ready; the owner clears it once it has copied its value out.
		if w&0xFF == 0 && (w&paramRespReadyMask == 0 || s.orphan) {
			if atomic.CompareAndSwapUint32(word, w,
				(w&^(uint32(0xFF)|paramRespReadyMask))|paramClaimed) {
				s.orphan = false
				return nil
			}
			// Lost the word to a concurrent field write; re-read and retry
			// without burning the deadline.
			continue
		}
		// Idle but holding an unread response: if it has sat there far longer
		// than any request deadline its owner is gone, so take it rather than
		// wedge every param access on the device forever.
		if w&0xFF == 0 && time.Now().After(stealAt) {
			if atomic.CompareAndSwapUint32(word, w,
				(w&^(uint32(0xFF)|paramRespReadyMask))|paramClaimed) {
				return nil
			}
			continue
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("param channel busy (timeout waiting for idle)")
		}
		time.Sleep(paramPollInterval)
	}
}

// commit publishes the request type, releasing the claim taken by claim().
//
// An ATOMIC store, not a plain byte write: it is the barrier that guarantees
// the key, slot and request_id written above are visible to the shim before
// it observes a servable request_type. The shim's side of this pair is an
// __ATOMIC_ACQUIRE load. A plain store carries no such ordering on ARM64, and
// the shim would be free to read a stale key for a fresh request.
//
// Bytes 1-3 of the word are ours while the claim is held, so carrying them
// through is safe. Must be called with s.mu held, holding a claim.
func (s *ShmParams) commit(reqType byte) {
	word := (*uint32)(unsafe.Pointer(&s.data[0]))
	w := atomic.LoadUint32(word)
	atomic.StoreUint32(word, (w&^uint32(0xFF))|uint32(reqType))
}

// release drops the claim without leaving a servable request behind.
// Must be called with s.mu held, holding a claim.
func (s *ShmParams) release() { s.commit(0) }

// consume releases the RESPONSE half of the channel, after this process has
// copied its value out. Until it is called no one may claim — which is the
// point: it is what stops the next requester destroying an answer somebody is
// still waiting for. Must be called on give-up paths too.
func (s *ShmParams) consume() {
	word := (*uint32)(unsafe.Pointer(&s.data[0]))
	for {
		w := atomic.LoadUint32(word)
		if atomic.CompareAndSwapUint32(word, w, w&^paramRespReadyMask) {
			return
		}
	}
}

// releaseIfMine drops the claim ONLY if this process still owns it.
//
// The servicer clears request_type at the same moment it publishes the
// response, so by the time we return from waitResponse the channel is
// already free — and may already have been claimed and filled in by the
// shadow UI. An unconditional clear here therefore wipes somebody else's
// in-flight request: the shim never sees it, and that process waits out its
// entire timeout for a response that will now never be generated. That was
// the second half of the param-channel bug, and the half that survived
// making the claim atomic.
//
// Checked against BOTH the request type and our own request id, because a
// concurrent request of the same type would otherwise look like ours.
// Must be called with s.mu held.
func (s *ShmParams) releaseIfMine(reqType byte, reqID uint32) {
	word := (*uint32)(unsafe.Pointer(&s.data[0]))
	for {
		w := atomic.LoadUint32(word)
		if byte(w&0xFF) != reqType {
			return // servicer already released it, or someone else owns it
		}
		if binary.LittleEndian.Uint32(s.data[paramOffRequestID:]) != reqID {
			return // still our type, but no longer our request
		}
		if atomic.CompareAndSwapUint32(word, w, w&^uint32(0xFF)) {
			return
		}
	}
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

	if err := s.claim(); err != nil {
		return "", true, err
	}

	reqID := s.nextReqID.Add(1)

	s.data[paramOffSlot] = slot
	s.data[paramOffResponseReady] = 0
	s.data[paramOffError] = 0
	binary.LittleEndian.PutUint32(s.data[paramOffRequestID:], reqID)

	copy(s.data[paramOffKey:paramOffKey+paramKeyLen], make([]byte, paramKeyLen))
	copy(s.data[paramOffKey:], key)

	s.commit(2)

	if err := s.waitResponse(reqID); err != nil {
		s.releaseIfMine(2, reqID)
		s.consume()
		return "", true, err
	}

	if s.data[paramOffError] != 0 {
		s.releaseIfMine(2, reqID)
		s.consume()
		return "", true, fmt.Errorf("param get error (slot=%d key=%q)", slot, key)
	}

	resultLen := int32(binary.LittleEndian.Uint32(s.data[paramOffResultLen:]))
	if resultLen < 0 {
		s.releaseIfMine(2, reqID)
		s.consume()
		return "", true, fmt.Errorf("param get failed (result_len=%d)", resultLen)
	}
	if int(resultLen) > paramValueLen {
		resultLen = int32(paramValueLen)
	}

	value := string(s.data[paramOffValue : paramOffValue+int(resultLen)])
	s.releaseIfMine(2, reqID)
	s.consume()
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

	// Short claim — if shadow_ui is mid-request, bail quickly rather than
	// hold up the web request. Same atomic claim as everywhere else: the old
	// inline "spin until it reads 0, then write it" here was a third copy of
	// the same time-of-check/time-of-use race.
	if err := s.claimFor(10 * time.Millisecond); err != nil {
		return fmt.Errorf("param channel busy")
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
	// We will never read the answer, so let the next claim bin it.
	s.commit(1)
	s.orphan = true
	return nil
}

// GetParam reads a parameter from the given chain slot.
func (s *ShmParams) GetParam(slot uint8, key string) (string, error) {
	if len(key) >= paramKeyLen {
		return "", fmt.Errorf("key too long (%d >= %d)", len(key), paramKeyLen)
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	if err := s.claim(); err != nil {
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
	s.commit(2)

	// Wait for response.
	if err := s.waitResponse(reqID); err != nil {
		s.releaseIfMine(2, reqID)
		s.consume()
		return "", err
	}

	// Check error flag.
	if s.data[paramOffError] != 0 {
		s.releaseIfMine(2, reqID)
		s.consume()
		return "", fmt.Errorf("param get error (slot=%d key=%q)", slot, key)
	}

	// Read result.
	resultLen := int32(binary.LittleEndian.Uint32(s.data[paramOffResultLen:]))
	if resultLen < 0 {
		s.releaseIfMine(2, reqID)
		s.consume()
		return "", fmt.Errorf("param get failed (result_len=%d)", resultLen)
	}
	if int(resultLen) > paramValueLen {
		resultLen = int32(paramValueLen)
	}

	value := string(s.data[paramOffValue : paramOffValue+int(resultLen)])

	s.releaseIfMine(2, reqID)

	s.consume()
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

	if err := s.claim(); err != nil {
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
	s.commit(1)

	// Wait for response.
	if err := s.waitResponse(reqID); err != nil {
		s.releaseIfMine(1, reqID)
		s.consume()
		return err
	}

	if s.data[paramOffError] != 0 {
		s.releaseIfMine(1, reqID)
		s.consume()
		return fmt.Errorf("param set error (slot=%d key=%q)", slot, key)
	}

	s.releaseIfMine(1, reqID)

	s.consume()
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
