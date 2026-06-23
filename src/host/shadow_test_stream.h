/*
 * shadow_test_stream.h — Phase 2+ E2E test infrastructure: shim-side
 * publishers for the test-bus stream SHMs.
 *
 * The shim's role is to copy events of interest (MIDI_OUT, eventually
 * MIDI_IN / log / audio fingerprints) into per-channel SHM rings. The
 * test daemon (schwung-testd) subscribes by flipping each ring's
 * `enabled` flag and drains via DUMP.
 *
 * Cost when disabled (default state): one atomic load + branch per
 * frame per channel. When enabled: ~160 ns per frame for the MIDI_OUT
 * channel. Designed to stay safely under the SPI callback budget.
 *
 * Lives in a separate translation unit so each new channel (MIDI_IN,
 * log tail, etc.) lands here instead of growing schwung_shim.c. See
 * tools/pytest-schwung/README.md for the protocol and usage.
 */

#ifndef SHADOW_TEST_STREAM_H
#define SHADOW_TEST_STREAM_H

#include <stdint.h>

/* One-time setup: shm_open + mmap each channel's SHM segment. Called
 * from the shim's SHM init block alongside the other shadow segments.
 * Idempotent and safe to call when test-bus is not in use (the daemon
 * never connects; cost stays at one atomic load per frame). */
void shadow_test_stream_init(void);

/* Publish all real packets in the 80-byte MIDI_OUT mailbox region into
 * the test stream, tagged with `frame` (shim_counter at capture time).
 * `hw_midi_out` points to the start of the MIDI_OUT region (the first
 * 80 bytes of the hw mailbox; see SPI_PROTOCOL.md). */
void shadow_test_stream_publish_midi_out(const uint8_t *hw_midi_out,
                                         uint32_t frame);

#endif /* SHADOW_TEST_STREAM_H */
