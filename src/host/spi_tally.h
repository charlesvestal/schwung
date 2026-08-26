/* spi_tally.h — SPI frame telemetry from the kernel's own counters.
 *
 * Two numbers ablspi already maintains, that nothing was reading:
 *
 *   spi_tx_time   `struct ablspi_sys_info` at the END of the mmap'd page
 *                 (PAGE_SIZE - 8). The driver stamps it after every transfer:
 *
 *                     s64 then = trace_clock_local();
 *                     ablspi_send_message(...);
 *                     wait_for_completion(&dma_transfer_finished);
 *                     sysinfo->spi_tx_time = trace_clock_local() - then;
 *
 *                 So it is the REAL wire+DMA time of the frame we just did, in
 *                 nanoseconds, for the cost of one aligned 8-byte load. The
 *                 shim already maps the whole page, and the post-transfer
 *                 hw→shadow memcpy already copies those bytes.
 *
 *   irq_count     /proc/ableton/ablspi0.0/irq_count — bumped by the GPIO ISR
 *                 on every XMOS frame, whether or not we serviced it.
 *
 * The difference between irq_count and our own frame count is the useful part,
 * and it needs one fact about the driver to read correctly: **ablspi's IRQ is a
 * counting semaphore, not a flag.** The ISR does `atomic_inc(&irq_arrived)`;
 * `ABLSPI_WAIT_AND_SEND_MESSAGE_WITH_SIZE` does `atomic_dec`. So when we
 * overrun our budget the frame is NOT dropped — it queues, and the next wait
 * returns immediately, replaying back-to-back. An overrun is therefore
 * invisible as a gap and shows up as a BURST, which is exactly the signature
 * that has been read as somebody else's producer misbehaving.
 *
 * `irqs - frames` over a window is that queue's change: the number of XMOS
 * frames that fired while we were still busy with an earlier one.
 *
 * This file is pure — no I/O, no allocation, no locks — so `spi_tally_record`
 * is safe on the SPI callback and the whole thing is host-testable
 * (tests/host/test_spi_tally.c). Reading /proc and logging live on the worker.
 */

#ifndef SPI_TALLY_H
#define SPI_TALLY_H

#include <stdint.h>

/* 128 frames at 44100 Hz = the SPI frame period Move runs at, in ns.
 * Used only to express how much of each period the transfer itself consumed;
 * it is nominal, not measured. */
#define SPI_TALLY_PERIOD_NS 2902494u

/* Written by the SPI callback, read by the worker. Single writer, aligned,
 * monotonic — so no atomics and no lock. Nothing here is ever reset by the
 * reader, which is what keeps the handoff race-free: the worker takes
 * DELTAS of cumulative counters rather than draining a window the producer is
 * still filling (which would lose whichever sample landed in between, and the
 * sample most worth keeping is the outlier). */
typedef struct {
    volatile uint64_t frames;      /* completed transfers observed */
    volatile uint64_t tx_ns_sum;   /* sum of spi_tx_time */
    volatile uint32_t tx_ns_max;   /* worst single transfer since arming */
    volatile uint32_t tx_ns_last;  /* most recent, for a spot check */
} spi_tally_t;

/* Worker-owned. Carries the previous sample so deltas can be taken. */
typedef struct {
    uint64_t prev_frames;
    uint64_t prev_tx_ns_sum;
    uint32_t prev_irqs;
    int      have_prev;
    uint64_t backlog;       /* cumulative late IRQs since arming */
    uint64_t backlog_peak;  /* worst single window */
} spi_tally_state_t;

/* One window's worth of answer. */
typedef struct {
    uint64_t frames;        /* frames this window */
    uint32_t irqs;          /* IRQs this window */
    uint32_t avg_ns;        /* mean transfer time this window */
    uint32_t max_ns;        /* worst transfer since arming */
    uint32_t headroom_ns;   /* SPI_TALLY_PERIOD_NS - avg_ns, floored at 0 */
    uint32_t late;          /* IRQs that arrived while busy, this window */
    uint64_t backlog;       /* cumulative late IRQs since arming */
    uint64_t backlog_peak;  /* worst single window */
    int      first;         /* 1 = no previous sample; deltas not meaningful */
} spi_tally_sample_t;

/* The shim's single instance: written by the SPI callback in
 * shim_post_transfer, read by the worker's ~1 Hz tick. It lives here rather
 * than in either of them so neither has to reach into the other's statics. */
extern spi_tally_t shim_spi_tally;

/* SPI-callback safe. Call once per completed transfer with the kernel's
 * spi_tx_time. A zero tx_ns is ignored — the driver leaves the field
 * untouched until the first transfer completes, and folding that zero in
 * would drag the mean down and pin the minimum at zero forever. */
void spi_tally_record(spi_tally_t *t, uint32_t tx_ns);

/* Drop both the accumulator and the window state. Called on the rising edge of
 * arming so a second session does not inherit the first one's peak. */
void spi_tally_reset(spi_tally_t *t, spi_tally_state_t *s);

/* Fold one worker sample. `irqs` is the raw /proc counter.
 *
 * That counter is printed from an `int` that only ever increments, so it goes
 * NEGATIVE past 2^31 (~72 days at the 344.5 Hz block rate) and wraps at 2^32.
 *
 * What makes the delta survive both is the WIDTH, not the signedness: the
 * subtraction has to happen in exactly 32 bits, matching the counter. Two's
 * complement makes signed and unsigned 32-bit subtraction produce the same
 * bits, so `(int32_t)a - (int32_t)b` is equally correct — but widen either
 * operand to 64 bits first and the arithmetic stops being modular, so the
 * wrap reads as a 4-billion-IRQ window instead of a 44-IRQ one. That is why
 * `prev_irqs` and `spi_tally_sample_t::irqs` are uint32_t and must stay that
 * way; widening them "for consistency with frames" is the regression, and
 * tests/host/test_spi_tally.c fails on exactly it. */
void spi_tally_fold(spi_tally_state_t *s, const spi_tally_t *t, uint32_t irqs,
                    spi_tally_sample_t *out);

/* Steady-state line. Returns the length written. */
int spi_tally_format(const spi_tally_sample_t *s, char *buf, int len);

/* Overrun line — only meaningful when s->late is nonzero. */
int spi_tally_format_late(const spi_tally_sample_t *s, char *buf, int len);

#endif /* SPI_TALLY_H */
