// SPDX-License-Identifier: MIT
//
// schwung-host sim backend (macOS / dev).
//
// Synthesizes the SPI mailbox in-process so schwung-host can run without a
// real Move device. Used on macOS where the device doesn't exist. The host
// calls these functions instead of bare libc open/mmap/ioctl when compiled
// with SCHWUNG_SIM_BACKEND.
//
// SPI protocol constants (SCHWUNG_PAGE_SIZE, SCHWUNG_OFF_IN_BASE, etc.) are
// imported from schwung-spi's header — this file does not depend on the
// schwung-spi library at link time.

#ifndef SCHWUNG_HOST_SIM_BACKEND_H
#define SCHWUNG_HOST_SIM_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open the "device". Returns a real fd backed by an unlinked tempfile (so
// mmap/close work normally) but does not touch /dev/ablspi0.0. Returns -1
// on failure. Subsequent calls return the same fd until schwung_sim_close().
int schwung_sim_open(void);

// "mmap" the device. Returns the in-process shadow buffer (4096 bytes,
// aligned). The hw side is backed by calloc; both live until process exit.
// Must be called after schwung_sim_open(). NULL on failure.
uint8_t *schwung_sim_mmap(void);

// One SPI frame: shadow→hw[TX] memcpy → block on tick pipe → hw→shadow[RX]
// memcpy. The sim daemon (a separate thread in the same process) is
// expected to fill hw[RX] and pulse the tick fd between calls. Returns 0
// on success, -1 with errno on failure.
int schwung_sim_ioctl_wait(void);

// Tear down: close the fd, free hw. shadow buffer remains (static). Safe to
// call multiple times. Mostly useful for tests.
void schwung_sim_close(void);

// ============================================================================
// Sim Daemon API — used by whatever thread/process drives the mailbox
// ============================================================================

// Writable pointer to the hw buffer. The sim daemon writes the RX side
// (MIDI IN, display status, audio IN) here between barriers. Returns NULL
// before schwung_sim_mmap() has been called.
uint8_t *schwung_sim_get_hw_buffer(void);

// Write end of the tick pipe. The sim daemon writes 1 byte after refreshing
// hw[RX] — that pulse releases the host's blocked schwung_sim_ioctl_wait().
// Returns -1 before schwung_sim_open() or if pipe init failed.
int schwung_sim_get_tick_fd(void);

// ============================================================================
// Display SHM (macOS sim only)
// ============================================================================
//
// On device, the LD_PRELOAD shim assembles display chunks from the SPI mailbox
// and writes them to a POSIX SHM at /schwung-display-live (1024 bytes,
// 128×64 1-bit framebuffer). display-server reads that SHM and streams via SSE.
//
// In sim mode there is no shim, so schwung-host writes the SHM directly.
// Call from push_screen() right after packing the framebuffer.

// Lazily open + mmap /schwung-display-live (creates if absent). Copies the
// 1024-byte framebuffer into the SHM. Logged once on first failure;
// subsequent calls are silent no-ops if SHM init failed.
// Returns 0 on success, -1 on shm_open/mmap failure.
int schwung_sim_push_display(const uint8_t *frame_1024);

// ============================================================================
// MIDI IN queue (browser → host)
// ============================================================================
//
// The browser ships 3-byte MIDI messages over WebSocket; the WS thread pushes
// them here. The audio render callback drains the queue once per tick and
// writes the events into the mailbox MIDI_IN region (offset 2048) as 8-byte
// AblSpiMidiEvent slots with monotonically increasing timestamps. Cable is
// always 0 (internal Move hardware controls — same path real pads use on
// device).

// Push one MIDI message. `bytes` should be 1-3 bytes (status + up to 2 data).
// Returns 0 on success, -1 if the queue is full (drops the message).
int schwung_sim_push_midi_in(const uint8_t *bytes, size_t len);

// Drain the queue into the mailbox MIDI_IN region at offset 2048. Writes up to
// max 30 events per call (31st slot is reserved for the XMOS heartbeat).
// Returns the number of events written. Safe to call from the audio thread.
int schwung_sim_drain_midi_in_to_mailbox(uint8_t *mailbox);

#ifdef __cplusplus
}
#endif

#endif // SCHWUNG_HOST_SIM_BACKEND_H
